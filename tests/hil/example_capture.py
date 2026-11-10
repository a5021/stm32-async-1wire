#!/usr/bin/env python3
"""Capture one logic-analyzer window per example and publish it for the viewer.

Run on the STM32F030 bench with the fx2lafw analyzer on D1 (PA2) and the
parasite-powered bus. `--mcu` selects another bench for a partial refresh; the
manifest keeps its global bench and records a per-app `bench` when it differs
(the 9_multi capture is taken on the F446 DS2413 bench). Every app entry also
records its own `commit` and `generated` date, so a partial refresh cannot
inherit the provenance of an older full run. The script builds and
flashes each example, captures a generous window, trims it to the first reset
plus the app's window, and writes a sigrok `.sr` archive the viewer can open
with `?chan=1`.

The output is committed, so this script is a bench tool, not a CI step. The
host-side gate that checks the committed archives and links is
`tests/hil/test_example_captures.py`.

Usage:
    python tests/hil/example_capture.py
    python tests/hil/example_capture.py --only 2_device_search --only 9_multi
    python tests/hil/example_capture.py --no-build --no-flash   # already flashed
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import zipfile

try:
    import numpy as np
except ImportError:
    print("example_capture: numpy is required", file=sys.stderr)
    raise SystemExit(1)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

import common  # noqa: E402
import dma_pressure as dp  # noqa: E402

BENCH = "STM32F030x6"
CHANNEL = 0          # PA10 is the 1-Wire bus; PA2/D1 is the optional marker
RATE = "4M"
RATE_HZ = 4_000_000
RATE_TEXT = "4 MHz"
DEFAULT_SIGROK = r"C:\tools\sigrok-cli\sigrok-cli.exe"
DEFAULT_SR_DIR = os.path.join(REPO, "docs", "examples", "captures")
DEFAULT_JSON = os.path.join(REPO, "docs", "examples", "captures.json")
RAW_DIR = os.path.join(REPO, "bench_logs", "hil", "examples")

#: `seconds` is the raw analyzer window; `window_us` is what is kept after the
#: first reset. The 5 s pause in the measurement examples is why several
#: windows are longer than one exchange.
APPS = [
    {
        "app": "0_dma_pressure",
        "seconds": 3.0,
        "window_us": 2_500_000,
        "allow_no_reset": True,
        "description": "Diagnostic 64-slot write pattern at stress level 0.",
    },
    {
        "app": "2_device_search",
        "seconds": 9.0,
        "window_us": 7_500_000,
        "description": "Startup Search ROM, then a Match ROM measurement.",
    },
    {
        "app": "3_round_robin",
        "seconds": 9.0,
        "window_us": 7_500_000,
        "description": "Search ROM and a round-robin measurement with resolution changes.",
    },
    {
        "app": "4_scan_mode",
        "seconds": 9.0,
        "window_us": 7_500_000,
        "description": "Broadcast Convert T followed by Match ROM read-back.",
    },
    {
        "app": "5_commands",
        "seconds": 10.0,
        "window_us": 8_500_000,
        "description": "ROM, power-supply, scratchpad and EEPROM commands.",
    },
    {
        "app": "6_statistics",
        "seconds": 8.0,
        "window_us": 6_500_000,
        "description": "Sequential measurements with statistics enabled.",
    },
    {
        "app": "7_low_power",
        "seconds": 9.0,
        "window_us": 7_500_000,
        "description": "Low-power build: the same search and measurement with WFE waits.",
    },
    {
        "app": "8_ds2431",
        "seconds": 7.0,
        "window_us": 5_500_000,
        "description": "DS2431 EEPROM write, copy and read-back.",
    },
    {
        "app": "9_multi",
        "seconds": 10.0,
        "window_us": 8_500_000,
        "description": "DS18B20 scan, DS2431 record update, and DS2413 PIOA/PIOB blink (D2=PIOA, D3=PIOB).",
    },
]

SKIPPED = {
    "1_basic": (
        "Not captured: this example assumes a single DS18B20 and Skip ROM; "
        "the reference bench has several devices, so Skip ROM would collide. "
        "Use 2_device_search for the multi-drop equivalent."
    ),
}


def first_reset_us(raw, rate_hz, channel):
    """First falling edge whose low run is a reset (>= 300 us)."""
    v = (raw >> channel) & 1
    d = np.diff(v.astype(np.int8))
    falls = np.flatnonzero(d < 0) + 1
    rises = np.flatnonzero(d > 0) + 1
    j = 0
    for fall in falls:
        while j < len(rises) and rises[j] <= fall:
            j += 1
        if j < len(rises):
            width = (rises[j] - fall) / rate_hz * 1e6
            if width >= 300.0:
                return fall / rate_hz * 1e6
    return None


def first_falling_us(raw, rate_hz, channel):
    """First falling edge on the channel, used by patterns without a reset."""
    v = (raw >> channel) & 1
    d = np.diff(v.astype(np.int8))
    falls = np.flatnonzero(d < 0)
    return (falls[0] + 1) / rate_hz * 1e6 if len(falls) else None


def write_sr(raw, rate_hz, dst, start_us, end_us):
    """Write one trimmed window as a sigrok `.sr` archive."""
    start = max(0, int(round(start_us * 1e-6 * rate_hz)))
    end = min(len(raw), int(round(end_us * 1e-6 * rate_hz)))
    if end - start < 16:
        raise RuntimeError("capture window is empty")
    meta = (
        "[global]\n"
        "sigrok version=0.6.0-example-capture\n\n"
        "[device 1]\n"
        "capturefile=logic-1\n"
        "total probes=8\n"
        "samplerate=%s\n"
        "total analog=0\n"
        "probe1=D0\n"
        "probe2=D1\n"
        "probe3=D2\n"
        "probe4=D3\n"
        "probe5=D4\n"
        "probe6=D5\n"
        "probe7=D6\n"
        "probe8=D7\n"
        "unitsize=1\n" % RATE_TEXT
    )
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("version", "2")
        z.writestr("metadata", meta)
        z.writestr("logic-1-1", np.asarray(raw[start:end], dtype=np.uint8).tobytes())


def git_head():
    try:
        proc = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"], cwd=REPO,
            capture_output=True, text=True, timeout=5)
        return proc.stdout.strip() if proc.returncode == 0 else "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def merge_manifest(existing, updates, meta):
    """Merge selected app entries into an existing manifest.

    A full run starts from an empty dict; `--only` passes the loaded manifest
    so the other examples and their skip reasons survive a partial refresh.
    """
    out = dict(existing or {})
    out.update(meta)
    apps = dict(out.get("apps") or {})
    for name, entry in updates.items():
        entry = dict(entry)
        if entry.get("bench") == out.get("bench"):
            entry.pop("bench", None)
        apps[name] = entry
    out["apps"] = apps
    return out


def capture_one(entry, args):
    app = entry["app"]
    raw_path = os.path.join(RAW_DIR, app + ".bin")
    os.makedirs(RAW_DIR, exist_ok=True)
    os.makedirs(args.out_dir, exist_ok=True)

    if not args.no_build:
        hexpath = common.build_hex(app)
        print("  built %s" % hexpath, flush=True)
        if not args.no_flash:
            common.flash(hexpath)
            print("  flashed %s" % app, flush=True)

    raw = None
    first = None
    capture_seconds = entry["seconds"]
    start = 0.0
    for attempt in (1, 2):
        capture_seconds = entry["seconds"] + (attempt - 1) * 2.0
        samples = dp.capture(args.sigrok, args.prog, raw_path, capture_seconds)
        if samples < int(RATE_HZ * capture_seconds * 0.5):
            if attempt == 2:
                raise RuntimeError("short capture for %s (%d bytes)" % (app, samples))
            print("  retrying short capture for %s" % app, flush=True)
            continue
        raw = np.fromfile(raw_path, dtype=np.uint8)
        first = first_reset_us(raw, RATE_HZ, CHANNEL)
        if first is None and entry.get("allow_no_reset"):
            first = first_falling_us(raw, RATE_HZ, CHANNEL)
        if first is None:
            if attempt == 2:
                raise RuntimeError("no reset found on D%d in %s" % (CHANNEL, app))
            print("  retrying %s: no reset in the window" % app, flush=True)
            continue
        start = max(0.0, first - 500.0)
        available_us = len(raw) / RATE_HZ * 1e6
        need_us = start + entry["window_us"]
        if available_us >= need_us:
            break
        if attempt == 2:
            raise RuntimeError(
                "capture for %s is too short: need %.0f us after the reset, "
                "have %.0f us" % (app, need_us, available_us))
        print("  retrying %s: window needs %.0f us, have %.0f us" % (
            app, need_us, available_us), flush=True)

    if raw is None or first is None:
        raise RuntimeError("capture for %s produced no data" % app)
    end = start + entry["window_us"]
    dst = os.path.join(args.out_dir, app + ".sr")
    write_sr(raw, RATE_HZ, dst, start, end)
    print("  %s: reset at %.1f us, kept %.1f ms -> %s" % (
        app, first, (end - start) / 1000.0, os.path.relpath(dst, REPO)), flush=True)
    return {
        "file": os.path.relpath(dst, os.path.dirname(args.json)).replace(os.sep, "/"),
        "seconds": capture_seconds,
        "window_us": int(round(end - start)),
        "description": entry["description"],
        "bench": args.mcu,
        # Provenance per capture: a partial refresh must not inherit the
        # manifest-wide commit/date of an unrelated earlier full run.
        "commit": git_head(),
        "generated": time.strftime("%Y-%m-%d"),
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mcu", default=BENCH)
    ap.add_argument("--sigrok", default=os.environ.get("SIGROK_CLI", DEFAULT_SIGROK))
    ap.add_argument("--prog", default="")
    ap.add_argument("--out-dir", default=DEFAULT_SR_DIR)
    ap.add_argument("--json", default=DEFAULT_JSON)
    ap.add_argument("--only", action="append", default=None,
                    help="capture only this app (repeatable)")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--no-flash", action="store_true")
    args = ap.parse_args(argv)

    common.select_bench(args.mcu)
    args.prog = args.prog or common.find_programmer()
    if not args.prog:
        print("example_capture: STM32CubeProgrammer CLI not found", file=sys.stderr)
        return 1
    if not dp.la_present(args.sigrok):
        print("example_capture: fx2lafw analyzer not found (SIGROK_CLI=%s)"
              % args.sigrok, file=sys.stderr)
        return 1

    selected = [e for e in APPS if not args.only or e["app"] in args.only]
    if args.only:
        known = {e["app"] for e in APPS}
        missing = sorted(set(args.only) - known)
        if missing:
            print("example_capture: unknown app(s): %s" % ", ".join(missing),
                  file=sys.stderr)
            return 1

    manifest_meta = {
        "bench": args.mcu,
        "channel": CHANNEL,
        "capture_rate_hz": RATE_HZ,
        "generated": time.strftime("%Y-%m-%d"),
        "commit": git_head(),
    }
    existing = {}
    if args.only and os.path.isfile(args.json):
        try:
            with open(args.json, encoding="utf-8") as fh:
                existing = json.load(fh)
        except (OSError, ValueError) as exc:
            print("example_capture: cannot read %s: %s" % (args.json, exc),
                  file=sys.stderr)
            return 1
        if existing.get("bench"):
            manifest_meta["bench"] = existing["bench"]
    manifest = merge_manifest(existing, {}, manifest_meta)
    if not args.only:
        manifest["apps"] = {}
    for app, reason in SKIPPED.items():
        manifest["apps"].setdefault(app, {"skip": reason})

    print("capturing %d example(s) on %s, D%d" % (len(selected), args.mcu, CHANNEL))
    for entry in selected:
        print("== %s ==" % entry["app"], flush=True)
        manifest["apps"][entry["app"]] = capture_one(entry, args)

    os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
    with open(args.json, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(manifest, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print("manifest: %s" % os.path.relpath(args.json, REPO))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
