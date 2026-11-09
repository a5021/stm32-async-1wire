#!/usr/bin/env python3
"""Host-side checks for the committed example captures - no bench needed.

Run: python tests/hil/test_example_captures.py   (make test-example-captures)

The archives are produced by `tests/hil/example_capture.py`; most on the F030
bench, the 9_multi archive on the F446 bench with the DS2413 PIO pins on D2/D3.
This gate keeps the published files and the viewer links honest: every listed
app has a readable sigrok archive with the expected samplerate and at least one
reset (or, for the diagnostic pattern, real edges), and `docs/examples.md`
links the matching archive through the viewer.
"""

import json
import os
import sys
import zipfile

try:
    import numpy as np
except ImportError:
    print("test_example_captures: SKIP (numpy not installed)")
    sys.exit(0)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import example_capture as ec  # noqa: E402

MANIFEST = os.path.join(REPO, "docs", "examples", "captures.json")
DOCS = os.path.join(REPO, "docs", "examples.md")

EXPECTED = [
    "0_dma_pressure", "1_basic", "2_device_search", "3_round_robin",
    "4_scan_mode", "5_commands", "6_statistics", "7_low_power",
    "8_ds2431", "9_multi",
]

failures = []


def check(what, condition, detail=""):
    if condition:
        print("  ok   %s" % what)
    else:
        print("  FAIL %s%s" % (what, (" - " + detail) if detail else ""))
        failures.append(what)


def edge_counts(raw, rate_hz, channel):
    """Falling edges and reset-shaped lows on one channel."""
    v = (raw >> channel) & 1
    d = np.diff(v.astype(np.int8))
    falls = np.flatnonzero(d < 0) + 1
    rises = np.flatnonzero(d > 0) + 1
    j = 0
    resets = 0
    for fall in falls:
        while j < len(rises) and rises[j] <= fall:
            j += 1
        if j < len(rises) and (rises[j] - fall) / rate_hz * 1e6 >= 300.0:
            resets += 1
    return len(falls), resets


def test_manifest_merge():
    """A partial refresh must not delete the other app entries."""
    existing = {
        "bench": "old",
        "apps": {
            "2_device_search": {"file": "captures/2_device_search.sr"},
            "9_multi": {"file": "captures/9_multi.sr"},
        },
    }
    merged = ec.merge_manifest(
        existing,
        {"2_device_search": {"file": "captures/new.sr"}},
        {"bench": "new", "channel": 0},
    )
    check("manifest merge updates the selected app",
          merged["apps"]["2_device_search"]["file"] == "captures/new.sr")
    check("manifest merge keeps the other app",
          merged["apps"]["9_multi"]["file"] == "captures/9_multi.sr")
    check("manifest merge keeps unrelated metadata",
          merged["bench"] == "new" and merged["channel"] == 0)


def main():
    test_manifest_merge()
    try:
        with open(MANIFEST, encoding="utf-8") as fh:
            manifest = json.load(fh)
    except OSError as exc:
        check("docs/examples/captures.json exists", False, str(exc))
        return 1

    check("manifest names the F030 bench", manifest.get("bench") == "STM32F030x6")
    check("manifest captures D0", manifest.get("channel") == 0)
    check("manifest samplerate is 4 MHz",
          manifest.get("capture_rate_hz") == 4_000_000)

    apps = manifest.get("apps") or {}
    check("manifest has no unexpected app",
          set(apps) <= set(EXPECTED), ", ".join(sorted(set(apps) - set(EXPECTED))))

    with open(DOCS, encoding="utf-8") as fh:
        docs = fh.read()

    base = os.path.dirname(MANIFEST)
    for app in EXPECTED:
        entry = apps.get(app)
        check("%s is in the manifest" % app, entry is not None)
        if not entry:
            continue
        if entry.get("skip"):
            check("%s is skipped with a reason" % app, bool(entry["skip"].strip()))
            if app == "1_basic":
                check("1_basic skip explains the single-sensor limitation",
                      "single" in entry["skip"].lower())
                check("docs explain the missing 1_basic capture",
                      "No capture:" in docs and "1_basic" in docs)
            continue

        bench = entry.get("bench", manifest["bench"])
        check("%s bench is a known profile" % app,
              bench in ec.common.BENCH_PROFILES, bench)
        if app == "9_multi":
            check("9_multi capture is from the F446 DS2413 bench",
                  bench == "STM32F446xx", bench)

        name = entry.get("file")
        check("%s has a capture file" % app, bool(name))
        if not name:
            continue
        path = os.path.join(base, name)
        check("%s capture exists" % app, os.path.isfile(path), path)
        if not os.path.isfile(path):
            continue

        with zipfile.ZipFile(path) as z:
            names = set(z.namelist())
            check("%s is a sigrok archive" % app,
                  {"version", "metadata", "logic-1-1"} <= names,
                  ", ".join(sorted(names)))
            meta = z.read("metadata").decode("utf-8", "replace")
            check("%s declares 4 MHz" % app, "samplerate=4 MHz" in meta)
            raw = np.frombuffer(z.read("logic-1-1"), dtype=np.uint8)

        if app == "9_multi":
            d2_falls, _ = edge_counts(raw, manifest["capture_rate_hz"], 2)
            d3_falls, _ = edge_counts(raw, manifest["capture_rate_hz"], 3)
            check("9_multi PIOA (D2) toggles", d2_falls >= 4,
                  "%d edges" % d2_falls)
            check("9_multi PIOB (D3) toggles", d3_falls >= 4,
                  "%d edges" % d3_falls)
            check("9_multi PIOA and PIOB are distinct channels",
                  not np.array_equal((raw >> 2) & 1, (raw >> 3) & 1))

        falls, resets = edge_counts(raw, manifest["capture_rate_hz"],
                                    manifest["channel"])
        check("%s has real traffic" % app, falls > 100, "%d falling edges" % falls)
        if app == "0_dma_pressure":
            check("%s has the diagnostic pattern" % app, falls > 1000,
                  "%d falling edges" % falls)
        else:
            check("%s contains a 1-Wire reset" % app, resets >= 1,
                  "%d resets" % resets)

        duration_us = len(raw) / manifest["capture_rate_hz"] * 1e6
        check("%s archive covers the declared window" % app,
              duration_us + 1.0 >= entry["window_us"],
              "%.0f us < %.0f us" % (duration_us, entry["window_us"]))
        check("%s window fits the raw capture" % app,
              entry["window_us"] <= entry["seconds"] * 1e6,
              "window %.0f us, raw %.0f us" % (
                  entry["window_us"], entry["seconds"] * 1e6))

        link = ("viewer/?src=./../examples/captures/%s.sr&chan=0&speed=standard"
                % app)
        check("%s is linked from docs/examples.md" % app, link in docs)
        if app == "9_multi":
            check("9_multi PIOA link uses the raw pulse viewer",
                  "chan=2&raw=1" in docs)
            check("9_multi PIOB link uses the raw pulse viewer",
                  "chan=3&raw=1" in docs)

    if failures:
        print("FAIL: %d check(s) failed" % len(failures))
        return 1
    print("PASS: example captures and viewer links")
    return 0


if __name__ == "__main__":
    sys.exit(main())
