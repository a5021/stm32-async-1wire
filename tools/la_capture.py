#!/usr/bin/env python3
"""Capture the 1-Wire bus (and the other LA channels) around an MCU reset.

Starts sigrok-cli on the fx2lafw 8-channel logic analyzer, resets the board over
SWD ~0.8 s later, and lets the capture run for --seconds.  Writes a .sr archive
that sigrok-cli / PulseView can decode, and prints per-channel edge counts so
the DQ line can be identified when the wiring is not known in advance.

Usage:
    python tools/la_capture.py --seconds 6 --output tools/la_capture.sr
"""

import argparse
import subprocess
import sys
import time
import zipfile
from collections import Counter

import numpy as np

SIGROK = r"C:\tools\sigrok-cli\sigrok-cli.exe"
PROGRAMMER = (
    r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer"
    r"\bin\STM32_Programmer_CLI.exe"
)

CHANNELS = ",".join("D%d" % i for i in range(8))


def count_edges(path: str) -> None:
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        logic = next(n for n in names if n.startswith("logic-1"))
        raw = z.read(logic)
    data = np.frombuffer(raw, dtype=np.uint8)
    print("samples: %d" % data.size)
    for ch in range(8):
        bits = (data >> ch) & 1
        edges = int(np.count_nonzero(bits[1:] != bits[:-1]))
        ones = int(np.count_nonzero(bits))
        print("  D%d: %9d edges, %9d high" % (ch, edges, ones))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--samplerate", default="2m")
    ap.add_argument("--output", default=r"C:\Users\mr.smith\stm32-async-1wire\tools\la_capture.sr")
    ap.add_argument("--no-reset", action="store_true")
    args = ap.parse_args()

    cmd = [
        SIGROK, "-d", "fx2lafw",
        "-c", "samplerate=%s" % args.samplerate,
        "--time", "%d" % int(args.seconds * 1000.0),
        "--channels", CHANNELS,
        "-o", args.output,
    ]
    print("capture: %s" % " ".join(cmd), file=sys.stderr)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)

    if not args.no_reset:
        time.sleep(0.8)
        rst = subprocess.run([PROGRAMMER, "-c", "port=SWD", "-rst"],
                             capture_output=True, text=True)
        ok = "Software reset is performed" in rst.stdout
        print("SWD reset: %s" % ("ok" if ok else "FAILED"), file=sys.stderr)
        if not ok:
            print(rst.stdout[-600:] + rst.stderr[-300:], file=sys.stderr)

    out, err = proc.communicate()
    if out.strip():
        print(out.strip(), file=sys.stderr)
    if err.strip():
        print(err.strip(), file=sys.stderr)
    if proc.returncode != 0:
        print("sigrok-cli failed with %d" % proc.returncode, file=sys.stderr)
        return proc.returncode

    count_edges(args.output)
    print("saved: %s" % args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
