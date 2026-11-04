#!/usr/bin/env python3
"""Host-side checks for the published stress-test report - no bench needed.

Run: python tests/hil/test_stress_report.py   (make test-stress-report)

The report is a deterministic function of `docs/stress/results.json`, and each
row links a committed `.sr` capture. This test is the gate that keeps the two
honest: the HTML must regenerate byte-for-byte, every capture must exist and
decode as one clean frame, and the reported guard_min must be the smallest
clean guard actually measured.
"""

import json
import os
import sys
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools"))

import dma_pressure as dp  # noqa: E402
import stress_report  # noqa: E402

DATA = os.path.join(REPO, "docs", "stress", "results.json")
CAPTURE = os.path.join(REPO, "docs", "stress", "capture.json")
HTML = os.path.join(REPO, "docs", "stress", "index.html")
CAPTURES = os.path.join(REPO, "docs", "stress", "captures")
PLATFORMS = os.path.join(REPO, "docs", "stress", "platforms")


def load_platforms():
    """The extra benches, plus their optional high-placement runs."""
    platforms = []
    priority = []
    if not os.path.isdir(PLATFORMS):
        return platforms, priority
    for name in sorted(os.listdir(PLATFORMS)):
        base = os.path.join(PLATFORMS, name)
        path = os.path.join(base, "results.json")
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as fh:
            platforms.append((name, json.load(fh)))
        high = os.path.join(base, "results-high.json")
        if os.path.isfile(high):
            with open(high, encoding="utf-8") as fh:
                priority.append((name, "high", json.load(fh)))
    return platforms, priority

failures = []


def check(what, condition, detail=""):
    if condition:
        print("  ok   %s" % what)
    else:
        print("  FAIL %s%s" % (what, (" - " + detail) if detail else ""))
        failures.append(what)


def main():
    try:
        with open(DATA, encoding="utf-8") as fh:
            data = json.load(fh)
    except OSError as exc:
        check("docs/stress/results.json exists", False, str(exc))
        return 1
    try:
        with open(CAPTURE, encoding="utf-8") as fh:
            capture = json.load(fh)
    except OSError as exc:
        check("docs/stress/capture.json exists", False, str(exc))
        return 1

    with open(HTML, encoding="utf-8") as fh:
        actual = fh.read()
    platforms, priority = load_platforms()
    expected = stress_report.build_report(data, capture, platforms, priority)
    check("index.html is regenerated from results.json + capture.json",
          actual == expected,
          "run python tools/stress_report.py" if actual != expected else "")

    cells = {(c["level"], c["clock_mhz"], c["guard_us"]): c for c in data["cells"]}
    guards = data["guards"]
    default = data["default_guard_us"]
    rate_hz = data["capture_rate_hz"]

    for c in data["cells"]:
        name = c.get("capture")
        if not name:
            continue
        path = os.path.join(CAPTURES, name)
        if not os.path.isfile(path):
            check("capture %s exists" % name, False)
            continue
        with zipfile.ZipFile(path) as z:
            names = set(z.namelist())
            check("%s is a sigrok archive" % name,
                  {"version", "metadata", "logic-1-1"} <= names,
                  ", ".join(sorted(names)))
            meta = z.read("metadata").decode("utf-8", "replace")
            check("%s declares the report samplerate" % name,
                  "samplerate=%.0f MHz" % (rate_hz / 1e6) in meta)
            raw = np.frombuffer(z.read("logic-1-1"), dtype=np.uint8)
        slot_us = dp.ONE_US + dp.ZERO_US + c["guard_us"] + 1
        res = dp.analyse(raw, rate_hz, slot_us=slot_us)
        check("%s is one clean frame" % name,
              res["frames"] == 1 and res["clean"],
              "%d frame(s), %s" % (res["frames"], res["first"]))
        check("%s is linked from the report" % name,
              ("../stress/captures/" + name) in actual)

    for row in data["guard_min"]:
        level, clock, gmin = row["level"], row["clock_mhz"], row["guard_us"]
        cell = cells.get((level, clock, gmin))
        check("guard_min %s/%s/%s is clean" % (level, clock, gmin),
              cell is not None and cell["clean"])
        for guard in guards:
            if guard < gmin and (level, clock, guard) in cells:
                check("guard %d below the minimum fails for %s/%s" % (guard, level, clock),
                      not cells[(level, clock, guard)]["clean"])
        default_cell = cells.get((level, clock, default))
        check("default guard is clean for %s/%s" % (level, clock),
              default_cell is not None and default_cell["clean"])
        tel = (default_cell or {}).get("telemetry") or [None] * 20
        check("cndtr_late is 0 for %s/%s" % (level, clock), tel[1] == 0)
        check("dma_error is 0 for %s/%s" % (level, clock), tel[3] == 0)
        check("stop_timeout is 0 for %s/%s" % (level, clock), tel[16] == 0)

    check("the report has a row per level and clock",
          len(data["guard_min"]) == 12, "got %d" % len(data["guard_min"]))

    cap_cells = capture.get("capture_cells") or []
    check("capture drain was measured", capture.get("capture_supported") is True)
    check("every Search ROM cell matches the unloaded baseline",
          bool(cap_cells) and all(c.get("matches") for c in cap_cells),
          "%d cell(s)" % len(cap_cells))
    for c in cap_cells:
        name = c.get("capture")
        if not name:
            continue
        path = os.path.join(CAPTURES, name)
        check("search capture %s exists" % name, os.path.isfile(path))
        check("search capture %s is linked" % name,
              ("../stress/captures/" + name) in actual)
        with zipfile.ZipFile(path) as z:
            check("search capture %s is a sigrok archive" % name,
                  {"version", "metadata", "logic-1-1"} <= set(z.namelist()))

    inj_cells = capture.get("inject_cells") or []
    first_bad = capture.get("first_bad") or {}
    check("the negative control ran", bool(inj_cells))
    for mode in ("write", "search"):
        for clock in sorted({c["clock_mhz"] for c in inj_cells},
                            key=lambda c: c if c is not None else -1):
            key = "%s_%s" % (mode, clock if clock is not None else "def")
            check("a delayed %s is detected at %s" % (mode, key),
                  first_bad.get(key) is not None, str(first_bad.get(key)))
    for c in inj_cells:
        name = c.get("capture")
        if name:
            check("inject capture %s is linked" % name,
                  ("../stress/captures/" + name) in actual)

    if failures:
        print("FAIL: %d check(s) failed" % len(failures))
        return 1
    print("PASS: stress report and captures")
    return 0


if __name__ == "__main__":
    sys.exit(main())
