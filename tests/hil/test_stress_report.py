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

try:
    import numpy as np
except ImportError:
    print("test_stress_report: SKIP (numpy not installed)")
    sys.exit(0)

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
    """The extra benches, high-placement runs, captures and knob re-runs."""
    platforms = []
    priority = []
    captures = {}
    knob = {}
    if not os.path.isdir(PLATFORMS):
        return platforms, priority, captures, knob
    for name in sorted(os.listdir(PLATFORMS)):
        base = os.path.join(PLATFORMS, name)
        path = os.path.join(base, "results.json")
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as fh:
            platforms.append((name, json.load(fh)))
        cap = os.path.join(base, "capture.json")
        if os.path.isfile(cap):
            with open(cap, encoding="utf-8") as fh:
                captures[name] = json.load(fh)
        high = os.path.join(base, "results-high.json")
        if os.path.isfile(high):
            with open(high, encoding="utf-8") as fh:
                priority.append((name, "high", json.load(fh)))
        entry = {}
        for cls in (1, 2, 3):
            payload = {}
            kres = os.path.join(base, "results-prio%d.json" % cls)
            if os.path.isfile(kres):
                with open(kres, encoding="utf-8") as fh:
                    payload["results"] = json.load(fh)
            kcap = os.path.join(base, "capture-prio%d.json" % cls)
            if os.path.isfile(kcap):
                with open(kcap, encoding="utf-8") as fh:
                    payload["capture"] = json.load(fh)
            if payload:
                entry[cls] = payload
        if entry:
            knob[name] = entry
    return platforms, priority, captures, knob

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
    platforms, priority, platform_captures, knob = load_platforms()
    expected = stress_report.build_report(data, capture, platforms, priority,
                                          platform_captures, knob)
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
        check("load_error is 0 for %s/%s" % (level, clock),
              not (default_cell or {}).get("load_error"))

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
    clocks = sorted({c["clock_mhz"] for c in inj_cells},
                    key=lambda c: c if c is not None else -1)
    guards = sorted({c.get("guard_us", 5) for c in inj_cells
                     if c["mode"] == "write"})
    for clock in clocks:
        key = "search_%s" % (clock if clock is not None else "def")
        check("a delayed search is detected at %s" % key,
              first_bad.get(key) is not None, str(first_bad.get(key)))
        detected = []
        for guard in guards:
            bad = [c for c in inj_cells
                   if c["mode"] == "write" and c["clock_mhz"] == clock
                   and c.get("guard_us", 5) == guard and not c.get("clean")]
            if not bad:
                # The widest guard can sit above the last measured value; the
                # table shows ">= N us" instead of a threshold.
                continue
            detected.append(guard)
            key = "write_%s_g%d" % (clock if clock is not None else "def",
                                    guard)
            check("a delayed feed is detected at %s" % key,
                  first_bad.get(key) is not None, str(first_bad.get(key)))
        check("a delayed feed is detected at %s MHz at some guard" %
              (clock if clock is not None else "default"), bool(detected))
    check("the feed delay was measured at more than one guard",
          len(guards) > 1, "guards: %s" % guards)
    check("the feed-delay-vs-guard table is rendered",
          'id="feed-delay"' in actual)
    for c in inj_cells:
        if c["mode"] == "write" and c.get("guard_us", 5) != 5:
            # The guard sweep is summarised in its own table, which links the
            # frame the boundary was measured on rather than every cell.
            continue
        name = c.get("capture")
        good = c.get("clean") if c["mode"] == "write" else c.get("matches")
        # A failing injection links its kept failure window, not `capture`.
        target = name if good else c.get("capture_fail")
        if target:
            check("inject capture %s is linked" % target,
                  ("./../stress/captures/" + target) in actual)

    platforms, priority, platform_captures, knob = load_platforms()
    if knob:
        check("the priority-knob table is rendered", 'id="knob"' in actual)
        for name, classes in knob.items():
            for cls, payload in classes.items():
                for kind, source in payload.items():
                    cells = (source.get("cells")
                             or source.get("capture_cells") or [])
                    check("knob %s prio%d %s has cells" % (name, cls, kind),
                          bool(cells))
                    check("knob %s prio%d %s records an image hash"
                          % (name, cls, kind),
                          all(c.get("image_sha256") for c in cells))
                    check("knob %s prio%d %s records feed_prio"
                          % (name, cls, kind),
                          source.get("feed_prio") == cls)
    linked = set()
    for name, pdata in platforms:
        # One representative failing frame per level; a level that already has a
        # clean guard was not shown as failing and needs no link.
        best = {}
        for cell in pdata["cells"]:
            if not cell.get("capture_fail"):
                continue
            level = cell["level"]
            best.setdefault(level, []).append(cell)
        for level, group in best.items():
            # A not-clean row is shown when no measured guard passed at the
            # default, mirroring the table's own verdict rule.
            default_guard = pdata.get("default_guard_us", 5)
            by_cell = {(c["level"], c["clock_mhz"], c["guard_us"]): c
                       for c in pdata["cells"]}
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
        if cell.get("clean"):
            check("%s is one clean frame" % label,
                  a["frames"] == 1 and a["clean"],
                  "%d frame(s), %s" % (a["frames"], a["first"]))
        else:
            # A failing cell may still record the frame it could not decode;
            # the archive only has to replay to a verdict, not to a clean one.
            check("%s replays to a verdict" % label,
                  a["frames"] >= 0 and (a["frames"] > 0 or bool(a["first"])),
                  "%d frame(s), %s" % (a["frames"], a["first"]))

    for name, _role, data_high in priority:
        base = os.path.join(PLATFORMS, name, "captures-high")
        for cell in data_high["cells"]:
            label = "%s-high l%d/%s/g%d" % (
                name, cell["level"],
                "%d MHz" % cell["clock_mhz"] if cell["clock_mhz"] else "default",
                cell["guard_us"])
            check("%s has a valid run identity" % label,
                  cell.get("identity") is True)
            tel = cell.get("telemetry") or []
            check("%s has live telemetry" % label,
                  any(v not in (None, 0) for v in tel),
                  "all-zero telemetry")
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
                check("%s l%d/%s/g%d has no load-channel error" % (
                    name, cell["level"], cell["clock_mhz"], cell["guard_us"]),
                    not cell.get("load_error"))
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

    for name, pdata in platforms:
        cap = platform_captures.get(name)
        if not cap:
            continue
        ccells = cap.get("capture_cells") or []
        check("%s capture cells record a verdict" % name,
              bool(ccells) and all("matches" in c for c in ccells))
        for c in ccells:
            cname = c.get("capture")
            if not cname:
                continue
            path = os.path.join(PLATFORMS, name, "captures", cname)
            check("%s search capture %s exists" % (name, cname),
                  os.path.isfile(path))
            check("%s search capture %s is linked" % (name, cname),
                  ("./../stress/platforms/%s/captures/%s" % (name, cname))
                  in actual)
        bad = [c for c in ccells if not c.get("matches")]
        if bad:
            check("%s capture failures are named in the report" % name,
                  ("returned no devices in %d of %d cells"
                   % (len(bad), len(ccells))) in actual)
            check("%s capture failures keep both attempts" % name,
                  all(c.get("attempts", 1) >= 2 for c in bad))

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
