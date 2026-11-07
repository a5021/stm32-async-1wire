#!/usr/bin/env python3
"""Measure the CPU cycles spent inside the non-blocking 1-Wire driver.

Build the example with `-DAPP_PROFILE=1`, flash it, then read the UART for a
configurable window. The example prints one `PROFILE` line per search and per
measurement cycle; this script turns those lines into average/worst CPU cycles,
cycles per poll, and the CPU share of the active and full cycle.

This is a bench tool, not a CI step. It needs the board and its UART.

Usage:
    python tests/hil/cpu_profile.py --app 2_device_search --mcu STM32H503xx \
        --sysclk 250 --hse 8 --seconds 300 --port COM5
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

import common  # noqa: E402

PROFILE_RE = re.compile(
    r"^PROFILE\s+(?P<kind>cal|s|m)\s+"
    r"(?:o=(?P<overhead>\d+)|c=(?P<cycles>\d+)\s+p=(?P<events>\d+)\s+"
    r"w=(?P<worst>\d+)(?:\s+a=(?P<active>\d+)\s+t=(?P<cycle>\d+))?)"
)


def build(app, mcu, sysclk, hse, ext):
    cmd = ["make", "APP=%s" % app, "OW_MCU=%s" % mcu,
           "SYSCLK_MHZ=%d" % sysclk]
    if hse:
        cmd.append("HSE_MHZ=%d" % hse)
    cmd.append("EXT=%s" % ext)
    proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError("make failed (%d):\n%s\n%s"
                           % (proc.returncode, proc.stdout, proc.stderr))
    hexpath = os.path.join(REPO, "build", "ds18b20_%s.hex" % app)
    if not os.path.isfile(hexpath):
        raise RuntimeError("build did not produce %s" % hexpath)
    return hexpath


def parse_line(text):
    m = PROFILE_RE.match(text)
    if not m:
        return None
    kind = m.group("kind")
    if kind == "cal":
        return {"kind": "cal", "overhead": int(m.group("overhead"))}
    return {
        "kind": kind,
        "cycles": int(m.group("cycles")),
        "events": int(m.group("events")),
        "worst": int(m.group("worst")),
        "active_ms": int(m.group("active")) if m.group("active") else 0,
        "cycle_ms": int(m.group("cycle")) if m.group("cycle") else 0,
    }


def p95(values):
    if not values:
        return 0
    ordered = sorted(values)
    return ordered[max(0, int(round(0.95 * len(ordered))) - 1)]


def load_percent(cycles, ms, sysclk):
    if ms <= 0:
        return 0.0
    return 100.0 * cycles / (ms * sysclk * 1000.0)


def summarize(samples, sysclk):
    if not samples:
        return None
    cycles = [s["cycles"] for s in samples]
    events = [s["events"] for s in samples]
    worst = [s["worst"] for s in samples]
    active = [s["active_ms"] for s in samples]
    full = [s["cycle_ms"] for s in samples]
    total_cycles = sum(cycles)
    total_active = sum(active)
    total_full = sum(full)
    return {
        "count": len(samples),
        "cycles_avg": total_cycles / len(samples),
        "cycles_min": min(cycles),
        "cycles_max": max(cycles),
        "cycles_p95": p95(cycles),
        "events_avg": sum(events) / len(samples),
        "worst_avg": sum(worst) / len(samples),
        "worst_max": max(worst),
        "active_ms_avg": total_active / len(samples),
        "cycle_ms_avg": total_full / len(samples),
        "cycles_per_event": total_cycles / max(1, sum(events)),
        "load_active_pct": load_percent(total_cycles, total_active, sysclk),
        "load_full_pct": load_percent(total_cycles, total_full, sysclk),
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", default="2_device_search")
    ap.add_argument("--mcu", default="STM32H503xx")
    ap.add_argument("--sysclk", type=int, default=250)
    ap.add_argument("--hse", type=int, default=8)
    ap.add_argument("--seconds", type=int, default=300)
    ap.add_argument("--port", default=os.environ.get("HIL_PORT", ""))
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--json", default="")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--no-flash", action="store_true")
    args = ap.parse_args(argv)

    common.select_bench(args.mcu)
    ext = common.build_ext(args.app)
    if "APP_PROFILE" not in ext:
        ext = (ext + " -DAPP_PROFILE=1").strip()
    if "OW_PROFILE" not in ext:
        ext = (ext + " -DOW_PROFILE=1").strip()

    if not args.no_build:
        hexpath = build(args.app, args.mcu, args.sysclk, args.hse, ext)
        print("built %s" % os.path.relpath(hexpath, REPO), flush=True)
        if not args.no_flash:
            common.flash(hexpath)
            print("flashed %s" % args.app, flush=True)

    port = args.port
    if not port:
        port, tried = common.find_port()
        if not port:
            print("cpu_profile: no example UART found (tried %s); pass --port"
                  % ", ".join(tried), file=sys.stderr)
            return 1
    print("reading %s at %d baud for %d s" % (port, args.baud, args.seconds),
          flush=True)

    uart = common.open_uart(port, args.baud)
    cal = None
    search = []
    measure = []
    end = time.monotonic() + args.seconds
    next_report = time.monotonic() + 30.0
    try:
        while time.monotonic() < end:
            line = uart.readline()
            if line:
                text = line.decode("utf-8", "replace").strip()
                rec = parse_line(text)
                if rec:
                    if rec["kind"] == "cal":
                        cal = rec["overhead"]
                        print("calibration overhead: %d cycles" % cal, flush=True)
                    elif rec["kind"] == "s":
                        search.append(rec)
                        print("search: %d cycles, %d work events, worst %d"
                              % (rec["cycles"], rec["events"], rec["worst"]),
                              flush=True)
                    else:
                        measure.append(rec)
            now = time.monotonic()
            if now >= next_report:
                print("  %d s, %d measurement line(s)" % (
                    args.seconds - int(end - now), len(measure)), flush=True)
                next_report = now + 30.0
    finally:
        uart.close()

    summary = {
        "app": args.app,
        "mcu": args.mcu,
        "sysclk_mhz": args.sysclk,
        "seconds": args.seconds,
        "calibration_cycles": cal,
        "search": summarize(search, args.sysclk),
        "measure": summarize(measure, args.sysclk),
    }
    print(json.dumps(summary, indent=2, sort_keys=True))
    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(summary, fh, indent=2, sort_keys=True)
            fh.write("\n")
    if not measure:
        print("cpu_profile: no measurement lines; was the build made with "
              "-DAPP_PROFILE=1?", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
