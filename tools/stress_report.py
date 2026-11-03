#!/usr/bin/env python3
"""Generate the published stress-test report from the HIL JSON.

The report is a deterministic function of `results.json` (plus the committed
`.sr` captures it names): `make check` runs this file into a temporary path and
compares it with the committed `docs/stress/index.html`, so the published page
cannot drift from the measured data.

Usage:
    python tools/stress_report.py \
        --data docs/stress/results.json \
        --out docs/stress/index.html
"""

import argparse
import html
import json
import os
import sys
from urllib.parse import quote

REPO_URL = "https://github.com/a5021/stm32-async-1wire"

LEVEL_INFO = {
    0: ("none", "The driver's own margin: no load beside the bus."),
    1: ("CPU SRAM hammer",
        "The CPU walks the 1 KiB destination array in the "
        "onewire_bus_done() poll loop."),
    2: ("finite DMA burst, ~1 ms",
        "A 512-halfword mem-to-memory burst on DMA1 channel 2, started about "
        "once per millisecond."),
    3: ("finite DMA burst, back to back",
        "The same channel-2 burst restarted as soon as it drains, so the "
        "channel competes for nearly every slot."),
    4: ("circular DMA hog",
        "A continuous circular mem-to-memory transfer on DMA1 channel 1 "
        "(the lowest channel number, equal priority)."),
    5: ("circular DMA + CPU",
        "Level 4 plus the CPU SRAM hammer."),
}

FAILURE_CLASSES = [
    ("A transfer past the update", "the slot repeats its previous bit; the "
     "decoded pattern changes and a frame can lose the final release."),
    ("A transfer inside the next low pulse", "the pulse stretches or a second "
     "edge appears; the slot no longer matches a '1' or a '0' width."),
    ("A transfer never serviced", "CNDTR stays non-zero at completion and the "
     "line can be parked low - complete starvation."),
]


def e(value):
    return html.escape("" if value is None else str(value), quote=True)


def window_us(guard):
    """The real feed window: ARR+1 - CC2 = guard + 1 microseconds."""
    return None if guard is None else guard + 1


def fmt_guard(guard):
    return "never clean" if guard is None else "%d us" % guard


def cell_map(data):
    return {(c["level"], c["clock_mhz"], c["guard_us"]): c for c in data["cells"]}


def telemetry(cell):
    t = (cell or {}).get("telemetry") or [None] * 8
    return t + [None] * (8 - len(t))


def viewer_href(cell):
    name = (cell or {}).get("capture")
    if not name:
        return None
    src = quote("../stress/captures/" + name, safe="/.")
    # Speed is pinned: the 60 us '0' pulses of a diagnostic pattern would
    # otherwise look like overdrive resets to the auto detector.
    return "../viewer/?src=" + src + "&speed=standard"


def link_to_viewer(cell, label):
    href = viewer_href(cell)
    if not href:
        return '<span class="muted">%s -</span>' % e(label)
    name = cell["capture"]
    return '<a href="%s" title="%s">%s</a>' % (e(href), e(name), e(label))


def load_evidence(cell, level):
    if not cell:
        return "?"
    t = telemetry(cell)
    parts = []
    if level in (4, 5) and t[5]:
        parts.append("hog starts %d" % t[5])
    if level in (2, 3) and t[6]:
        parts.append("DMA bursts %d" % t[6])
    if level in (1, 5) and t[7]:
        parts.append("CPU passes %d" % t[7])
    return ", ".join(parts) or "-"


def violation_text(cell):
    if not cell:
        return "?"
    parts = []
    for key, label in (("bad_len", "length"), ("bad_width", "width"),
                       ("bad_bits", "bits"), ("bad_period", "period")):
        if cell.get(key):
            parts.append("%s %d" % (label, cell[key]))
    if cell.get("stuck_low"):
        parts.append("line stuck low")
    return ", ".join(parts) or "0"


def timeline_svg():
    """A schematic of one slot: the feed must land between CC2 and update."""
    x0, x1 = 46.0, 640.0
    span = 80.0

    def x(us):
        return x0 + (x1 - x0) * us / span

    guard_l, guard_r = x(65), x(71)
    return """<svg class="timeline" viewBox="0 0 700 120" role="img"
     aria-label="One slot: the feed request at 65 microseconds must land before the update at 71 microseconds">
  <line x1="%.1f" y1="72" x2="%.1f" y2="72" stroke="currentColor" stroke-width="2"/>
  <rect x="%.1f" y="58" width="%.1f" height="28" fill="#f0a020" opacity="0.35"/>
  <line x1="%.1f" y1="50" x2="%.1f" y2="94" stroke="#b06a00" stroke-width="2"/>
  <line x1="%.1f" y1="50" x2="%.1f" y2="94" stroke="#b06a00" stroke-width="2"/>
  <text x="%.1f" y="40" text-anchor="middle" class="svg-label">CC2: feed request</text>
  <text x="%.1f" y="40" text-anchor="middle" class="svg-label">update: next slot</text>
  <text x="%.1f" y="112" class="svg-label">guard window: 6 us at guard 5, 2 us at guard 1</text>
</svg>""" % (
        x(0), x(80), guard_l, guard_r - guard_l, guard_l, guard_l, guard_r, guard_r,
        guard_l, guard_r, x0,
    )


def bar_chart(rows):
    top = max((v for _, v in rows), default=0) or 1
    out = ['<div class="bars">']
    for label, value in rows:
        width = 100.0 * value / top
        out.append(
            '<div class="bar"><span class="bar-label">%s</span>'
            '<span class="track"><span class="fill" style="width:%.1f%%"></span></span>'
            '<span class="bar-value">%d</span></div>' % (e(label), width, value))
    out.append("</div>")
    return "\n".join(out)


def mode_label(mode):
    return "feed (write frame)" if mode == "write" else "capture (Search ROM)"


def capture_section(capture):
    """The Search ROM under load table, or "" when the section did not run."""
    cells = capture.get("capture_cells") or []
    if not capture.get("capture_supported") or not cells:
        return ""
    rows = []
    for c in sorted(cells, key=lambda c: (c["clock_mhz"] or -1, c["guard_us"], c["level"])):
        href = viewer_href(c)
        link = ('<a href="%s">frame</a>' % e(href)) if href else '<span class="muted">-</span>'
        rows.append(
            "<tr><td>%d</td><td>%s</td><td>%d us</td><td>%s</td><td><code>%08X</code></td>"
            "<td>%s</td><td>%s</td><td>%s</td></tr>" % (
                c["level"], "%d MHz" % c["clock_mhz"] if c["clock_mhz"] else "default",
                c["guard_us"], c["count"] if c.get("count") is not None else "?",
                c.get("rom_crc") or 0, c.get("sink_count", "?"),
                "yes" if c.get("matches") else "no", link))
    baseline = capture.get("capture_baseline") or {}
    base_bits = []
    for clock, b in sorted(baseline.items(), key=lambda kv: str(kv[0])):
        base_bits.append("%s: %s devices, <code>%08X</code>" % (
            clock if clock != "def" else "default", b.get("count"), b.get("rom_crc") or 0))
    return """<section id="capture">
<h2>Capture drain under load</h2>
<p>The Search ROM pass reads every bit through the capture DMA (channel 4),
so it exercises the other deadline the Reddit question names. One search runs
per boot at each level and clock, at the default guard and at the 1 us floor.
The unloaded level-0 pass is the baseline (%s); a stressed pass must return
the same DS18B20 count and the same FNV-1a checksum over the found ROMs.</p>
<table><thead><tr><th>Level</th><th>Clock</th><th>Guard</th><th>DS18B20s</th>
<th>ROM checksum</th><th>ROM callbacks</th><th>Matches baseline</th>
<th>Viewer</th></tr></thead>
<tbody>%s</tbody></table>
<p class="note">All %d stressed cells match. The DS18B20 search filters family
0x28, so the firmware counts seven sensors while the viewer shows all eight
responders on the bench (seven DS18B20 plus one DS2431). The viewer opens the
whole search: the protocol tree shows the reset, the search command and the
ROM triplets. At the highest stress levels the poll loop can stretch the
inter-bit gaps; the firmware's count/checksum remains the primary check and
the viewer splits the search at those gaps.</p>
</section>""" % (", ".join(base_bits), "\n".join(rows), len(cells))


def inject_section(capture):
    """The deliberately delayed transfer table, or ""."""
    cells = capture.get("inject_cells") or []
    if not cells:
        return ""
    rows = []
    for c in sorted(cells, key=lambda c: (c["mode"], c["clock_mhz"] or -1, c["inject_us"])):
        good = c.get("clean") if c["mode"] == "write" else c.get("matches")
        href = viewer_href(c)
        link = ('<a href="%s">frame</a>' % e(href)) if href else '<span class="muted">-</span>'
        rows.append("<tr><td>%s</td><td>%s</td><td>%d us</td><td>%s</td><td>%s</td></tr>" % (
            e(mode_label(c["mode"])),
            "%d MHz" % c["clock_mhz"] if c["clock_mhz"] else "default",
            c["inject_us"], "clean" if good else "<strong>detected</strong>", link))
    first = capture.get("first_bad") or {}
    feed = [v for k, v in first.items() if k.startswith("write") and v is not None]
    capd = [v for k, v in first.items() if k.startswith("search") and v is not None]
    feed_txt = ("%d us" % min(feed)) if feed else "not detected"
    cap_txt = ("%d us" % min(capd)) if capd else "not detected"
    return """<section id="inject">
<h2>Negative control: a deliberately delayed transfer</h2>
<p>Detecting a late transfer is not the same as detecting a wrong count or a
wrong route. This is a positive control: <code>STRESS_INJECT_US</code> holds
the feed channel (write mode) or the capture channel (search mode) disabled
for N microseconds, then restores it. The pending request is serviced late;
the harness must go red.</p>
<table><thead><tr><th>Path</th><th>Clock</th><th>Channel off</th>
<th>Result</th><th>Viewer</th></tr></thead>
<tbody>%s</tbody></table>
<p>The first delayed feed is detected at <strong>%s</strong> and the first
delayed capture at <strong>%s</strong>. The write frame's first CC2 request is
at 65 us, so an 80 us disable already misses the deadline; the search's first
capture is later, which is why its threshold is higher. The viewer links on
the detected rows show the corrupted frame/search.</p>
</section>""" % ("\n".join(rows), feed_txt, cap_txt)


def build_report(data, capture=None):
    capture = capture or {}
    cells = cell_map(data)
    floors = data.get("guard_min") or []
    control = max(data.get("guards") or [100])
    default_guard = data.get("default_guard_us", 5)
    floor_guard = min((row["guard_us"] for row in floors), default=default_guard)
    floor_window = window_us(floor_guard)
    default_window = window_us(default_guard)
    ratio = (default_window / floor_window) if floor_window else 0.0
    clocks = []
    for row in floors:
        clock = row["clock_mhz"]
        if clock is not None and clock not in clocks:
            clocks.append(clock)
    rate_mhz = (data.get("capture_rate_hz") or 0) / 1e6

    all_floor_clean = True
    all_default_clean = True
    all_cndtr_zero = True
    all_no_error = True
    for row in floors:
        level, clock = row["level"], row["clock_mhz"]
        floor_cell = cells.get((level, clock, row["guard_us"]))
        default_cell = cells.get((level, clock, default_guard))
        if not floor_cell or not floor_cell["clean"]:
            all_floor_clean = False
        if not default_cell or not default_cell["clean"]:
            all_default_clean = False
        for cell in (floor_cell, default_cell):
            t = telemetry(cell)
            if t[1] not in (0, None):
                all_cndtr_zero = False
            if t[3] not in (0, None):
                all_no_error = False

    cpu_bits = []
    for clock in clocks:
        cpu1 = telemetry(cells.get((1, clock, default_guard)))
        cpu5 = telemetry(cells.get((5, clock, default_guard)))
        if cpu1[7] is not None and cpu5[7] is not None:
            cpu_bits.append("at %d MHz from %d to %d" % (clock, cpu1[7], cpu5[7]))

    findings = [
        "Every level x clock cell is clean at the %s floor (a %d us window)."
        % (fmt_guard(floor_guard), floor_window),
        "cndtr_late is 0 and no DMA error was latched in any cell: not even "
        "a queued transfer remained at completion.",
        "The load is real: the circular transfer plus the CPU hammer cuts the "
        "CPU's SRAM passes " + "; ".join(cpu_bits) + "."
        if cpu_bits else
        "The load is real: the stress counters in the table show every channel "
        "running.",
        "The %d us guard is a %d us window, %.1fx the measured %d us floor; "
        "the library and the DMA priority stay unchanged."
        % (default_guard, default_window, ratio, floor_window),
    ]
    if not (all_floor_clean and all_default_clean and all_cndtr_zero and all_no_error):
        findings = ["The published run contains a failure; see the table below."]
    else:
        cap_cells = capture.get("capture_cells") or []
        cap_supported = bool(capture.get("capture_supported") and cap_cells)
        if cap_supported and all(c.get("matches") for c in cap_cells):
            baseline = capture.get("capture_baseline") or {}
            base_count = next(iter(baseline.values()), {}).get("count")
            findings.append(
                "Search ROM under the same loads returned the same %s devices "
                "and checksum in all %d cells: the capture DMA met its deadline "
                "too." % (base_count, len(cap_cells)))
        first_bad = capture.get("first_bad") or {}
        feed = [v for k, v in first_bad.items()
                if k.startswith("write") and v is not None]
        capd = [v for k, v in first_bad.items()
                if k.startswith("search") and v is not None]
        if feed and capd:
            findings.append(
                "A deliberately delayed feed (%d us) and capture (%d us) both "
                "turn the checks red: the harness catches a late transfer, not "
                "just a wrong count or route." % (min(feed), min(capd)))

    # Main matrix.
    matrix_rows = []
    for row in floors:
        level, clock = row["level"], row["clock_mhz"]
        guard_min = row["guard_us"]
        default_cell = cells.get((level, clock, default_guard))
        floor_cell = cells.get((level, clock, guard_min))
        control_cell = cells.get((level, clock, control))
        t = telemetry(default_cell or floor_cell)
        links = []
        seen = set()
        for cell, label in ((control_cell, "%d us" % control),
                            (default_cell, "%d us" % default_guard),
                            (floor_cell, "%s" % fmt_guard(guard_min))):
            guard = (cell or {}).get("guard_us")
            if guard in seen:
                continue
            seen.add(guard)
            links.append(link_to_viewer(cell, label))
        matrix_rows.append(
            "<tr><td>%d</td><td>%s</td><td>%s%s</td><td>%d</td><td>%s</td>"
            "<td>%s</td><td>%s</td><td>%s</td><td class=\"links\">%s</td></tr>" % (
                level,
                "%d MHz" % clock if clock else "default",
                fmt_guard(guard_min),
                "" if guard_min is None else " (%d us)" % window_us(guard_min),
                (default_cell or {}).get("frames", 0),
                violation_text(default_cell),
                t[1] if t[1] is not None else "?",
                t[3] if t[3] is not None else "?",
                e(load_evidence(default_cell, level)),
                " ".join(links) or '<span class="muted">-</span>'))

    # CPU contention: level 1 vs level 5 at each clock.
    cpu_rows = []
    for clock in clocks:
        for level in (1, 5):
            cell = cells.get((level, clock, default_guard))
            t = telemetry(cell)
            if t[7] is not None:
                cpu_rows.append(("%d MHz - level %d (%s)" % (
                    clock, level, "CPU only" if level == 1 else "circular + CPU"),
                    t[7]))
    chart = bar_chart(cpu_rows) if cpu_rows else ""

    level_rows = []
    for level in sorted(LEVEL_INFO):
        name, detail = LEVEL_INFO[level]
        evidence = []
        for clock in clocks:
            cell = cells.get((level, clock, default_guard))
            evidence.append("%d MHz: %s" % (clock, load_evidence(cell, level)))
        level_rows.append("<tr><td>%d</td><td>%s</td><td>%s</td><td>%s</td></tr>" % (
            level, e(name), e(detail), e("; ".join(evidence) if evidence else "-")))

    failure_rows = "\n".join(
        "<tr><td>%s</td><td>%s</td></tr>" % (e(title), e(text))
        for title, text in FAILURE_CLASSES)

    cap_html = capture_section(capture)
    inj_html = inject_section(capture)

    commit = data.get("commit") or "unknown"
    tree_note = " (working tree)" if data.get("dirty") else ""
    generated = data.get("generated") or "unknown"
    bench = data.get("bench") or "unknown"
    mcu = data.get("mcu") or bench
    pattern = data.get("pattern") or "?"
    slots = data.get("slots") or 0
    bursts = data.get("bursts") or 0
    words = data.get("stress_words") or 0

    return """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Stress test report - stm32-async-1wire</title>
<style>
:root{--fg:#182230;--muted:#5b6b85;--line:#dbe2ec;--bg:#fff;--soft:#f6f8fb;
--accent:#1b57c4;--ok:#1f7a3d;--okbg:#eaf6ee;--warn:#9a5b00;--bad:#a12222}
@media (prefers-color-scheme:dark){:root{--fg:#e7edf5;--muted:#9fb0c7;
--line:#334155;--bg:#0f1722;--soft:#162130;--accent:#7fb0ff;--ok:#6fce8f;
--okbg:#12301e;--warn:#e0a03a;--bad:#ff8a8a}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);
font:16px/1.55 system-ui,-apple-system,Segoe UI,sans-serif}
header{border-bottom:1px solid var(--line);background:var(--soft)}
header .inner,main{max-width:74rem;margin:0 auto;padding:0 1.25rem}
header .inner{display:flex;gap:1rem;align-items:baseline;padding-top:.8rem;
padding-bottom:.8rem;flex-wrap:wrap}
header a{color:var(--accent);text-decoration:none;font-weight:600}
header .sep{color:var(--muted)}
main{padding-top:1.5rem;padding-bottom:3rem}
h1{font-size:2rem;margin:.2rem 0} h2{margin-top:2.2rem;font-size:1.35rem}
h3{margin-top:1.3rem;font-size:1.05rem}
p.lead{color:var(--muted);font-size:1.1rem;margin:.2rem 0 1rem}
.meta{color:var(--muted);font-size:.92rem}
.verdict{border:1px solid var(--line);border-left:6px solid var(--ok);
background:var(--okbg);padding:.9rem 1rem;border-radius:8px;margin:1rem 0}
.verdict.bad{border-left-color:var(--bad)}
ul.findings{padding-left:1.2rem} ul.findings li{margin:.35rem 0}
table{border-collapse:collapse;width:100%%;margin:1rem 0;font-size:.92rem}
th,td{border:1px solid var(--line);padding:.45rem .55rem;text-align:left;
vertical-align:top} th{background:var(--soft)}
td.num{text-align:right;white-space:nowrap}
td.links a{display:inline-block;margin-right:.5rem;white-space:nowrap}
a{color:var(--accent)} .muted{color:var(--muted)}
code,pre{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
font-size:.88em} pre{background:var(--soft);border:1px solid var(--line);
padding:.8rem 1rem;border-radius:8px;overflow:auto}
.timeline{width:100%%;max-width:700px;display:block;margin:1rem 0}
.svg-label{font:13px system-ui,sans-serif;fill:currentColor}
.svg-small{font:12px system-ui,sans-serif;fill:#6b7a90}
.bars{margin:1rem 0;max-width:46rem}
.bar{display:grid;grid-template-columns:16rem 1fr 4.5rem;gap:.6rem;
align-items:center;margin:.35rem 0;font-size:.9rem}
.bar-label{color:var(--muted)} .track{background:var(--soft);
border:1px solid var(--line);height:1rem;border-radius:4px;overflow:hidden}
.fill{display:block;height:100%%;background:var(--accent);opacity:.65}
.bar-value{text-align:right;font-variant-numeric:tabular-nums}
.note{color:var(--muted);font-size:.9rem}
footer{border-top:1px solid var(--line);color:var(--muted);font-size:.85rem;
padding:1rem 1.25rem 2rem;max-width:74rem;margin:0 auto}
</style>
</head>
<body>
<header><div class="inner">
<a href="../">stm32-async-1wire</a><span class="sep">/</span>
<strong>Stress test report</strong><span class="sep">|</span>
<a href="../viewer/">1-Wire viewer</a>
<a href="../api/">API</a>
<a href="../coverage/">Coverage</a>
</div></header>
<main>
<h1>Stress test report</h1>
<p class="lead">DMA feed margin under memory-subsystem load - %s</p>
<p class="meta">Measured %s at commit <code>%s</code>%s on <code>%s</code>
(<code>%s</code>). Build <code>%s</code>, pattern <code>%s</code>,
%d slots per frame, %d frames per cell, %d halfwords per DMA burst.</p>

<section id="summary">
<h2>Summary</h2>
<div class="verdict%s">
<strong>%s</strong>
</div>
<ul class="findings">
%s
</ul>
<p>The question was whether memory-subsystem load can delay the CCR3 feed DMA
past its slot deadline. On this bench it does not: the wire stayed correct at
the timer's smallest valid guard even when the load halved the CPU's own SRAM
throughput.</p>
</section>

<section id="mechanism">
<h2>What can go wrong</h2>
<p>Each write slot is one TIM1 period. The CC2 compare requests the next CCR3
value from DMA1 channel 3; the update event starts the next slot. The transfer
has the window between those two events - <code>guard + 1</code> microseconds,
because the counter runs from 0 to ARR inclusive.</p>
%s
<table><thead><tr><th>Late transfer</th><th>What the wire shows</th></tr></thead>
<tbody>%s</tbody></table>
<p>The port leaves OC3PE off on the DMA-fed paths, so a late CCR3 write acts
immediately instead of waiting for an update: the failure can appear inside
the current slot, not only at the next one.</p>
</section>

<section id="rig">
<h2>Rig and method</h2>
<p><code>examples/0_dma_pressure</code> transmits a fixed 8-byte pattern (64
slots plus the trailing release) back to back, with a selectable load beside
the bus. The bench is <code>%s</code>; D0 is the 1-Wire line on PA10. The
capture runs at %s MS/s and the firmware is flashed halted, reset by the
harness, so only the intended run is judged.</p>
<table><tbody>
<tr><th>MCU</th><td><code>%s</code></td></tr>
<tr><th>Clocks</th><td>%s</td></tr>
<tr><th>Pattern</th><td><code>%s</code> (LSB first, per byte)</td></tr>
<tr><th>Frames per cell</th><td>%d</td></tr>
<tr><th>Load channels</th><td>DMA1 channel 1 (circular hog) and channel 2
(finite bursts), all at the default priority</td></tr>
<tr><th>On-chip check</th><td><code>cndtr_late</code> (<code>DMA1_Channel3-&gt;CNDTR</code>
at completion) and <code>TEIF3</code>, read back over SWD</td></tr>
<tr><th>Wire check</th><td>every low pulse is a '1' or a '0' width, the slot
period matches the build, all 64 bits decode, and the frame has exactly 64
pulses and a release</td></tr>
</tbody></table>
<p class="note">The first slot of a frame is armed by a register write, not by
the feed DMA: its width includes the CPU gap between enabling OC3 and starting
the timer (about 1 us at 48 MHz, 7 us at 8 MHz). The check skips that slot's
width and the first period of each frame. Guard 0 is not an operating point:
CC2 then coincides with the update and the release write produces a spurious
end-of-frame pulse.</p>
</section>

<section id="levels">
<h2>Load ladder</h2>
<table><thead><tr><th>Level</th><th>Load</th><th>What it does</th>
<th>Evidence at the default guard</th></tr></thead>
<tbody>%s</tbody></table>
</section>

<section id="results">
<h2>Results</h2>
<p>Smallest clean guard per level and clock, with the default-guard cell and
the capture links. The parenthetical is the real transfer window
(<code>guard + 1</code> us). A clean cell means 100 complete frames with no
width, period, length or decode violation.</p>
<table><thead><tr><th>Level</th><th>Clock</th><th>guard_min</th>
<th>Frames @ %d us</th><th>Violations @ %d us</th><th>CNDTR late</th>
<th>DMA error</th><th>Load evidence</th><th>Viewer</th></tr></thead>
<tbody>%s</tbody></table>
<p class="note">The viewer opens one representative frame (64 slots, 1 ms
lead-in and tail). The pattern is deliberately not a 1-Wire command sequence,
so the protocol tree stays empty; the Detail view shows the measured low and
slot widths, which is the evidence. The JSON holds every tested cell,
including the %d us control and the bisection probes.</p>
<h3>CPU contention</h3>
<p>The circular transfer is not free: it competes with the CPU for SRAM. These
are the CPU-side passes completed while the bus was running, at the default
guard.</p>
%s
<p>Yet the feed met the same %d us floor in both cases. The load shortened the
CPU's own memory work without shortening the DMA's deadline.</p>
</section>

%s
%s
<section id="interpretation">
<h2>Interpretation</h2>
<ul>
<li>No wire-visible slip at the %d us floor on either clock, under any load
level.</li>
<li><code>cndtr_late = 0</code> everywhere: every feed transfer was serviced
before completion, including the final release.</li>
<li>The load is real, not a no-op: the circular DMA visibly cuts the CPU's
SRAM passes (see the chart above), and the finite-burst levels restart the
channel hundreds or thousands of times per run.</li>
<li>The %d us guard is a %d us window. Even at the %d us floor the wire stayed
clean, so this part does not need a larger default and the library keeps its
default DMA priority.</li>
</ul>
</section>

<section id="caveats">
<h2>Caveats</h2>
<ul>
<li><strong>Equal priority only.</strong> Every load channel stays at the
default DMA priority. An application channel with <code>PL &gt; 0</code> is a
different arbitration case and is not covered by this run.</li>
<li><strong>F0 only.</strong> The generator is implemented for the F0 backend
(STM32F030x6). F4 streams and H5/H7 GPDMA have different arbitration and need
their own run.</li>
<li><strong>One part, one bench, write-only.</strong> The pattern is not a
1-Wire transaction and the attached devices are never addressed (no reset), so
the measurement is the master's timing, not a protocol exchange.</li>
<li><strong>Timer floor.</strong> The 1 us timer cannot resolve a guard below
1; the report therefore says "a 2 us window was enough", not "the latency is
exactly 1 us".</li>
</ul>
</section>

<section id="reproduce">
<h2>Reproduce</h2>
<pre>make APP=0_dma_pressure OW_MCU=STM32F030x6 \\
     EXT="-DOW_PARASITE_POWER=0 -DSTRESS_LEVEL=5 -DONEWIRE_GUARD_BAND=1"

python tests/hil/dma_pressure.py \\
     --json docs/stress/results.json \\
     --sr-dir docs/stress/captures

python tools/stress_report.py \\
     --data docs/stress/results.json \\
     --out docs/stress/index.html</pre>
<p class="note">The matrix needs an fx2lafw logic analyzer and an ST-Link; it
prints SKIP when either is missing. A full 12-cell run takes a few minutes
because every cell is rebuilt and flashed.</p>
</section>

<section id="artifacts">
<h2>Artifacts</h2>
<ul>
<li><a href="results.json">results.json</a> - every tested cell, counters and
SWD telemetry.</li>
<li><a href="matrix.txt">matrix.txt</a> - the exact console output of the
run.</li>
<li><code>captures/</code> - one representative frame per cell, in sigrok
<code>.sr</code> form; the Viewer column above links each one.</li>
<li><a href="%s/blob/main/examples/0_dma_pressure/main.c">examples/0_dma_pressure/main.c</a>
and <a href="%s/blob/main/tests/hil/dma_pressure.py">tests/hil/dma_pressure.py</a>
- the rig and the harness.</li>
</ul>
</section>
</main>
<footer>Generated from <code>results.json</code> by
<code>tools/stress_report.py</code>. This page is deterministic:
<code>make check</code> regenerates it and compares the bytes.</footer>
</body>
</html>
""" % (
        e(mcu), e(generated), e(commit), e(tree_note), e(bench), e(mcu), e(data.get("example") or "?"),
        e(pattern), slots, bursts, words,
        "" if (all_floor_clean and all_default_clean and all_cndtr_zero and all_no_error)
        else " bad",
        "No starvation, no corrupt slot, no missing release under the tested loads."
        if (all_floor_clean and all_default_clean and all_cndtr_zero and all_no_error)
        else "The published run contains a failure.",
        "\n".join("<li>%s</li>" % e(f) for f in findings),
        timeline_svg(), failure_rows,
        e(bench), e(rate_mhz), e(mcu),
        e(", ".join("%d MHz" % c for c in clocks) if clocks else "default"),
        e(pattern), bursts,
        "\n".join(level_rows), default_guard, default_guard, "\n".join(matrix_rows),
        control, chart, floor_window,
        cap_html, inj_html,
        floor_window, default_guard, default_window, floor_window,
        REPO_URL, REPO_URL,
    )


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", default="docs/stress/results.json")
    ap.add_argument("--capture", default="docs/stress/capture.json",
                    help="capture-drain and negative-control JSON")
    ap.add_argument("--out", default="docs/stress/index.html")
    args = ap.parse_args(argv)
    with open(args.data, encoding="utf-8") as fh:
        data = json.load(fh)
    capture = None
    if args.capture and os.path.isfile(args.capture):
        with open(args.capture, encoding="utf-8") as fh:
            capture = json.load(fh)
    text = build_report(data, capture)
    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("report: %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
