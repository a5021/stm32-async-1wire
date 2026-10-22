#!/usr/bin/env python3
"""Host-side tests for the wire decoder - no bench needed.

Run: python tests/hil/test_la_decode.py   (make test-hil-parsing)

The decoder is the only check that reads the *bytes* on the wire, so it is
also the one that can quietly lie: a decoder that guesses a bit in the gap
between '1' and '0', or skips a command it does not understand, turns a red
capture green. The cases below feed it exchanges whose bytes are known in
advance, and the negative controls are the point: a wrong bit must fail the
CRC, a shifted frame must be an error, an unknown command must be refused.
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import la_capture  # noqa: E402
import la_decode  # noqa: E402

ONE = 5.0
ZERO = 60.0
READ_ZERO = 29.0
PRESENCE = 128.8
RESET = 481.8
SLOT = 166.0

ROM_A = bytes.fromhex("28 78 B8 AC 0B 00 00 2C")
ROM_B = bytes.fromhex("28 20 78 92 07 00 00 67")
SCRATCHPAD = bytes.fromhex("72 01 4B 46 7F FF 0E 10 57")  # 23.125 C, CRC 0x57

failures = []


def check(what, condition):
    if condition:
        print("  ok   %s" % what)
    else:
        print("  FAIL %s" % what)
        failures.append(what)


def slots(data, kind="w"):
    """One 8-slot byte, LSB first. A read-0 is the device's short hold."""
    return [("slot", (b >> k) & 1, kind) for b in data for k in range(8)]


def exchange(*parts):
    return [("reset",), ("presence",)] + [p for part in parts for p in part]


def build(transactions, idle_us=1000.0):
    """Pulse widths and falling-edge gaps for a sequence of transactions."""
    widths, starts = [], []
    t = 0.0
    for tx in transactions:
        for spec in tx:
            if spec[0] == "reset":
                widths.append(RESET)
                starts.append(t)
                t += RESET + 29.0  # the presence delay
            elif spec[0] == "presence":
                widths.append(PRESENCE)
                starts.append(t)
                t += PRESENCE + 355.0  # release before the first slot
            else:
                _, bit, kind = spec
                width = ONE if bit else (ZERO if kind == "w" else READ_ZERO)
                widths.append(width)
                starts.append(t)
                t += SLOT
        t = starts[-1] + SLOT + idle_us
    gaps = np.diff(np.array(starts))
    return np.array(widths, dtype=float), gaps


def cfg():
    return la_capture.read_config()


def decode(transactions, idle_us=1000.0):
    widths, gaps = build(transactions, idle_us)
    return la_decode.decode(widths, gaps, cfg())


def search_pass(rom, collisions=()):
    """bit, complement, direction - the master writes the bit it chose."""
    out = []
    for k in range(64):
        bit = (rom[k // 8] >> (k % 8)) & 1
        if k in collisions:
            out += [("slot", 0, "r"), ("slot", 0, "r"), ("slot", bit, "w")]
        else:
            out += [("slot", bit, "r"), ("slot", 1 - bit, "r"),
                    ("slot", bit, "w")]
    return out


def test_a_full_exchange_decodes():
    print("a MATCH ROM + CONVERT, then a MATCH ROM + READ SCRATCHPAD")
    txns, orphan, dropped = decode([
        exchange(slots(b"\x55" + ROM_A), slots(b"\x44")),
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"), slots(SCRATCHPAD, "r")),
    ])
    check("two transactions", len(txns) == 2)
    check("no orphan slots, no artifacts", orphan == 0 and dropped == 0)
    match, convert = txns[0].frames
    check("the ROM is rebuilt exactly", match.rom == ROM_A)
    check("the ROM CRC is checked", match.crc_ok is True)
    check("CONVERT has no payload", convert.command == 0x44)
    read = txns[1].frames[1]
    check("the scratchpad is rebuilt", read.payload == SCRATCHPAD)
    check("the scratchpad CRC is checked", read.crc_ok is True)
    check("the temperature is 23.125 C", abs(read.value - 23.125) < 1e-9)


def test_a_wrong_bit_fails_the_crc():
    print("a flipped bit in the scratchpad")
    broken = bytearray(SCRATCHPAD)
    broken[0] ^= 0x01
    txns, _, _ = decode([
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"), slots(broken, "r")),
    ])
    read = txns[0].frames[1]
    check("the bit was decoded as sent", read.payload[0] != SCRATCHPAD[0])
    check("the CRC says the frame is broken", read.crc_ok is False)
    check("failures() counts it", la_decode.failures(txns) == 1)


def test_a_shifted_frame_is_an_error():
    print("a frame shifted by one slot")
    widths, gaps = build([exchange(slots(b"\x55" + ROM_A), slots(b"\x44"))])
    # A leading slot before the reset does not shift the frame; shift the
    # frame itself by inserting a slot right after the presence pulse.
    widths = np.insert(widths, 2, ONE)
    gaps = np.insert(gaps, 2, SLOT)
    try:
        la_decode.decode(widths, gaps, cfg())
        check("the decoder refuses it", False)
    except la_decode.DecodeError as e:
        check("the decoder refuses it (%s)" % e, True)


def test_a_short_presence_is_an_error():
    print("a presence pulse too short")
    widths, gaps = build([exchange(slots(b"\xCC"), slots(b"\x44"))])
    widths[1] = 40.0
    try:
        la_decode.decode(widths, gaps, cfg())
        check("the decoder refuses it", False)
    except la_decode.DecodeError as e:
        check("the decoder refuses it (%s)" % e, True)


def test_an_unknown_command_is_an_error():
    print("a function command the decoder does not know")
    try:
        decode([exchange(slots(b"\x55" + ROM_A), slots(b"\x99"))])
        check("the decoder refuses it", False)
    except la_decode.DecodeError as e:
        check("the decoder refuses it (%s)" % e, True)


def test_a_search_pass_rebuilds_the_rom():
    print("a Search ROM pass")
    txns, _, _ = decode([exchange(slots(b"\xF0"), search_pass(ROM_A))])
    search = txns[0].frames[0]
    check("one pass", len(search.passes) == 1)
    check("the ROM is the written directions", search.passes[0].rom == ROM_A)
    check("its CRC is checked", search.passes[0].crc_ok is True)
    check("no collisions in a one-device pass", search.passes[0].collisions == 0)


def test_two_passes_with_a_collision():
    print("two passes, one collision")
    txns, _, _ = decode([
        exchange(slots(b"\xF0"), search_pass(ROM_A), search_pass(ROM_B, {0})),
    ])
    search = txns[0].frames[0]
    check("two passes", len(search.passes) == 2)
    check("first ROM", search.passes[0].rom == ROM_A)
    check("second ROM", search.passes[1].rom == ROM_B)
    check("the collision is reported", search.passes[1].collisions == 1)


def test_an_empty_pass_terminates_the_search():
    print("a pass in which no device answers")
    empty = [("slot", 1, "r"), ("slot", 1, "r")]
    txns, _, _ = decode([
        exchange(slots(b"\xF0"), search_pass(ROM_A), empty),
    ])
    search = txns[0].frames[0]
    check("only the real pass is recorded", len(search.passes) == 1)
    check("the empty terminator is marked", search.empty_terminator is True)


def test_a_direction_contradicting_the_pair_is_an_error():
    print("all devices say 0, the master writes 1")
    bad = search_pass(ROM_A)
    bad[2] = ("slot", 1, "w")  # ROM_A bit 0 is 0, so the pair is (0, 1)
    try:
        decode([exchange(slots(b"\xF0"), bad)])
        check("the decoder refuses it", False)
    except la_decode.DecodeError as e:
        check("the decoder refuses it (%s)" % e, True)


def test_a_capture_that_starts_mid_transaction():
    print("a capture that starts in the middle of an exchange")
    widths, gaps = build([
        exchange(slots(b"\x55" + ROM_A), slots(b"\x44")),
    ])
    # Drop the reset and presence: the leading slots are a partial exchange.
    widths, gaps = widths[2:], gaps[2:]
    txns, orphan, _ = la_decode.decode(widths, gaps, cfg())
    check("nothing is decoded from the partial exchange", len(txns) == 0)
    check("the partial slots are counted, not dropped", orphan == len(widths))


def test_an_idle_capture_has_nothing():
    print("an idle capture")
    txns, orphan, dropped = la_decode.decode(
        np.array([5.0, 60.0]), np.array([166.0]), cfg())
    check("no transactions", txns == [])
    check("the pulses are reported as orphans", orphan == 2)


def test_the_log_must_agree():
    print("the application's log")
    txns, _, _ = decode([
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"), slots(SCRATCHPAD, "r")),
    ])
    roms = {ROM_A}
    good = [(ROM_A, 23.1)]
    check("a matching log passes",
          la_decode.compare_log(txns, roms, good, 1) == 0)
    check("a wrong temperature fails",
          la_decode.compare_log(txns, roms, [(ROM_A, 22.0)], 1) > 0)
    check("a ROM missing from the wire's log fails",
          la_decode.compare_log(txns, set(), good, 1) > 0)
    check("a log that enumerates a different device fails",
          la_decode.compare_log(txns, roms, [(ROM_B, 23.1)], 1) > 0)
    check("a wrong census fails",
          la_decode.compare_log(txns, roms, good, 2) > 0)
    # The wire's 23.125 C rounds to the 23.1 C the application prints; the
    # comparison is an equality of the printed value, not a tolerance.
    check("23.125 C matches the printed 23.1",
          la_decode.tenths(23.125) == la_decode.tenths(23.1))


def test_the_reads_after_the_boot_are_paired():
    print("a capture that caught the flash")
    txns, _, _ = decode([
        # the previous image's round, then the boot of the logged one
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"),
                 slots(bytes.fromhex("88 01 19 0F 7F FF 10 10 BB"), "r")),
        exchange(slots(b"\xF0"), search_pass(ROM_A)),
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"),
                 slots(SCRATCHPAD, "r")),
    ])
    check("only the post-boot read is compared",
          la_decode.compare_log(txns, {ROM_A}, [(ROM_A, 23.1)], 1) == 0)


def test_the_search_set_is_the_log_set():
    print("a full search against the log")
    txns, _, _ = decode([
        exchange(slots(b"\xF0"), search_pass(ROM_A), search_pass(ROM_B)),
    ])
    roms = {ROM_A, ROM_B}
    readings = [(ROM_A, 23.1), (ROM_B, 23.0)]
    check("both ROMs agree with the log",
          la_decode.compare_log(txns, roms, readings, 2) == 0)
    check("a missing device in the log fails",
          la_decode.compare_log(txns, {ROM_A}, readings, 1) > 0)
    check("a wrong count fails",
          la_decode.compare_log(txns, roms, readings, 3) > 0)


def test_another_family_on_the_wire_is_fine():
    print("a DS2431 next to the sensors")
    ds2431 = bytes.fromhex("2D F8 47 60 26 00 00 69")
    txns, _, _ = decode([
        exchange(slots(b"\xF0"), search_pass(ROM_A), search_pass(ds2431)),
    ])
    # The example reports DS18B20s only; the 0x2D device is on the wire but
    # absent from the log by design, and that is not a failure.
    check("the other family does not fail the comparison",
          la_decode.compare_log(txns, {ROM_A}, [(ROM_A, 23.1)], 1) == 0)


def main():
    test_a_full_exchange_decodes()
    test_a_wrong_bit_fails_the_crc()
    test_a_shifted_frame_is_an_error()
    test_a_short_presence_is_an_error()
    test_an_unknown_command_is_an_error()
    test_a_search_pass_rebuilds_the_rom()
    test_two_passes_with_a_collision()
    test_an_empty_pass_terminates_the_search()
    test_a_direction_contradicting_the_pair_is_an_error()
    test_a_capture_that_starts_mid_transaction()
    test_an_idle_capture_has_nothing()
    test_the_log_must_agree()
    test_the_reads_after_the_boot_are_paired()
    test_the_search_set_is_the_log_set()
    test_another_family_on_the_wire_is_fine()
    print("")
    if failures:
        print("test_la_decode: %d FAIL" % len(failures))
        return 1
    print("test_la_decode: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
