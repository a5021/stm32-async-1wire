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
import re
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
              ("./../stress/captures/" + name) in actual)

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
        check("start_timeout is 0 for %s/%s" % (level, clock),
              not (default_cell or {}).get("start_timeout"))

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
              ("./../stress/captures/" + name) in actual)
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
        good = c.get("clean") if c["mode"] == "write" else c.get("matches")
        # A failing injection links its kept failure window, not `capture`.
        target = name if good else c.get("capture_fail")
        if target:
            check("inject capture %s is linked" % target,
                  ("./../stress/captures/" + target) in actual)

    platforms, priority = load_platforms()
    linked = set()
    for name, data in platforms:
        # One representative failing frame per level; a level that already has a
        # clean guard was not shown as failing and needs no link.
        best = {}
        for cell in data["cells"]:
            if not cell.get("capture_fail"):
                continue
            level = cell["level"]
            best.setdefault(level, []).append(cell)
        for level, group in best.items():
            # A not-clean row is shown when no measured guard passed at the
            # default, mirroring the table's own verdict rule.
            default_guard = data.get("default_guard_us", 5)
            by_cell = {(c["level"], c["clock_mhz"], c["guard_us"]): c
                       for c in data["cells"]}
            shown = []
            for cell in group:
                if cell["clean"]:
                    continue
                default = by_cell.get((level, cell["clock_mhz"], default_guard))
                if default and default["clean"]:
                    continue
                shown.append(cell)
            if not shown:
                continue
            shown.sort(key=lambda c: (c["clock_mhz"] or -1, -c["guard_us"]))
            pick = shown[0]
            linked.add((name, level, pick["clock_mhz"], pick["guard_us"]))
    def replay_capture(cell, base, pdata, label):
        """Decode a committed capture and require the verdict the data claims."""
        path = os.path.join(base, cell["capture"])
        check("%s exists" % label, os.path.isfile(path))
        if not os.path.isfile(path):
            return
        with zipfile.ZipFile(path) as z:
            check("%s is a sigrok archive" % label,
                  {"version", "metadata", "logic-1-1"} <= set(z.namelist()))
            raw = np.frombuffer(z.read("logic-1-1"), dtype=np.uint8)
        slot_us = dp.ONE_US + dp.ZERO_US + cell["guard_us"] + 1
        a = dp.analyse(raw, pdata.get("capture_rate_hz") or rate_hz,
                       slot_us=slot_us)
        check("%s is one clean frame" % label,
              a["frames"] == 1 and a["clean"],
              "%d frame(s), %s" % (a["frames"], a["first"]))

    for name, _role, data_high in priority:
        base = os.path.join(PLATFORMS, name, "captures-high")
        for cell in data_high["cells"]:
            if cell.get("capture"):
                replay_capture(cell, base, data_high,
                               "%s-high/%s" % (name, cell["capture"]))

    for name, pdata in platforms:
        base = os.path.join(PLATFORMS, name, "captures")
        check("%s platform section is in the report" % name,
              ('id="platform-%s"' % name) in actual)
        for cell in pdata["cells"]:
            if cell.get("capture"):
                replay_capture(cell, base, pdata,
                               "%s/%s" % (name, cell["capture"]))
            if cell.get("clean"):
                check("%s l%d/%s/g%d has no start timeout" % (
                    name, cell["level"], cell["clock_mhz"], cell["guard_us"]),
                    not cell.get("start_timeout"))
            if cell.get("clean"):
                continue
            # The failing frame is the evidence a dirty cell exists for.
            fail = cell.get("capture_fail")
            check("%s l%d/%s/g%d keeps its failing frame" % (
                name, cell["level"], cell["clock_mhz"], cell["guard_us"]),
                bool(fail), "no capture_fail recorded")
            if not fail:
                continue
            check("%s/%s exists" % (name, fail),
                  os.path.isfile(os.path.join(base, fail)))
            # The table links one representative failing frame per level; only
            # that one is required to be reachable from the page.
            if (name, cell["level"], cell["clock_mhz"],
                    cell["guard_us"]) not in linked:
                continue
            check("%s/%s is linked" % (name, fail),
                  ("./../stress/platforms/%s/captures/%s" % (name, fail)) in actual)

    for label, pdata in [("primary", data)] + platforms:
        if pdata.get("schema", 1) < 2:
            continue
        check("%s records its capture directory" % label,
              bool(pdata.get("sr_dir")))
        check("%s records an image hash" % label,
              all(c.get("image_sha256") for c in pdata["cells"]
                  if c.get("capture")))
    dirty_sections = sum(1 for d in [data] + [p for _n, p in platforms]
                         if d.get("dirty"))
    check("the working-tree flag is rendered for every dirty run",
          actual.count("(working tree)") >= dirty_sections)

    if failures:
        print("FAIL: %d check(s) failed" % len(failures))
        return 1
    print("PASS: stress report and captures")
    return 0


if __name__ == "__main__":
    sys.exit(main())
