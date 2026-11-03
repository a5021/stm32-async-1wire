#!/usr/bin/env python3
"""HIL: capture-drain and deliberate-delay checks for 0_dma_pressure.

This is the companion to `dma_pressure.py`. It answers the two questions the
write-only matrix cannot:

  * capture drain under load: a DS18B20 Search ROM pass (the capture DMA,
    channel 4) is run once per boot at every stress level. The level-0
    (unloaded) pass is the baseline; a stressed pass must return the same
    device count and the same FNV-1a over the found ROMs.
  * deliberate delay: `STRESS_INJECT_US` holds the feed (write mode) or
    capture (search mode) DMA channel disabled for N microseconds, then
    restores it. The write harness must report a bad frame, and the search
    must stop matching the baseline. This proves the tests catch a late
    transfer, not only a wrong count or route.

One pass per boot avoids re-using a one-shot driver state machine and makes
each cell independent. The script writes `docs/stress/capture.json` for the
report generator.

Usage:
    python tests/hil/dma_capture.py                 # full section
    python tests/hil/dma_capture.py --json out.json
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

import numpy as np

import common
import dma_pressure as dp

CAPTURE_JSON = os.path.join(common.REPO, "docs", "stress", "capture.json")

#: Search ROM under each load; both the default guard and the measured floor.
CAPTURE_GUARDS = (5, 1)
#: Injected channel-disable windows, in microseconds (see STRESS_INJECT_US).
SEARCH_INJECT = (0, 200, 400, 600, 1000)
WRITE_INJECT = (0, 80, 120, 200)


def bench_setup(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", default=CAPTURE_JSON)
    ap.add_argument("--sr-dir", default=os.path.join(common.REPO, "docs",
                                                     "stress", "captures"),
                    help="base directory for the search/inject .sr captures")
    ap.add_argument("--sigrok", default=os.environ.get("SIGROK_CLI",
                    r"C:\tools\sigrok-cli\sigrok-cli.exe"))
    ap.add_argument("--keep-runs", action="store_true")
    args = ap.parse_args(argv)

    if not dp.la_present(args.sigrok):
        print("SKIP: no fx2lafw logic analyzer found (SIGROK_CLI=%s)" % args.sigrok)
        return None, args
    status, detail = common.probe()
    if status == "absent":
        print("SKIP: %s" % detail)
        return None, args
    if status == "error":
        print("FAIL: %s" % detail)
        return None, args
    requested = os.environ.get("HIL_MCU")
    detected = common.bench_from_probe(detail)
    if requested and detected != requested:
        print("FAIL: %s is attached but HIL_MCU=%s was requested"
              % (detected or "an unrecognised target", requested))
        return None, args
    if detected is None:
        print("FAIL: cannot identify the attached target (known: %s)"
              % ", ".join(common.BENCH_PROFILES))
        return None, args
    common.select_bench(detected)
    prog = common.find_programmer()
    if not prog:
        print("SKIP: STM32CubeProgrammer CLI not found")
        return None, args
    print("bench: %s" % detected)
    return (detected, prog, args.sigrok, args.sr_dir), args


def first_reset_us(raw, rate_hz):
    """First falling edge whose low run is a reset (>= 300 us)."""
    falls = np.flatnonzero((raw & 1) == 0)
    rises = np.flatnonzero((raw & 1) == 1)
    j = 0
    for fall in falls:
        while j < len(rises) and rises[j] <= fall:
            j += 1
        if j < len(rises):
            width = (rises[j] - fall) / rate_hz * 1e6
            if width >= 300.0:
                return fall / rate_hz * 1e6
    return None


class Rig:
    def __init__(self, mcu, prog, sigrok, json_path, keep_all, sr_dir):
        self.mcu = mcu
        self.prog = prog
        self.sigrok = sigrok
        self.json_path = json_path
        self.keep_all = keep_all
        self.sr_dir = sr_dir
        self.raw = os.path.join(common.REPO, "bench_logs", "hil",
                                "dma_pressure", "_capture.bin")

    def run(self, mode, level, guard, clock, inject_us=0):
        print("  %s level=%d guard=%d clock=%s inject=%d" % (
            mode, level, guard, clock if clock else "default", inject_us),
            flush=True)
        extra = ""
        if mode == "search":
            extra += " -DSTRESS_CAPTURE=1"
        if inject_us:
            extra += " -DSTRESS_INJECT_US=%d" % inject_us
        dp.build(level, guard, clock, extra=extra)
        dp.flash_halt(self.prog)
        dp.capture(self.sigrok, self.prog, self.raw,
                   1.2 if mode == "search" else dp.run_seconds(guard))
        tel = dp.telemetry(self.prog) or [None] * 20
        res = {
            "mode": mode,
            "level": level,
            "guard_us": guard,
            "clock_mhz": clock,
            "inject_us": inject_us,
            "telemetry": tel,
        }
        if mode == "search":
            res.update({
                "count": tel[11],
                "rom_crc": tel[10],
                "sink_count": tel[14],
                "stop_timeout": tel[16],
                "done": tel[15] == 5,
                "completed": (tel[13] == 1 and tel[15] == 5 and tel[16] == 0),
            })
            raw = np.fromfile(self.raw, dtype=np.uint8)
            first_us = first_reset_us(raw, dp.RATE_HZ)
            if self.sr_dir and first_us is not None:
                name = "search/l%d_%s_g%d_i%d.sr" % (
                    level, "%dmhz" % clock if clock else "def", guard,
                    inject_us)
                out = os.path.join(self.sr_dir, name)
                os.makedirs(os.path.dirname(out), exist_ok=True)
                dp.write_sr(raw, dp.RATE_HZ, out, first_us - dp.SR_LEAD_US,
                            first_us + 300000.0)
                res["capture"] = name
        else:
            raw = np.fromfile(self.raw, dtype=np.uint8)
            slot_us = dp.ONE_US + dp.ZERO_US + guard + 1
            a = dp.analyse(raw, dp.RATE_HZ,
                           min_start_us=(dp.PRE_RESET_S + dp.SETTLE_MARGIN_S) * 1e6,
                           slot_us=slot_us)
            clean = bool(a["clean"] and tel[3] == 0 and tel[4] == 0
                         and tel[16] == 0)
            res.update({
                "clean": clean,
                "stop_timeout": tel[16],
                "frames": a["frames"],
                "first": a["first"],
                "capture": None,
            })
            if self.sr_dir and a["first_frame_us"] is not None:
                name = "inject/write_l%d_%s_g%d_i%d.sr" % (
                    level, "%dmhz" % clock if clock else "def", guard,
                    inject_us)
                out = os.path.join(self.sr_dir, name)
                os.makedirs(os.path.dirname(out), exist_ok=True)
                dp.write_sr(raw, dp.RATE_HZ, out, a["first_frame_us"] - dp.SR_LEAD_US,
                            a["first_frame_us"] + dp.SLOTS * slot_us + dp.SR_TAIL_US)
                res["capture"] = name
            elif self.sr_dir:
                falls = np.flatnonzero((raw & 1) == 0)
                if falls.size:
                    first_us = falls[0] / dp.RATE_HZ * 1e6
                    name = "inject/write_l%d_%s_g%d_i%d.sr" % (
                        level, "%dmhz" % clock if clock else "def", guard,
                        inject_us)
                    out = os.path.join(self.sr_dir, name)
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    dp.write_sr(raw, dp.RATE_HZ, out, first_us - dp.SR_LEAD_US,
                                first_us + dp.SLOTS * slot_us + dp.SR_TAIL_US)
                    res["capture"] = name
        if self.keep_all:
            tag = "%s_l%d_g%d_i%d.bin" % (mode, level, guard, inject_us)
            shutil.copyfile(self.raw, os.path.join(
                os.path.dirname(self.raw), tag))
        return res


def capture_matrix(rig, clocks, levels):
    cells = []
    baselines = {}
    for clock in clocks:
        for guard in CAPTURE_GUARDS:
            for level in levels:
                res = rig.run("search", level, guard, clock)
                cells.append(res)
                if level == 0 and guard == 5:
                    baselines[clock] = res
    supported = bool(baselines) and all(
        (b or {}).get("count", 0) for b in baselines.values())
    for res in cells:
        base = baselines.get(res["clock_mhz"])
        res["matches"] = bool(
            supported and base and res["completed"] and res["count"] > 0
            and res["count"] == base["count"]
            and res["rom_crc"] == base["rom_crc"])
    return cells, baselines, supported


def inject_matrix(rig, clocks):
    cells = []
    for clock in clocks:
        # Search-mode baseline (no injection) for this clock.
        search_base = rig.run("search", 0, 5, clock)
        for value in SEARCH_INJECT:
            if value == 0:
                res = dict(search_base)
            else:
                res = rig.run("search", 0, 5, clock, inject_us=value)
            res["matches"] = bool(
                res["completed"] and res["count"] > 0
                and res["count"] == search_base["count"]
                and res["rom_crc"] == search_base["rom_crc"])
            cells.append(res)
        for value in WRITE_INJECT:
            res = rig.run("write", 0, 5, clock, inject_us=value)
            cells.append(res)
    return cells


def first_bad(cells, mode, clock):
    for res in cells:
        if res["mode"] != mode or res["clock_mhz"] != clock:
            continue
        good = res.get("clean") if mode == "write" else res.get("matches")
        if not good:
            return res["inject_us"]
    return None


def main(argv=None):
    setup, args = bench_setup(argv)
    if setup is None:
        return 0 if args.json else 1
    mcu, prog, sigrok, sr_dir = setup
    f030 = mcu.startswith("STM32F0")
    clocks = [48, 8] if mcu == "STM32F030x6" else [None]
    levels = list(dp.LEVELS) if f030 else [0]
    if not f030:
        print("note: the stress generator is F0-only; measuring level 0")

    rig = Rig(mcu, prog, sigrok, args.json, args.keep_runs, sr_dir)
    print("capture drain: Search ROM under load")
    capture_cells, baselines, supported = capture_matrix(rig, clocks, levels)
    if not supported:
        print("note: no DS18B20 found by the unloaded baseline; capture section SKIP")
    for res in capture_cells:
        print("  %s L%d g%d c%s count=%s crc=%08X matches=%s" % (
            res["mode"], res["level"], res["guard_us"],
            res["clock_mhz"] if res["clock_mhz"] else "def",
            res["count"], res["rom_crc"] or 0, res["matches"]))

    print("deliberate delay: channel disabled for N us")
    inject_cells = inject_matrix(rig, clocks)
    for res in inject_cells:
        good = res.get("clean") if res["mode"] == "write" else res.get("matches")
        print("  %s c%s inject=%d %s" % (
            res["mode"], res["clock_mhz"] if res["clock_mhz"] else "def",
            res["inject_us"], "clean" if good else "BAD"))

    data = {
        "schema": 1,
        "bench": mcu,
        "generated": time.strftime("%Y-%m-%d"),
        "commit": common._git("rev-parse", "--short", "HEAD") or None,
        "dirty": bool(common._git("status", "--porcelain")),
        "capture_supported": supported,
        "capture_baseline": {
            str(clock if clock is not None else "def"): {
                "count": base["count"], "rom_crc": base["rom_crc"],
            }
            for clock, base in baselines.items()
        },
        "capture_cells": [
            {k: res[k] for k in ("mode", "level", "guard_us", "clock_mhz",
                                 "count", "rom_crc", "sink_count", "done",
                                 "completed", "matches", "capture")}
            | {"stop_timeout": res.get("stop_timeout")}
            for res in capture_cells
        ],
        "inject_cells": [
            {k: res[k] for k in ("mode", "inject_us", "clock_mhz")}
            | {("clean" if res["mode"] == "write" else "matches"):
               res.get("clean" if res["mode"] == "write" else "matches")}
            | {"capture": res.get("capture"),
               "stop_timeout": res.get("stop_timeout")}
            for res in inject_cells
        ],
        "inject_values": {"search": list(SEARCH_INJECT), "write": list(WRITE_INJECT)},
        "first_bad": {
            "%s_%s" % (mode, clock if clock is not None else "def"):
            first_bad(inject_cells, mode, clock)
            for mode in ("search", "write") for clock in clocks
        },
    }
    out = os.path.abspath(args.json)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print("json: %s" % args.json)

    ok = True
    if supported and not all(c["matches"] for c in capture_cells):
        ok = False
        print("FAIL: a stressed Search ROM does not match the unloaded baseline")
    if not any(v is not None for v in data["first_bad"].values()):
        ok = False
        print("FAIL: no injected delay was detected - the harness is blind")
    print("PASS: capture drain and deliberate-delay control" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
