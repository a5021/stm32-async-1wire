"""Decode a 1-Wire Search ROM pass from a logic-analyzer capture.

The stress bench records D0 (bus) and D1 (marker) into ``logic-1-1`` of a
sigrok archive, one byte per 4 MHz sample. This module turns that byte stream
back into the wire-level Search ROM transaction: the reset/presence, the 0xF0
command, and the per-bit id/cmp reads plus the direction writes the DUT put on
the bus. It is a diagnostic for cell failures, not part of the firmware: a
failing search is usually invisible in the telemetry (count/CRC only), while
the wire still shows which bit the DUT misread.

A trace that ends right after a read pair means the capture stopped there; it
does not by itself prove the search engine aborted. The same shape appears
when the caller abandons the pass (for example, a load channel that failed to
restart) and the analyzer window closes. Compare the per-bit table with the
telemetry fields before drawing a conclusion.

Usage:
    python tests/hil/search_trace.py docs/.../l3_8mhz_g5_i0.sr
    python tests/hil/search_trace.py capture.bin --rate 4000000

The module also exposes ``decode()`` for one-off analysis scripts.
"""

import argparse
import zipfile

import numpy as np

#: The bench samples D0..D7 at 4 MHz; the search captures keep that rate.
RATE_HZ = 4_000_000
#: D0 is the 1-Wire bus, D1 the marker the HIL firmware drives around a run.
BUS_BIT = 0
MARKER_BIT = 1
#: ``onewire_bit_from_pulse()`` calls a low pulse <= this a '1'.
SHORT_PULSE_MAX = 10.0
#: A command/direction write below this is a '0' slot in ``main.c`` terms.
WRITE_ZERO_MIN = 30.0


def load(path):
    """Return the packed sample bytes from a .sr archive or a raw .bin file."""
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            return np.frombuffer(z.read("logic-1-1"), dtype=np.uint8)
    return np.fromfile(path, dtype=np.uint8)


def runs(raw, rate_hz=RATE_HZ, bit=BUS_BIT):
    """Return (value, start_us, duration_us) for every bus run."""
    level = ((raw >> bit) & 1).astype(np.int8)
    edges = np.flatnonzero(np.diff(level)) + 1
    starts = np.r_[0, edges]
    ends = np.r_[edges, len(level)]
    return [(int(level[s]), s / rate_hz * 1e6, (e - s) / rate_hz * 1e6)
            for s, e in zip(starts, ends)]


def _slot(run_list, i):
    """Read one master slot at ``i``: low pulse plus the high that follows."""
    if i + 1 >= len(run_list) or run_list[i][0] != 0:
        return None
    low = run_list[i]
    high = run_list[i + 1] if run_list[i + 1][0] == 1 else None
    return low, high


def decode(raw, rate_hz=RATE_HZ):
    """Decode one Search ROM pass.

    Returns a dict with ``command`` (list of 8 bits, LSB first), ``bits`` (each
    entry has ``bit``, ``id``, ``cmp``, the wire pulse widths, and the
    ``direction`` the DUT wrote after the pair, or None at the end of the
    trace), ``truncated_after_pair`` (the trace stops before a direction
    write - an abandoned pass looks the same), and the last pair.
    """
    rl = runs(raw, rate_hz)
    lows = [i for i, r in enumerate(rl) if r[0] == 0]
    reset = next((i for i in lows if rl[i][2] >= 300.0), None)
    if reset is None:
        return {"error": "no reset pulse"}
    # The first low after the reset is the presence pulse; the next one is the
    # first command bit.
    presence = next(i for i in lows if i > reset)
    after_presence = [i for i in lows if i > presence]
    if not after_presence:
        return {"error": "no command after presence"}
    i = after_presence[0]

    command = []
    for _ in range(8):
        low, high = _slot(rl, i)
        if low is None or high is None:
            return {"error": "truncated command", "command": command}
        command.append(0 if low[2] > WRITE_ZERO_MIN else 1)
        i += 2

    def read_bit(slot):
        # A device '0' holds the line low through the slot; a '1' releases it
        # after the 5 us read pulse.
        if slot is None:
            return None
        low, high = slot
        return (0 if low[2] > SHORT_PULSE_MAX else 1,
                round(low[2], 2), round(high[2], 2))

    # Collect master slots: each is a low pulse plus the high that follows.
    # The protocol order is fixed - read id, read cmp, write direction, then
    # read id/cmp again - so the slot index, not the pulse width, says whether
    # a short pulse is a '1' read or a '1' direction write. A reset low ends
    # the path: the search restarts for the next ROM.
    slots = []
    while i < len(rl) - 1:
        low, high = _slot(rl, i)
        if low is None or high is None:
            break
        if low[2] >= 300.0:
            break
        slots.append((low, high))
        i += 2

    bits = []
    s = 0
    bit_no = 1
    # First pair: no direction write before it.
    if len(slots) < 2:
        return {"error": "truncated first pair", "command": command}
    first = read_bit(slots[0])
    second = read_bit(slots[1])
    bits.append({"bit": bit_no, "id": first[0], "cmp": second[0],
                 "id_pulse": first[1:], "cmp_pulse": second[1:],
                 "direction": None})
    s = 2
    # After a pair the protocol expects a direction write, then the next
    # id/cmp pair. The slot position, not the pulse width, decides the phase.
    while s < len(slots):
        low, _high = slots[s]
        bits[-1]["direction"] = 0 if low[2] > WRITE_ZERO_MIN else 1
        s += 1
        if s + 1 >= len(slots):
            # No complete pair after this direction. A single remaining slot
            # is the first half of the next pair; keep it as truncated
            # evidence instead of treating it as another direction.
            if s < len(slots):
                first = read_bit(slots[s])
                bit_no += 1
                bits.append({"bit": bit_no, "id": first[0], "cmp": None,
                             "id_pulse": first[1:], "cmp_pulse": None,
                             "direction": None})
            break
        first = read_bit(slots[s])
        second = read_bit(slots[s + 1])
        bit_no += 1
        bits.append({"bit": bit_no, "id": first[0], "cmp": second[0],
                     "id_pulse": first[1:], "cmp_pulse": second[1:],
                     "direction": None})
        s += 2

    last = bits[-1] if bits else None
    truncated_after_pair = bool(last and last["direction"] is None)
    # At a bit where the devices agree (id == cmp) the DUT must follow that
    # value: writing the opposite direction means its captured pair differs
    # from the wire. At a disagreement (id != cmp) either branch is legal, so
    # only the agreeing case is a contradiction.
    contradiction = None
    for b in bits:
        if b["direction"] is not None and b["id"] == b["cmp"] \
                and b["direction"] != b["id"]:
            contradiction = b
            break
    return {
        "command": command,
        "command_ok": command == [0, 0, 0, 0, 1, 1, 1, 1],
        "bits": bits,
        "truncated_after_pair": truncated_after_pair,
        "last": last,
        "contradiction": contradiction,
        "reset_us": round(rl[reset][2], 2),
        "presence_us": round(rl[presence][2], 2),
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("capture", help=".sr archive or raw .bin")
    ap.add_argument("--rate", type=float, default=RATE_HZ)
    ap.add_argument("--bits", action="store_true",
                    help="print the per-bit id/cmp/direction table")
    args = ap.parse_args(argv)
    result = decode(load(args.capture), args.rate)
    if "error" in result:
        print("error: %s" % result["error"])
        return 1
    print("command:", result["command"], "ok=%s" % result["command_ok"])
    print("reset %.1f us, presence %.1f us, bits %d"
          % (result["reset_us"], result["presence_us"], len(result["bits"])))
    if args.bits:
        for b in result["bits"]:
            print("  bit %2d id=%d cmp=%d dir=%s id_pulse=%s cmp_pulse=%s"
                  % (b["bit"], b["id"], b["cmp"], b["direction"],
                     b["id_pulse"], b["cmp_pulse"]))
    last = result["last"]
    if last is not None:
        print("last bit %d: wire id=%d cmp=%d dir=%s"
              % (last["bit"], last["id"], last["cmp"], last["direction"]))
    bad = result["contradiction"]
    if bad is not None:
        print("CONTRADICTION at bit %d: wire id=%d cmp=%d but DUT wrote dir=%d "
              "(id_pulse=%s cmp_pulse=%s)"
              % (bad["bit"], bad["id"], bad["cmp"], bad["direction"],
                 bad["id_pulse"], bad["cmp_pulse"]))
    print("trace ends after a read pair (not proof of an engine abort):",
          result["truncated_after_pair"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
