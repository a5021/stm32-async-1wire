#!/usr/bin/env python3
"""HIL matrix: measure the CCR3 feed margin under memory-subsystem load.

Builds `examples/0_dma_pressure` for each (stress level, guard band) cell,
flashes it, captures D0 around a reset with the fx2lafw logic analyzer, and
checks every low pulse: the width must be a '1' or a '0' slot, the 64-bit
pattern must decode, and the frame must have exactly 64 pulses. A clean cell
means the feed DMA delivered every CCR3 reload inside the guard window.

The guard band is bisected per level. The smallest guard with a clean capture
bounds the feed latency from above (the timer resolution is 1 us and the
smallest valid guard is 1, so a clean cell at guard 1 reads as "a 2 us window
was enough").

The script is standalone (it rebuilds between cells), so `run.py` hands the
`dma_pressure` script straight to `main()`. A missing logic analyzer or probe
is a SKIP, not a failure. Telemetry is read back over SWD when the symbol is
available: `cndtr_late` is the on-chip proof of complete starvation and
`cpu_passes` shows how hard the load actually hit the memory bus.

Usage:
    python tests/hil/dma_pressure.py
    python tests/hil/dma_pressure.py --levels 0,4,5 --guards 1,2,5 --clocks 8
    python tests/hil/dma_pressure.py --keep-runs      # save every capture
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile

import numpy as np

import common

APP = "0_dma_pressure"
HEX = os.path.join(common.REPO, "build", "ds18b20_0_dma_pressure.hex")
ELF = os.path.join(common.REPO, "build", "ds18b20_0_dma_pressure.elf")

PATTERN = bytes([0x00, 0xFF, 0xAA, 0x55, 0x0F, 0xF0, 0x3C, 0xC3])
SLOTS = 64
ONE_US = 5
ZERO_US = 60
TOL_US = 1.5
#: A frame boundary. The longest gap inside a frame is the high tail of a '1'
#: slot at the widest guard (165 - 5 = 160 us); the gap between frames is at
#: least 1 ms (the firmware waits two millisecond ticks).
FRAME_GAP_US = 400.0
#: The capture tail can contain a few stray samples after the last real edge.
TAIL_IGNORE_US = 100.0

RATE = "4M"
RATE_HZ = 4_000_000
#: A shared viewer link opens one frame, not the whole 100-frame run: a 1 ms
#: lead-in and tail keep the first falling edge and the release visible.
SR_RATE_TEXT = "4 MHz"
SR_LEAD_US = 1000.0
SR_TAIL_US = 1000.0
#: Time from starting the analyzer to resetting the target into the run, and
#: the extra margin before the firmware's 200 ms settle ends. The margin also
#: skips the bus-pin init glitch some boards produce just as the application
#: starts (a lone sub-microsecond pulse, separated from the first frame by a
#: gap far longer than a frame boundary).
PRE_RESET_S = 0.3
SETTLE_MARGIN_S = 0.30

#: Guard values tried by the bisection, ascending. 1 is the smallest guard the
#: timer can express (0 makes CC2 coincide with the update event and produces a
#: spurious end-of-frame pulse, so it is not an operating point).
GUARDS = (1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 100)
LEVELS = (0, 1, 2, 3, 4, 5)
F030_CLOCKS = (48, 8)
#: The F446 load rig is characterized at the part's 180MHz default. At 16MHz
#: level 5 needs guard 89, and at 8MHz the fixed 512-word CPU hammer plus
#: stream 1 starves the feed even at guard 100; use --clocks 16,8 to measure
#: that margin explicitly rather than have every default run fail.
F446_CLOCKS = (180,)
#: Benches whose example actually generates a load. The others compile it and
#: run level 0 with `stress=unsupported` in the banner.
STRESS_BENCHES = ("STM32F030x6", "STM32F446xx")
#: The example's build defaults; the harness never overrides them.
BURSTS = 100
STRESS_WORDS = 512

SOURCE = os.path.join(common.REPO, "examples", APP, "main.c")


def source_pattern():
    """The bit pattern the firmware transmits, read from its source.

    The harness must check the exact sequence the image sends; a constant
    copied here would be one more place to forget. Falls back to PATTERN when
    the table cannot be parsed, but a parsed table that disagrees is fatal.
    """
    try:
        with open(SOURCE, encoding="utf-8") as fh:
            text = fh.read()
    except OSError:
        return PATTERN
    m = re.search(r"pattern\[PATTERN_BYTES\]\s*=\s*\{([^}]*)\}", text)
    if not m:
        return PATTERN
    values = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})u?", m.group(1)))
    if values != PATTERN:
        raise SystemExit(
            "the example's pattern %s does not match the harness's %s"
            % (values.hex().upper(), PATTERN.hex().upper())
        )
    return values


# ---------------------------------------------------------------------------
#  Waveform analysis (no hardware: unit-tested by test_dma_pressure.py)
# ---------------------------------------------------------------------------


def analyse(raw, rate_hz, pattern=PATTERN, slots=SLOTS, min_start_us=0.0,
            one_us=ONE_US, zero_us=ZERO_US, tol_us=TOL_US,
            frame_gap_us=FRAME_GAP_US, tail_ignore_us=TAIL_IGNORE_US,
            slot_us=None):
    """Check a binary capture of D0 against the expected write pattern.

    `raw` is one byte per sample with D0 in bit 0 (the `-O binary` layout).
    `min_start_us` drops frames that began before the harness's own reset -
    the previous image is still in flight while the new one is flashed.
    `slot_us`, when given, also checks the slot period (the first period of a
    frame carries the software-arm offset and is skipped).

    Returns a dict with `clean`, the violation counters, the number of checked
    frames and a human-readable `first` violation.
    """
    raw = np.asarray(raw, dtype=np.uint8)
    if raw.size == 0:
        return _result(0, 0, 0, 0, 0, False, "empty capture", 0.0, 0.0)
    bits = raw & 1
    change = np.flatnonzero(bits[1:] != bits[:-1]) + 1
    tail = int(tail_ignore_us * 1e-6 * rate_hz)
    change = change[change < bits.size - tail]
    falls = change[bits[change] == 0]
    rises = change[bits[change] == 1]

    pulses = []  # (start_us, width_us or None when the line never rises)
    j = 0
    for fall in falls:
        while j < len(rises) and rises[j] <= fall:
            j += 1
        start = fall / rate_hz * 1e6
        if j < len(rises):
            pulses.append((start, (rises[j] - fall) / rate_hz * 1e6))
        else:
            pulses.append((start, None))
    if not pulses:
        return _result(0, 0, 0, 0, 0, False, "no low pulses", 0.0, 0.0)

    starts = np.array([p[0] for p in pulses])
    bounds = [0]
    for i in range(1, len(pulses)):
        if starts[i] - starts[i - 1] > frame_gap_us:
            bounds.append(i)
    bounds.append(len(pulses))

    expected = "".join(
        "1" if (pattern[i // 8] >> (i % 8)) & 1 else "0" for i in range(slots)
    )

    frames = bad_len = bad_width = bad_bits = bad_period = 0
    stuck_low = False
    first = None
    first_frame_us = None
    widths = [w for _, w in pulses if w is not None]
    max_width = max(widths) if widths else 0.0

    for a, b in zip(bounds[:-1], bounds[1:]):
        seg = pulses[a:b]
        if seg[0][0] < min_start_us:
            continue  # the previous image, still running when the capture began
        if len(seg) != slots:
            bad_len += 1
            if first is None:
                first = "frame at %.1f us has %d pulses, expected %d" % (
                    seg[0][0], len(seg), slots)
            continue
        frames += 1
        if first_frame_us is None:
            first_frame_us = seg[0][0]
        closed = True
        for k, (_, width) in enumerate(seg):
            if width is None:
                closed = False
                stuck_low = True
                if first is None:
                    first = "frame at %.1f us: line never released" % seg[0][0]
                break
            if k == 0:
                # Slot 1 is armed with a register write, not by the feed DMA:
                # the pulse also contains the CPU gap between enabling OC3 and
                # starting the timer (about 1 us at 48 MHz, 7 us at 8 MHz).
                continue
            if not (abs(width - one_us) <= tol_us or abs(width - zero_us) <= tol_us):
                bad_width += 1
                if first is None:
                    first = (
                        "frame at %.1f us slot %d: %.2f us is neither %d nor %d"
                        % (seg[0][0], k, width, one_us, zero_us)
                    )
        if slot_us is not None:
            for k in range(1, slots - 1):
                period = seg[k + 1][0] - seg[k][0]
                if abs(period - slot_us) > tol_us:
                    bad_period += 1
                    if first is None:
                        first = (
                            "frame at %.1f us slot %d: period %.2f us, expected %d"
                            % (seg[0][0], k, period, slot_us)
                        )
                    break
        if not closed:
            continue
        kinds = "".join("1" if w < 20.0 else "0" for _, w in seg)
        if kinds != expected:
            bad_bits += 1
            if first is None:
                first = "frame at %.1f us decodes %s, expected %s" % (
                    seg[0][0], kinds, expected)

    clean = (frames > 0 and bad_len == 0 and bad_width == 0 and bad_bits == 0
             and bad_period == 0 and not stuck_low)
    if frames == 0 and first is None:
        first = "no complete frame after %.1f us" % min_start_us
    return _result(frames, bad_len, bad_width, bad_bits, bad_period, stuck_low,
                   first, max_width, min(widths) if widths else 0.0, clean,
                   first_frame_us)


def _result(frames, bad_len, bad_width, bad_bits, bad_period, stuck_low, first,
            max_width, min_width, clean=False, first_frame_us=None):
    return {
        "frames": frames,
        "bad_len": bad_len,
        "bad_width": bad_width,
        "bad_bits": bad_bits,
        "bad_period": bad_period,
        "stuck_low": stuck_low,
        "first": first,
        "max_width": max_width,
        "min_width": min_width,
        "first_frame_us": first_frame_us,
        "clean": clean,
    }


def analyse_file(path, rate_hz, **kwargs):
    return analyse(np.fromfile(path, dtype=np.uint8), rate_hz, **kwargs)


def write_sr(raw, rate_hz, dst, start_us, end_us):
    """Write one trimmed window as a sigrok `.sr` the viewer can open.

    The stress report links one frame per cell, so the capture opens on the
    traffic instead of 200 ms of boot settle and 99 more frames. The archive
    is the same shape sigrok writes: `version`, `metadata`, `logic-1-1`.
    """
    start = max(0, int(round(start_us * 1e-6 * rate_hz)))
    end = min(len(raw), int(round(end_us * 1e-6 * rate_hz)))
    if end - start < 16:
        raise RuntimeError("capture window is empty")
    meta = (
        "[global]\n"
        "sigrok version=0.6.0-stress-report\n\n"
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
        "unitsize=1\n" % SR_RATE_TEXT
    )
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("version", "2")
        z.writestr("metadata", meta)
        z.writestr("logic-1-1", np.asarray(raw[start:end], dtype=np.uint8).tobytes())


# ---------------------------------------------------------------------------
#  Bench plumbing
# ---------------------------------------------------------------------------


def la_present(sigrok):
    try:
        proc = subprocess.run([sigrok, "-d", "fx2lafw", "--scan"],
                              capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return False
    out = (proc.stdout or "") + (proc.stderr or "")
    return "fx2lafw" in out or "Saleae" in out


def capture_cmd(sigrok, out, seconds, rate=RATE):
    return [sigrok, "-d", "fx2lafw",
            "-I", ",".join("D%d" % i for i in range(8)),
            "-c", "samplerate=%s" % rate,
            "--time", str(int(seconds * 1000)),
            "-O", "binary", "-o", out]


def reset_run(prog):
    subprocess.run([prog, "-c", "port=SWD", "-rst", "-run"],
                   capture_output=True, text=True, errors="replace")


def flash_halt(prog):
    """Program and leave the core halted, so the old image cannot run into
    the capture window."""
    proc = subprocess.run(
        [prog, "-c", "port=SWD", "-w", HEX, common.FLASH_ADDR, "-v", "-rst", "-halt"],
        cwd=common.REPO, capture_output=True, text=True, errors="replace")
    if proc.returncode != 0:
        raise RuntimeError("flash failed (%d):\n%s\n%s"
                           % (proc.returncode, proc.stdout, proc.stderr))


def capture(sigrok, prog, out, seconds, pre_reset=PRE_RESET_S):
    """Start the analyzer, reset the target into the run, wait for it."""
    proc = subprocess.Popen(capture_cmd(sigrok, out, seconds),
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True)
    time.sleep(pre_reset)
    reset_run(prog)
    stdout, stderr = proc.communicate()
    if proc.returncode != 0:
        raise RuntimeError("sigrok-cli failed (%d):\n%s\n%s"
                           % (proc.returncode, stdout, stderr))


def run_seconds(guard, bursts=BURSTS):
    slot_us = ONE_US + ZERO_US + guard + 1  # the counter runs 0..ARR inclusive
    return PRE_RESET_S + 0.25 + bursts * SLOTS * slot_us / 1e6 + bursts * 0.002 + 0.3


def build(level, guard, clock, extra=""):
    ext = common.BUILD_EXT or ""
    ext = (ext + " " if ext else "") + "-DSTRESS_LEVEL=%d -DONEWIRE_GUARD_BAND=%d" % (
        level, guard)
    if extra:
        ext += " " + extra
    cmd = [shutil.which("make") or "make", "APP=%s" % APP,
           "OW_MCU=%s" % common.MCU, "EXT=%s" % ext]
    if clock:
        cmd.append("SYSCLK_MHZ=%d" % clock)
    proc = subprocess.run(cmd, cwd=common.REPO, capture_output=True, text=True,
                          errors="replace")
    if proc.returncode != 0:
        raise RuntimeError("make failed (%d):\n%s\n%s"
                           % (proc.returncode, proc.stdout, proc.stderr))


def telemetry(prog):
    """Read `dma_pressure_telemetry` over SWD, or None when unavailable.

    Index order: frames, cndtr_late, cndtr_max, dma_error, aborted,
    ch1_starts, burst_starts, cpu_passes, search_runs, search_bad, rom_crc,
    search_count, inject_us, capture_mode, sink_count, stage, stop_timeout,
    spare, spare, spare.
    """
    nm = shutil.which("arm-none-eabi-nm")
    if not nm or not os.path.isfile(ELF):
        return None
    try:
        proc = subprocess.run([nm, ELF], capture_output=True, text=True,
                              errors="replace")
        m = re.search(r"^([0-9a-fA-F]+) b dma_pressure_telemetry$",
                      proc.stdout, re.M)
        if not m:
            return None
        addr = int(m.group(1), 16)
        out = subprocess.run(
            [prog, "-c", "port=SWD", "-r32", "0x%08x" % addr, "80"],
            capture_output=True, text=True, errors="replace").stdout
        words = []
        for line in out.splitlines():
            mm = re.match(r"^0x[0-9a-fA-F]+ : (.*)$", line)
            if mm:
                words += [int(x, 16) for x in mm.group(1).split()]
        return words if len(words) == 20 else None
    except (OSError, ValueError):
        return None


# ---------------------------------------------------------------------------
#  Matrix
# ---------------------------------------------------------------------------


class Runner:
    def __init__(self, sigrok, prog, out_dir, keep_all, guards=GUARDS, sr_dir=None):
        self.sigrok = sigrok
        self.prog = prog
        self.out_dir = out_dir
        self.keep_all = keep_all
        self.guards = tuple(guards)
        self.sr_dir = sr_dir
        if sr_dir:
            os.makedirs(sr_dir, exist_ok=True)
        self.cache = {}
        self.raw = os.path.join(out_dir, "_current.bin")

    def run(self, level, guard, clock):
        key = (level, guard, clock)
        if key in self.cache:
            return self.cache[key]
        print("  run level=%d guard=%d clock=%s" % (
            level, guard, clock if clock else "default"), flush=True)
        build(level, guard, clock)
        flash_halt(self.prog)
        capture(self.sigrok, self.prog, self.raw, run_seconds(guard))
        # The firmware settles for 200 ms after the reset; the previous image
        # is cut by the reset. Judge only frames that started after it.
        min_start_us = (PRE_RESET_S + SETTLE_MARGIN_S) * 1e6
        slot_us = ONE_US + ZERO_US + guard + 1
        raw = np.fromfile(self.raw, dtype=np.uint8)
        res = analyse(raw, RATE_HZ, min_start_us=min_start_us, slot_us=slot_us)
        tel = telemetry(self.prog)
        res["telemetry"] = tel
        if tel is None:
            res["clean"] = False
        else:
            res["clean"] = bool(res["clean"] and tel[3] == 0 and tel[4] == 0
                                and tel[16] == 0)
        res["level"] = level
        res["clock_mhz"] = clock
        res["guard_us"] = guard
        res["capture"] = None
        if self.sr_dir and res["first_frame_us"] is not None:
            name = "l%d_%s_g%d.sr" % (
                level, "%dmhz" % clock if clock else "def", guard)
            write_sr(raw, RATE_HZ, os.path.join(self.sr_dir, name),
                     res["first_frame_us"] - SR_LEAD_US,
                     res["first_frame_us"] + SLOTS * slot_us + SR_TAIL_US)
            res["capture"] = name
        self.cache[key] = res
        if self.keep_all or not res["clean"]:
            tag = "l%d_g%d_c%s" % (level, guard, clock if clock else "def")
            shutil.copyfile(self.raw, os.path.join(self.out_dir, tag + ".bin"))
        return res

    def min_guard(self, level, clock):
        """Smallest clean guard, or None when even the widest fails."""
        if not self.run(level, self.guards[-1], clock)["clean"]:
            return None
        if self.run(level, self.guards[0], clock)["clean"]:
            return self.guards[0]
        lo, hi = 0, len(self.guards) - 1  # lo fails, hi passes
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if self.run(level, self.guards[mid], clock)["clean"]:
                hi = mid
            else:
                lo = mid
        return self.guards[hi]


def parse_list(text, cast=str):
    return tuple(cast(x.strip()) for x in text.split(",") if x.strip())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--levels", default="", help="comma list, default all")
    ap.add_argument("--guards", default="", help="comma list, default "
                    + ",".join(str(g) for g in GUARDS))
    ap.add_argument("--clocks", default="", help="comma list; default 48,8 on "
                    "F030 and the part default elsewhere")
    ap.add_argument("--sigrok", default=os.environ.get("SIGROK_CLI",
                    r"C:\tools\sigrok-cli\sigrok-cli.exe"))
    ap.add_argument("--keep-runs", action="store_true",
                    help="save every capture, not just the failures")
    ap.add_argument("--json", default="",
                    help="write the whole matrix as JSON for the report generator")
    ap.add_argument("--sr-dir", default="",
                    help="write one trimmed frame per cell as .sr for the viewer")
    args = ap.parse_args(argv)

    guards = parse_list(args.guards, int) or GUARDS
    if list(guards) != sorted(guards) or guards[0] < 1:
        print("FAIL: guards must be ascending and >= 1")
        return 1

    if not la_present(args.sigrok):
        print("SKIP: no fx2lafw logic analyzer found (SIGROK_CLI=%s)"
              % args.sigrok)
        return 0

    status, detail = common.probe()
    if status == "absent":
        print("SKIP: %s" % detail)
        return 0
    if status == "error":
        print("FAIL: %s" % detail)
        return 1
    requested = os.environ.get("HIL_MCU")
    detected = common.bench_from_probe(detail)
    if requested and detected != requested:
        print("FAIL: %s is attached but HIL_MCU=%s was requested"
              % (detected or "an unrecognised target", requested))
        return 1
    if detected is None:
        print("FAIL: cannot identify the attached target (known: %s)"
              % ", ".join(common.BENCH_PROFILES))
        return 1
    common.select_bench(detected)

    prog = common.find_programmer()
    if not prog:
        print("SKIP: STM32CubeProgrammer CLI not found")
        return 0

    stress_supported = detected in STRESS_BENCHES
    levels = parse_list(args.levels, int) or (LEVELS if stress_supported else (0,))
    if not stress_supported:
        levels = tuple(level for level in levels if level == 0) or (0,)
        print("note: the stress generator is F0/F4-only; measuring level 0")
    if detected == "STM32F030x6":
        clocks = parse_list(args.clocks, int) or F030_CLOCKS
    elif detected == "STM32F446xx":
        clocks = parse_list(args.clocks, int) or F446_CLOCKS
    else:
        clocks = parse_list(args.clocks, int) or (None,)

    out_dir = os.path.join(common.REPO, "bench_logs", "hil", "dma_pressure")
    os.makedirs(out_dir, exist_ok=True)
    runner = Runner(args.sigrok, prog, out_dir, args.keep_runs, guards,
                    args.sr_dir or None)
    pattern = source_pattern()
    print("bench: %s, pattern %s, slots %d" % (detected, pattern.hex().upper(), SLOTS))

    rows = []
    failed_default = False
    for clock in clocks:
        for level in levels:
            guard_min = runner.min_guard(level, clock)
            res5 = runner.run(level, 5, clock) if 5 in guards else None
            default_ok = res5["clean"] if res5 else None
            if default_ok is False:
                failed_default = True
            rows.append((clock, level, guard_min, default_ok, res5))
            print("  -> guard_min=%s default5=%s" % (
                guard_min if guard_min is not None else "never clean",
                "clean" if default_ok else ("FAIL" if default_ok is False else "n/a")),
                flush=True)

    print("\nlevel  clock  guard_min  guard5  frames  bad_len  bad_width  bad_bits  "
          "bad_period  cndtr_late  load   cpu_passes")
    for clock, level, guard_min, default_ok, res5 in rows:
        tel = (res5 or {}).get("telemetry") or [None] * 8
        load = None
        if tel[5] is not None:
            load = tel[5] or tel[6] or tel[7]
        print("%-5d  %-5s  %-9s  %-6s  %-6s  %-7s  %-9s  %-8s  %-10s  %-10s  %-5s  %s" % (
            level, clock if clock else "def",
            guard_min if guard_min is not None else "never",
            "clean" if default_ok else ("FAIL" if default_ok is False else "n/a"),
            (res5 or {}).get("frames", 0),
            (res5 or {}).get("bad_len", 0),
            (res5 or {}).get("bad_width", 0),
            (res5 or {}).get("bad_bits", 0),
            (res5 or {}).get("bad_period", 0),
            tel[1] if tel[1] is not None else "?",
            load if load is not None else "?",
            tel[7] if tel[7] is not None else "?"))

    if args.json:
        head = common._git("rev-parse", "--short", "HEAD")
        dirty = bool(common._git("status", "--porcelain"))
        commit = head or None
        cells = []
        for (level, guard, clock), res in sorted(
                runner.cache.items(),
                key=lambda kv: (kv[0][2] if kv[0][2] is not None else -1,
                                kv[0][0], kv[0][1])):
            cells.append({
                "level": level,
                "clock_mhz": clock,
                "guard_us": guard,
                "clean": res["clean"],
                "frames": res["frames"],
                "bad_len": res["bad_len"],
                "bad_width": res["bad_width"],
                "bad_bits": res["bad_bits"],
                "bad_period": res["bad_period"],
                "stuck_low": res["stuck_low"],
                "first_frame_us": res["first_frame_us"],
                "capture": res.get("capture"),
                "telemetry": res.get("telemetry"),
            })
        data = {
            "schema": 1,
            "title": "Stress test report",
            "generated": time.strftime("%Y-%m-%d"),
            "bench": detected,
            "mcu": common.MCU,
            "example": APP,
            "pattern": pattern.hex().upper(),
            "slots": SLOTS,
            "bursts": BURSTS,
            "stress_words": STRESS_WORDS,
            "capture_rate_hz": RATE_HZ,
            "default_guard_us": 5,
            "guards": list(guards),
            "commit": commit,
            "dirty": dirty,
            "cells": cells,
            "guard_min": [
                {"level": level, "clock_mhz": clock, "guard_us": guard_min}
                for clock, level, guard_min, _default_ok, _res5 in rows
            ],
        }
        out_json = os.path.abspath(args.json)
        os.makedirs(os.path.dirname(out_json), exist_ok=True)
        with open(out_json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(data, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("json: %s" % args.json)

    if any(row[2] is None for row in rows):
        print("FAIL: at least one level never produced a clean frame")
        return 1
    if failed_default:
        print("FAIL: the 5 us default guard is not clean under at least one level")
        return 1
    print("PASS: every level is clean at 5 us; smallest clean guard %d us"
          % min(row[2] for row in rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
