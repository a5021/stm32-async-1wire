#!/usr/bin/env python3
"""Capture the 1-Wire line with a logic analyzer and check its geometry.

Run:
    python tests/hil/la_capture.py                    # 12 s @ 4 MHz on D0
    python tests/hil/la_capture.py --seconds 30 --rate 8M
    python tests/hil/la_capture.py --parasite         # expect the wider guard band
    python tests/hil/la_capture.py --wait-ms 750      # also check the conversion wait
    python tests/hil/la_capture.py --wait-ms 750 --wait-tol-ms 20   # a slow poll loop
    python tests/hil/la_capture.py --out bench_logs/hil/slot.bin

What it proves: the wire-level shape of the bus. The port's schedule is
ONE + ZERO + guard microseconds per bit slot, the master pulls the line low
for ONE on a '1' and for ZERO on a '0', and the reset pulse is
OW_PORT_RESET_PULSE_DURATION long. Those are the numbers a consumer's bus has
to live with, and a change to the port's schedule moves them - which is
exactly what this measures. The expectations are read from inc/ow_config.h, so
the check follows the configuration instead of restating it.

--wait-ms adds the one thing the host tests cannot prove: the *duration* of a
long wait. The engine and the host model both read the repetition counter as
"periods - 1" (the reference manual's convention), so a consistently wrong
convention is invisible in the host suite - both sides agree. On the wire it
is a gap of the wrong length, and that is measurable.

The tolerance is 2 ms because the gap carries a fixed overhead: the tail of the
last bit slot (at most 166 us) and the application's poll loop, which is
1 ms-granular in the examples. A firmware that polls more slowly needs
--wait-tol-ms; a tighter tolerance would only reject a correct plan.

The capture is the sigrok "binary" output: one byte per sample, with the
eight channels packed into it, D0 in the least significant bit. The unused
channels of an idle analyzer read HIGH, so a byte of 0xFF means "nothing
happened" on D0. Each capture writes a `<file>.meta` sidecar with the sample
rate, channel, duration, tool and SHA-256: the samples do not carry those, and
`la_decode.py` refuses a --rate or --channel that contradicts the sidecar
instead of silently rescaling the whole decode.

Requires sigrok-cli (set SIGROK_CLI to point at it) and numpy.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_SIGROK = r"C:\tools\sigrok-cli\sigrok-cli.exe"
TOLERANCE_US = 1.5  # edge quantisation at a few MHz
WAIT_TOLERANCE_US = 2000.0  # a long wait: pulse tail plus poll latency


def read_config():
    """The slot geometry the port is configured with, from inc/ow_config.h."""
    path = os.path.join(REPO, "inc", "ow_config.h")
    text = open(path, encoding="utf-8", errors="replace").read()

    def value(name):
        m = re.search(r"^#define\s+%s\s+(\d+)" % name, text, re.M)
        return int(m.group(1)) if m else None

    parasite = value("ONEWIRE_PARASITE_POWER")
    guard = value("ONEWIRE_GUARD_BAND")
    if parasite is None:
        parasite = 0
    if guard is None:
        guard = 0
    # The parasite build raises the guard band; the default is the narrow one.
    # Both spellings are in the header, so the effective value is whichever
    # branch the build takes - the caller says which.
    return {
        "one": value("ONEWIRE_ONE_PULSE"),
        "zero": value("ONEWIRE_ZERO_PULSE"),
        "guard_default": 5,
        "guard_parasite": 100,
    }


def parse_rate(text):
    """'4M' / '8k' / '4000000' -> Hz."""
    return float(str(text).upper().replace("M", "e6").replace("K", "e3"))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_meta(path, rate_hz, channel, seconds, tool):
    """Record what the samples cannot: the rate and channel they were taken at.

    A wrong --rate at decode time rescales every width, so the artifact and
    its parameters have to travel together. The hash ties the sidecar to the
    exact bytes; if the file is later replaced, the sidecar no longer
    describes it and the decoder refuses it rather than guessing.
    """
    meta = {
        "file": os.path.basename(path),
        "bytes": os.path.getsize(path),
        "sha256": sha256_file(path),
        "rate_hz": rate_hz,
        "channel": channel,
        "seconds": seconds,
        "captured": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "tool": tool,
    }
    meta_path = path + ".meta"
    with open(meta_path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(meta, fh, indent=2, sort_keys=True)
        fh.write("\n")
    return meta_path


def capture(out_path, seconds, rate, channel, sigrok):
    cmd = [
        sigrok, "-d", "fx2lafw",
        "-I", ",".join("D%d" % c for c in range(8)),
        "-c", "samplerate=%s" % rate,
        "--time", str(int(seconds * 1000)),
        "-O", "binary", "-o", out_path,
    ]
    print("захват: %s" % " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if not os.path.exists(out_path):
        sys.stderr.write((r.stdout or "") + (r.stderr or ""))
        raise SystemExit("захват не удался")
    print("файл: %s (%d байт)" % (out_path, os.path.getsize(out_path)))
    meta_path = write_meta(out_path, parse_rate(rate), channel, seconds, sigrok)
    print("метаданные: %s" % meta_path)
    return out_path


def analyse(path, rate_hz, channel):
    raw = np.fromfile(path, dtype=np.uint8)
    sig = np.unpackbits(raw[:, None], axis=1, bitorder="little")[:, channel]
    sig = sig.astype(np.int8)
    d = np.diff(sig)
    fall = np.flatnonzero(d < 0) + 1
    rise = np.flatnonzero(d > 0) + 1
    if len(fall) == 0 or len(rise) == 0:
        return None
    idx = np.clip(np.searchsorted(rise, fall), 0, len(rise) - 1)
    ok = rise[idx] > fall
    widths = (rise[idx][ok] - fall[ok]) / rate_hz * 1e6
    starts = fall[ok]
    gaps = np.diff(starts) / rate_hz * 1e6
    return widths, gaps


def wait_measurements(gaps, target_us, tol_us=WAIT_TOLERANCE_US):
    """Find the long wait in the inter-pulse gaps.

    Returns (measured, doubled): `measured` is the gap nearest the requested
    wait (None when the capture holds no gap at all), and `doubled` is the gap
    nearest twice the request, but only when it really sits at twice the
    request - a plan with one period too many lands exactly there.
    """
    if len(gaps) == 0:
        return None, None
    measured = float(gaps[np.argmin(np.abs(gaps - target_us))])
    double_gap = float(gaps[np.argmin(np.abs(gaps - 2.0 * target_us))])
    doubled = double_gap if abs(double_gap - 2.0 * target_us) <= tol_us else None
    return measured, doubled


def histogram(values, label, limit=8):
    if len(values) == 0:
        print("  %s: нет данных" % label)
        return
    u, c = np.unique(np.round(values, 1), return_counts=True)
    order = np.argsort(-c)[:limit]
    print("  %s: %s" % (label, ", ".join(
        "%.1f мкс x%d" % (u[i], c[i]) for i in order)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--rate", default="4M")
    ap.add_argument("--channel", type=int, default=0)
    ap.add_argument("--parasite", action="store_true",
                    help="the parasite build raises the guard band")
    ap.add_argument("--wait-ms", type=float, default=0.0,
                    help="expected long wait in ms (e.g. 750 for a 12-bit "
                         "conversion); 0 = don't check")
    ap.add_argument("--wait-tol-ms", type=float,
                    default=WAIT_TOLERANCE_US / 1000.0,
                    help="tolerance for --wait-ms in ms (default 2: the tail "
                         "of the last slot plus a 1 ms poll loop)")
    ap.add_argument("--out", default="")
    ap.add_argument("--sigrok", default=os.environ.get("SIGROK_CLI", DEFAULT_SIGROK))
    args = ap.parse_args()

    rate_hz = parse_rate(args.rate)

    out_path = args.out or os.path.join(
        REPO, "bench_logs", "hil", "la_d0_1wire.bin")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    capture(out_path, args.seconds, args.rate, args.channel, args.sigrok)

    res = analyse(out_path, rate_hz, args.channel)
    if res is None:
        print("шина была простаивающей весь захват - измерять нечего")
        return 1
    widths, gaps = res

    cfg = read_config()
    guard = cfg["guard_parasite"] if args.parasite else cfg["guard_default"]
    slot = cfg["one"] + cfg["zero"] + guard

    print("\n=== геометрия 1-Wire на проводе ===")
    histogram(widths, "ширины низких импульсов")
    histogram(gaps, "межимпульсные интервалы")

    print("\n=== сверка с конфигурацией ===")
    checks = []

    def check(name, measured, expected, tol=TOLERANCE_US):
        if measured is None:
            checks.append((name, None, expected, False))
            return
        good = abs(measured - expected) <= tol
        checks.append((name, measured, expected, good))

    def nearest(values, target):
        if len(values) == 0:
            return None
        return float(values[np.argmin(np.abs(values - target))])

    check("слот (ONE+zero+guard)", nearest(gaps, slot), slot)
    check("импульс '1' (ONE)", nearest(widths, cfg["one"]), cfg["one"])
    check("импульс '0' (ZERO)", nearest(widths, cfg["zero"]), cfg["zero"])
    check("сброс (OW_PORT_RESET_PULSE_DURATION)",
          nearest(widths, 480), 480)

    # ---- the duration of a long wait -------------------------------------
    #
    # A wait has no pulse of its own: the line simply stays high until the
    # timer fires. It is still on the wire, as the gap between the last
    # falling edge of the command that started it and the first falling edge
    # of the next transaction - so a wait planned one period short or long is
    # a gap of the wrong length.
    #
    # This is the one measurement the host suite cannot make: the engine and
    # the host model both read the repetition counter as "periods - 1", so a
    # consistently wrong convention passes every host test. Only the wire
    # knows.
    wait_target = args.wait_ms * 1000.0
    wait_tol = args.wait_tol_ms * 1000.0
    wait_doubled = None
    if args.wait_ms > 0.0:
        print("\n=== длительность длинного ожидания ===")
        if len(gaps):
            print("  самая длинная пауза: %.3f мс" % (float(gaps.max()) / 1000.0))
        wait_measured, wait_doubled = wait_measurements(gaps, wait_target, wait_tol)
        if wait_measured is not None:
            print("  отклонение от ожидания: %+.3f мс (допуск ±%.3f мс)"
                  % ((wait_measured - wait_target) / 1000.0, wait_tol / 1000.0))
        check("ожидание (--wait-ms)", wait_measured, wait_target, wait_tol)

    bad = 0
    for name, measured, expected, good in checks:
        if measured is None:
            print("  FAIL %-34s нет измерения (ожидалось %.1f мкс)"
                  % (name, expected))
            bad += 1
            continue
        mark = "ok  " if good else "FAIL"
        print("  %s %-34s измерено %7.2f мкс, ожидалось %6.1f мкс"
              % (mark, name, measured, expected))
        if not good:
            bad += 1

    # Negative control: a wait planned with one period too many lands at
    # exactly 2 x the target. It must fail the run even when a correct gap is
    # also present (a capture that spans a resolution change).
    if args.wait_ms > 0.0:
        if wait_doubled is not None:
            print("  FAIL %-34s пауза %.3f мс" % ("ожидание не удвоено",
                                                  wait_doubled / 1000.0))
            bad += 1
        else:
            print("  ok   %-34s" % "ожидание не удвоено")

    print("\n%s" % ("провод соответствует конфигурации"
                    if bad == 0 else "провод РАСХОДИТСЯ с конфигурацией"))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
