#!/usr/bin/env python3
"""Capture the 1-Wire line with a logic analyzer and check its geometry.

Run:
    python tests/hil/la_capture.py                    # 12 s @ 4 MHz on D0
    python tests/hil/la_capture.py --seconds 30 --rate 8M
    python tests/hil/la_capture.py --parasite         # expect the wider guard band
    python tests/hil/la_capture.py --out bench_logs/hil/slot.bin

What it proves: the wire-level shape of the bus. The port's schedule is
ONE + ZERO + guard microseconds per bit slot, the master pulls the line low
for ONE on a '1' and for ZERO on a '0', and the reset pulse is
OW_PORT_RESET_PULSE_DURATION long. Those are the numbers a consumer's bus has
to live with, and a change to the port's schedule moves them - which is
exactly what this measures. The expectations are read from inc/ow_config.h, so
the check follows the configuration instead of restating it.

The capture is the sigrok "binary" output: one byte per sample, with the
eight channels packed into it, D0 in the least significant bit. The unused
channels of an idle analyzer read HIGH, so a byte of 0xFF means "nothing
happened" on D0.

Requires sigrok-cli (set SIGROK_CLI to point at it) and numpy.
"""

import argparse
import os
import re
import subprocess
import sys

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_SIGROK = r"C:\tools\sigrok-cli\sigrok-cli.exe"
TOLERANCE_US = 1.5  # edge quantisation at a few MHz


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
    ap.add_argument("--out", default="")
    ap.add_argument("--sigrok", default=os.environ.get("SIGROK_CLI", DEFAULT_SIGROK))
    args = ap.parse_args()

    rate_hz = float(args.rate.upper().replace("M", "e6").replace("K", "e3"))

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

    def check(name, measured, expected):
        if measured is None:
            checks.append((name, None, expected, False))
            return
        good = abs(measured - expected) <= TOLERANCE_US
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

    print("\n%s" % ("геометрия соответствует конфигурации"
                    if bad == 0 else "геометрия РАСХОДИТСЯ с конфигурацией"))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
