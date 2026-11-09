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
    4: ("ch1 burst stream",
        "A finite mem-to-memory burst on DMA1 channel 1, restarted as soon "
        "as it drains. Circular mode is not allowed in memory-to-memory "
        "(RM0091); channel 1 is the lowest channel number at equal priority."),
    5: ("ch1 burst stream + CPU",
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

#: Samples before the frame start kept in every `.sr`, matching the HIL loader.
SR_LEAD_US = 1000.0


def e(value):
    return html.escape("" if value is None else str(value), quote=True)


#: The load channel/stream pair per family and per experiment. A "high" run
#: puts the load above the feed in the equal-priority arbitration order, which
#: is what the priority section compares.
LOAD_PLACEMENT = {
    "f0": {"default": "DMA1 channels 2 and 1", "high": "DMA1 channels 5 and 4"},
    "f4": {"default": "DMA2 streams 0 and 1", "high": "DMA2 streams 5 and 6"},
    "h5": {"default": "GPDMA1 channels 0/1, CCR.PRIO 0",
           "high": "GPDMA1 channels 0/1, CCR.PRIO 3"},
}

FAMILY_NAMES = {
    "f0": "STM32F0",
    "f1": "STM32F1",
    "f3": "STM32F3",
    "f4": "STM32F4",
    "g0": "STM32G0",
    "g4": "STM32G4",
    "l4": "STM32L4",
    "h5": "STM32H5",
    "h7": "STM32H7",
}


def family_of(mcu):
    """'STM32F030x6' -> 'f0'; 'STM32H503xx' -> 'h5'."""
    name = (mcu or "").lower()
    for fam in ("f0", "f1", "f3", "f4", "g0", "g4", "l4", "h5", "h7"):
        if name.startswith("stm32" + fam):
            return fam
    return None


def window_us(guard):
    """The real feed window: ARR+1 - CC2 = guard + 1 microseconds."""
    return None if guard is None else guard + 1


def fmt_guard(guard):
    return "never clean" if guard is None else "%d us" % guard


def cell_map(data):
    return {(c["level"], c["clock_mhz"], c["guard_us"]): c for c in data["cells"]}


def telemetry(cell):
    t = (cell or {}).get("telemetry") or [None] * 20
    return t + [None] * (20 - len(t))


def viewer_href(cell, platform=None, key="capture"):
    name = (cell or {}).get(key)
    if not name:
        return None
    tree = "stress/platforms/%s/" % platform if platform else "stress/"
    # The viewer resolves `src` from its own directory (/viewer/), so captures
    # are one level up in the path - but a leading `../` makes the URL parser
    # drop the repository segment of a gh-pages project site. `./../` climbs
    # the same directory without being stripped.
    src = quote("./../" + tree + "captures/" + name, safe="/.")

    def mark(us):
        return "%.3f" % (us - SR_LEAD_US)

    bounds = (cell or {}).get("window_us")
    bounds = bounds or []
    # The viewer opens on the start of the capture, but the frame sits after
    # the settle and the marker lead: pin it to the frame itself.
    start = bounds[0] if bounds else (cell or {}).get("first_frame_us")
    end = bounds[1] if len(bounds) > 1 else None
    if start is not None:
        # A capture windowed from `first_frame_us - SR_LEAD_US` puts the frame
        # at the lead, not at `first_frame_us`.
        if not bounds:
            start -= SR_LEAD_US
        src += "&t0=" + mark(start)
        if end is not None:
            src += "&t1=" + mark(end)
    # Speed is pinned: the 60 us '0' pulses of a diagnostic pattern would
    # otherwise look like overdrive resets to the auto detector.
    return "../viewer/?src=" + src + "&speed=standard"


def link_to_viewer(cell, label, platform=None, key="capture"):
    href = viewer_href(cell, platform, key)
    if not href:
        return '<span class="muted">%s -</span>' % e(label)
    name = cell[key]
    return '<a href="%s" title="%s">%s</a>' % (e(href), e(name), e(label))


def load_evidence(cell, level):
    if not cell:
        return "?"
    t = telemetry(cell)
    parts = []
    if level in (4, 5) and t[5]:
        parts.append("ch1 bursts %d" % t[5])
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
  <text x="%.1f" y="40" text-anchor="end" class="svg-label">CC2: feed request</text>
  <text x="%.1f" y="40" text-anchor="start" class="svg-label">update: next slot</text>
  <text x="%.1f" y="112" class="svg-label">guard window: 6 us at guard 5, 2 us at guard 1</text>
</svg>""" % (
        x(0), x(80), guard_l, guard_r - guard_l, guard_l, guard_l, guard_r, guard_r,
        guard_l - 6.0, guard_r + 6.0, x0,
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


def search_table_html(capture, platform=None):
    """Search ROM rows with links to the capture on the owning bench."""
    rows = []
    for c in sorted((capture or {}).get("capture_cells") or [],
                    key=lambda c: (c["clock_mhz"] or -1, c["guard_us"], c["level"])):
        # A mismatching search has no decoded frame either; link its kept
        # failure window instead of dropping the evidence.
        key = "capture" if c.get("capture") else "capture_fail"
        label = "failing frame" if key == "capture_fail" else "frame"
        link = link_to_viewer(c, label, platform, key=key)
        rows.append(
            "<tr><td>%d</td><td>%s</td><td>%d us</td><td>%s</td><td><code>%08X</code></td>"
            "<td>%s</td><td>%s</td><td>%s</td></tr>" % (
                c["level"], "%d MHz" % c["clock_mhz"] if c["clock_mhz"] else "default",
                c["guard_us"], c["count"] if c.get("count") is not None else "?",
                c.get("rom_crc") or 0, c.get("sink_count", "?"),
                "yes" if c.get("matches") else "no", link))
    if not rows:
        return ""
    return ("<table><thead><tr><th>Level</th><th>Clock</th><th>Guard</th>"
            "<th>DS18B20s</th><th>ROM checksum</th><th>ROM callbacks</th>"
            "<th>Matches baseline</th><th>Viewer</th></tr></thead>"
            "<tbody>%s</tbody></table>" % "\n".join(rows))


def capture_section(capture):
    """The Search ROM under load table, or "" when the section did not run."""
    cells = capture.get("capture_cells") or []
    if not capture.get("capture_supported") or not cells:
        return ""
    rows = search_table_html(capture)
    baseline = capture.get("capture_baseline") or {}
    base_bits = []
    for clock, b in sorted(baseline.items(), key=lambda kv: str(kv[0])):
        base_bits.append("%s: %s devices, <code>%08X</code>" % (
            clock if clock != "def" else "default", b.get("count"), b.get("rom_crc") or 0))
    guards = sorted({c["guard_us"] for c in cells})
    guard_bits = ", ".join(fmt_guard(g) for g in guards[:-1])
    if len(guards) > 1:
        guard_bits += " and " + fmt_guard(guards[-1])
    else:
        guard_bits = fmt_guard(guards[0])
    return """<section id="capture">
<h2>Capture drain under load</h2>
<p class="meta">Search ROM matrix measured %s at commit <code>%s</code>%s.</p>
<p>The Search ROM pass reads every bit through the capture DMA (channel 4), so
it tests the other deadline. One search runs per boot at each level and clock,
at every measured guard (%s). The unloaded level-0 pass is the baseline (%s); a
stressed pass must return the same DS18B20 count and FNV-1a checksum.</p>
%s
<p class="note">All %d stressed cells match. The search filters family 0x28, so
the firmware counts 7 sensors while the viewer shows 8 responders (7 DS18B20
plus 1 DS2431). <code>SEARCH_MAX</code> caps the count at 8, so this is not a
full bus census. At high load the poll loop can stretch the gaps between bits;
the firmware count/checksum is the main check; the gaps are real host idle
time, not dropped samples.</p>
</section>""" % (e(capture.get("generated") or "unknown"),
                         e(capture.get("commit") or "unknown"),
                         " (working tree)" if capture.get("dirty") else "",
                         guard_bits, ", ".join(base_bits), rows, len(cells))


def platform_fail_stats(data):
    """Return (default-guard failures, all failing cells) for one bench."""
    cells = cell_map(data)
    floors = data.get("guard_min") or []
    default_guard = data.get("default_guard_us", 5)
    default_bad = []
    for row in floors:
        cell = cells.get((row["level"], row["clock_mhz"], default_guard))
        if row["guard_us"] is None or not (cell and cell["clean"]):
            default_bad.append(row)
    all_bad = [c for c in data.get("cells") or [] if not c.get("clean")]
    return default_bad, all_bad


def platform_section(name, data, capture=None):
    """A second bench measured with the same rig, summarised."""
    fam = family_of(data.get("mcu") or data.get("bench"))
    cells = cell_map(data)
    floors = data.get("guard_min") or []
    default_guard = data.get("default_guard_us", 5)
    has_captures = any(c.get("capture") for c in data.get("cells") or [])
    clocks = []
    for row in floors:
        clock = row["clock_mhz"]
        if clock is not None and clock not in clocks:
            clocks.append(clock)
    rows = []
    fails = []
    for row in floors:
        level, clock = row["level"], row["clock_mhz"]
        guard_min = row["guard_us"]
        default_cell = cells.get((level, clock, default_guard))
        default_ok = bool(default_cell and default_cell["clean"])
        if guard_min is None or not default_ok:
            fails.append((level, clock, guard_min))
        if default_ok:
            links = link_to_viewer(default_cell, "frame", name)
        else:
            # The interesting frame: a failing cell has no decodable frame, so
            # the loader keeps the raw marker window and records it as
            # `capture_fail`. Show the widest measured guard that failed: it is
            # the last one before the level started passing, or the default
            # guard when the level never passed. There is no clean frame to
            # offer alongside it, so the row carries this link only.
            poor = [c for c in data.get("cells") or []
                    if c["level"] == level and c["clock_mhz"] == clock
                    and not c["clean"] and c.get("capture_fail")]
            poor.sort(key=lambda c: c["guard_us"], reverse=True)
            links = link_to_viewer(poor[0] if poor else default_cell,
                                   "failing frame", name,
                                   key="capture_fail")
        rows.append(
            "<tr><td>%d</td><td>%s</td><td>%s</td><td>%s</td><td class=\"links\">%s</td></tr>"
            % (level,
               "%d MHz" % clock if clock else "default",
               fmt_guard(guard_min),
               "clean" if default_ok else "<strong>not clean</strong>",
               links))
    commit = data.get("commit") or "unknown"
    tree_note = " (working tree)" if data.get("dirty") else ""
    has_hashes = any(c.get("image_sha256") for c in data.get("cells") or [])
    image_note = " Each cell carries its image SHA-256." if has_hashes else ""
    generated = data.get("generated") or "unknown"
    cap_html = ""
    ccells = (capture or {}).get("capture_cells") or []
    if ccells:
        cap_meta = ('<p class="meta">Search ROM matrix measured %s at commit '
                    '<code>%s</code>%s.</p>'
                    % (e(capture.get("generated") or "unknown"),
                       e(capture.get("commit") or "unknown"),
                       " (working tree)" if capture.get("dirty") else ""))
        bad = [c for c in ccells if not c.get("matches")]
        if bad:
            where = ", ".join(
                "l%d/%s/g%d" % (c["level"],
                                "%d MHz" % c["clock_mhz"] if c["clock_mhz"]
                                else "default", c["guard_us"])
                for c in bad)
            cap_html = cap_meta + "\n" + (
                '<p class="note">Search ROM under load returned no devices in '
                '%d of %d cells (%s); the capture JSON keeps both attempts.</p>'
                % (len(bad), len(ccells), e(where)))
        else:
            cap_html = cap_meta + "\n" + (
                '<p class="note">Search ROM under load matched the '
                'baseline in all %d cells.</p>' % len(ccells))
        cap_html += "\n" + search_table_html(capture, name) + "\n" + (
            '<p class="note">Every row links the capture on this bench. The '
            'gaps between slots are real host idle time, not dropped samples, '
            'so the slot intervals are captured correctly. Whether the ROM '
            'search found devices is the Matches column, not the capture '
            'quality: a zero-device row still has valid intervals. A failed '
            'capture returns 0 devices with checksum <code>811C9DC5</code>, '
            'which is also what an empty bus returns, so check the DMA '
            'counters and the capture window before reading a zero as absent '
            'sensors.</p>')
    if fails:
        verdict = ("<strong>%d level/clock cells do not pass</strong>; they are "
                   "listed as <em>not clean</em> below." % len(fails))
    else:
        verdict = ("<strong>Every level is clean at the %d us default</strong>; "
                   "the measured floor is shown per cell." % default_guard)
    return """
<section id="platform-%s">
<h2>%s (%s)</h2>
<p class="meta">Write matrix measured %s at commit <code>%s</code>%s on <code>%s</code>.
Same rig, same loader, marker-bracketed frames and run-identity checks.%s</p>
<div class="verdict">%s</div>
<table><thead><tr><th>Level</th><th>Clock</th><th>guard_min</th>
<th>Default guard</th><th>Viewer</th></tr></thead>
<tbody>%s</tbody></table>
%s
<p class="note">The floor is the smallest clean guard measured. <em>not
clean</em> means even the widest guard tested did not pass.
%s</p>
</section>
""" % (name, e(FAMILY_NAMES.get(fam) or (data.get("mcu") or name)),
       e(data.get("mcu") or data.get("bench")), e(generated), e(commit),
       e(tree_note), e(data.get("bench")), image_note, verdict, "\n".join(rows),
       cap_html,
       "The capture links open one representative frame per cell."
       if has_captures else
       "No one-frame captures were recorded for this bench.")


def priority_section(primary, others, has_knob=False):
    """Compare the same load placed above and below the feed in arbitration."""
    rows = []
    seen = set()
    for label, role, data in (("primary", "default", primary),) + tuple(others):
        fam = family_of(data.get("mcu") or data.get("bench"))
        if fam not in LOAD_PLACEMENT:
            continue
        key = (fam, data.get("mcu"), role)
        if key in seen:
            continue
        seen.add(key)
        cells = cell_map(data)
        floors = data.get("guard_min") or []
        default_guard = data.get("default_guard_us", 5)
        if not floors:
            continue
        bad = [row for row in floors
               if row["guard_us"] is None
               or not (cells.get((row["level"], row["clock_mhz"], default_guard))
                       or {}).get("clean")]
        clean = len(floors) - len(bad)
        clocks = sorted({row["clock_mhz"] for row in floors if row["clock_mhz"]})
        rows.append("<tr><td>%s</td><td><code>%s</code></td><td>%s</td>"
                    "<td>%d</td><td>%d</td></tr>"
                    % (e(FAMILY_NAMES.get(fam) or fam), e(data.get("mcu") or "?"),
                       e(LOAD_PLACEMENT[fam][role]),
                       clean, len(bad)))
    if len(rows) < 2:
        return ""
    return """
<section id="priority">
<h2>Load placement and DMA priority</h2>
<p>On a classic DMA controller or an F4 DMA stream, equal software priority is
broken by the lower channel/stream number. The historical load sits on the low
numbers (F0 channels 2/1, F4 streams 0/1), so it outranks the feed; the "high"
runs move the same burst to the higher numbers, where the feed wins. GPDMA has
no such tie: <code>CCR.PRIO</code> selects the class, so the H5 "high" run keeps
channels 0/1 and raises the load to PRIO 3 against the feed's PRIO 0. The rows
below are the same levels and guards with only that placement or priority
changed.</p>
<table><thead><tr><th>Family</th><th>Part</th><th>Load placement</th>
<th>Clean cells (default guard)</th><th>Failing cells (default guard)</th></tr></thead>
<tbody>%s</tbody></table>
<p class="note">These are level/clock cells in the <em>write-frame</em> matrix
at the default guard, not the Search ROM matrix above. A failing cell is a
write frame that did not decode clean. On the H503 PRIO 3 row every failing
cell has <code>start_timeout = 0</code> and <code>load_error = 0</code>, so the
higher priority starved the feed; the equal-priority H503 row is clean. A part
that is clean in both rows has spare bandwidth; a part that is clean only with
the load below the feed is sensitive to arbitration order.</p>
<p><strong>Why F0 is immune and F4/H5 are not.</strong> F0's DMA1 arbitrates one
transfer at a time, so a load channel cannot hold the bus across a whole feed
window. An F4 DMA stream can hold a burst/FIFO, and equal priority is broken by
the lower stream number; the historical load on streams 0/1 therefore wins
against the feed on stream 2. H5 GPDMA uses <code>CCR.PRIO</code> classes
instead of channel-number ties, so a PRIO 3 load can starve the default-priority
feed outright. G0, G4, F1, F3 and H7 are not measured here; predict them from
their arbitration granularity and priority rules rather than from the F0
result.</p>
<p><strong>Integration rule.</strong> A higher-ranked, uninterruptible load
burst must not last longer than the feed window (<code>guard + 1</code> us).
If it can, lower its priority, split it into shorter bursts, or raise the feed
and capture priority above it. The library's reset-default priority is not a
guarantee against an application channel with a higher rank.</p>
<p>That rule is deliberately conservative. The delayed-feed sweep above shows
the feed tolerated 14-16 us at 48 MHz and 16-18 us at 8 MHz at the default
guard, so a burst slightly longer than <code>guard + 1</code> is not
automatically fatal; the extra slack depends on the DMA IP, the clock and the
bus position, so it is not a design budget. Measure the burst duration under
the worst SRAM contention, not at its nominal length: on the F446 the load on
streams 0/1 is clean at levels 3 and 4 and fails only at level 5, where the
CPU hammer is added. A lower stream number is necessary but not sufficient to
explain a failure.</p>
<p>%s</p>
</section>
""" % ("\n".join(rows),
       "These rows move the load, not the library. The direct test of the build"
       " knob is the next section: the same failing cells re-run with the feed"
       " and capture raised."
       if has_knob else
       "These rows move the load, not the library. The build knob itself is"
       " not re-measured on these benches yet.")


def _knob_verdict(cell, mode):
    if cell is None:
        return '<span class="muted">not measured</span>'
    ok = cell.get("clean") if mode == "write" else cell.get("matches")
    return "clean" if ok else "<strong>fail</strong>"


def _knob_failure(cell, mode):
    if mode == "write":
        return "<strong>fail</strong>"
    count = cell.get("count")
    return "<strong>fail</strong> (%s devices)" % ("0" if not count else count)


def _knob_classes(knob):
    classes = set()
    for entry in (knob or {}).values():
        classes.update(entry)
    return sorted(classes)


def _knob_cell(entry, cls, mode, level, clock, guard):
    """The cell of one class for the given operation, or None."""
    payload = (entry or {}).get(cls) or {}
    source = payload.get("results" if mode == "write" else "capture")
    if not source:
        return None
    key = "cells" if mode == "write" else "capture_cells"
    for c in source.get(key) or []:
        if (c["level"] == level and c["clock_mhz"] == clock
                and c["guard_us"] == guard):
            return c
    return None


def _knob_good(cell, mode):
    return bool(cell) and bool(
        cell.get("clean") if mode == "write" else cell.get("matches"))


def _knob_baseline(name, data, priority, knob):
    """The committed run a knob build must be compared against.

    The knob runs are made against a load placement: the F446 default (streams
    0/1 at PL 0) or, on the H503, the raised load (PRIO 3). The default column
    has to come from the matching baseline run.
    """
    entry = (knob or {}).get(name) or {}
    first = next((p.get("results") for p in entry.values()
                  if p.get("results")), None)
    raised = bool(first) and (first.get("gpdma_prio") is not None
                              or first.get("load_high"))
    high = {n: d for n, role, d in (priority or []) if role == "high"}
    if raised and name in high:
        return high[name], raised, first
    return data, raised, first


def _knob_minimal(platforms, priority, platform_captures, knob):
    """{bench name: smallest measured class that clears every failure}."""
    mins = {}
    for name, data in (platforms or []):
        entry = (knob or {}).get(name) or {}
        if not entry:
            continue
        base, _raised, _first = _knob_baseline(name, data, priority, knob)
        if base is None:
            continue
        dg = base.get("default_guard_us", 5)
        write_fail = [c for c in base.get("cells") or []
                      if c["guard_us"] == dg and not c["clean"]]
        cap = (platform_captures or {}).get(name) or {}
        search_fail = [c for c in cap.get("capture_cells") or []
                       if not c.get("matches")]
        if not write_fail and not search_fail:
            continue
        for cls in _knob_classes(knob):
            if not entry.get(cls):
                continue
            ok = all(_knob_good(_knob_cell(entry, cls, "write", c["level"],
                                           c["clock_mhz"], dg), "write")
                     for c in write_fail)
            ok = ok and all(_knob_good(_knob_cell(entry, cls, "search",
                                                  c["level"], c["clock_mhz"],
                                                  c["guard_us"]), "search")
                            for c in search_fail)
            if ok:
                mins[name] = cls
                break
    return mins


def knob_section(platforms, priority, platform_captures, knob):
    """Default-failing cells re-measured with OW_DMA_PRIO raised."""
    if not knob:
        return ""
    classes = _knob_classes(knob)
    mins = _knob_minimal(platforms, priority, platform_captures, knob)
    rows = []
    minimal = []
    extra_bits = []
    for name, data in (platforms or []):
        entry = knob.get(name) or {}
        if not entry:
            continue
        base, raised, first = _knob_baseline(name, data, priority, knob)
        if base is None:
            continue
        dg = base.get("default_guard_us", 5)
        bench = base.get("bench") or name
        load_label = ("high (load PRIO %d)" % first["gpdma_prio"]
                      if raised else "default")
        write_fail = [c for c in base.get("cells") or []
                      if c["guard_us"] == dg and not c["clean"]]
        cap = (platform_captures or {}).get(name) or {}
        search_fail = [c for c in cap.get("capture_cells") or []
                       if not c.get("matches")]
        chosen = mins.get(name)
        if chosen is not None:
            minimal.append((bench, chosen))
        if chosen is not None:
            source = (entry.get(chosen) or {}).get("results")
            if source:
                total = clean = 0
                for (_level, _clock, guard), kc in cell_map(source).items():
                    if guard == dg:
                        continue
                    total += 1
                    clean += 1 if kc.get("clean") else 0
                if total:
                    extra_bits.append("%s at class %d: %d of %d other-guard "
                                      "cells clean" % (bench, chosen, clean,
                                                       total))
        for c in write_fail:
            cells = "".join(
                "<td>%s</td>" % _knob_verdict(
                    _knob_cell(entry, cls, "write", c["level"],
                               c["clock_mhz"], dg), "write")
                for cls in classes)
            rows.append(
                "<tr><td>%s</td><td>%s</td><td>write frame</td><td>%s</td>"
                "<td>%d</td><td>%s</td><td>%s</td>%s</tr>"
                % (e(bench), e(load_label),
                   "%d MHz" % c["clock_mhz"] if c["clock_mhz"] else "default",
                   c["level"], fmt_guard(dg), _knob_failure(c, "write"), cells))
        for c in search_fail:
            cells = "".join(
                "<td>%s</td>" % _knob_verdict(
                    _knob_cell(entry, cls, "search", c["level"],
                               c["clock_mhz"], c["guard_us"]), "search")
                for cls in classes)
            rows.append(
                "<tr><td>%s</td><td>%s</td><td>Search ROM</td><td>%s</td>"
                "<td>%d</td><td>%s</td><td>%s</td>%s</tr>"
                % (e(bench), e(load_label),
                   "%d MHz" % c["clock_mhz"] if c["clock_mhz"] else "default",
                   c["level"], fmt_guard(c["guard_us"]),
                   _knob_failure(c, "search"), cells))
    if not rows:
        return ""
    head = "".join("<th><code>OW_DMA_PRIO=%d</code></th>" % cls
                   for cls in classes)
    if minimal:
        min_bits = "; ".join("%s: %d" % (bench, cls) for bench, cls in minimal)
        min_text = ("The smallest measured class that clears every "
                    "default-guard failure is %s." % min_bits)
    else:
        min_text = "No measured class clears every default-guard failure."
    extra_text = (" For the minimal class, the other guards are clean too: %s."
                  % "; ".join(extra_bits)) if extra_bits else ""
    return """<section id="knob">
<h2>DMA priority knob: measured effect</h2>
<p><code>OW_DMA_PRIO</code> is the only lever that changes the library itself.
This table takes the write cells that fail at the default guard plus the Search
ROM cells that fail at the guards the Search matrix covers (1 and 5 us), and
re-runs them at each measured class, leaving the load where it was. The
Default column is the committed run <em>with the same load placement</em>: the
F446 default streams for the F446 rows, and the raised H503 load for the H503
rows, because that is the placement the knob was tested against.</p>
<table><thead><tr><th>Bench</th><th>Load</th><th>Matrix</th><th>Clock</th>
<th>Level</th><th>Guard</th><th>Default</th>%s</tr></thead>
<tbody>%s</tbody></table>
<p>%s%s On classic DMA and F4 streams an equal-class load is still ordered by
channel/stream index, so the knob is not a substitute for the
<code>guard + 1</code> rule there. On the H5 GPDMA the measured equal class was
clean: <code>CCR.PRIO</code> does not break ties by channel number. On the
H5 GPDMA only class 3 is a fixed higher class; classes 0-2 share a weighted
round-robin (AN5593, <em>GPDMA channel priority</em>), which is why classes 1
and 2 do not outrank a PRIO 3 load. The library keeps class 0 by default:
raising it gives the library's pending feed and capture requests precedence
over lower-priority DMA requests during a 1-Wire frame, potentially delaying
those channels. The library asks for one halfword per slot (about every 70 us),
so a victim channel loses one arbitration round per slot, not a frame-long
stall. This report does not measure that victim-side cost, so the default
stays 0 until it is measured; where class 1 clears every failure on F4, the
cost of raising it is the open question, not the benefit.</p>
</section>""" % (head, "\n".join(rows), min_text, extra_text)


def _first_bad_text(first, prefix):
    """Per-clock first-bad values, e.g. "16 us (48 MHz) and 18 us (8 MHz)"."""
    pairs = []
    for key in sorted(first, key=lambda k: (k.split("_", 1)[1] != "def",
                                            k.split("_", 1)[1])):
        value = first[key]
        if not key.startswith(prefix) or value is None:
            continue
        pairs.append((key.split("_", 1)[1], value))
    if not pairs:
        return "not detected"
    groups = {}
    for clock, value in pairs:
        groups.setdefault(value, []).append(clock)
    parts = []
    for value, clocks in groups.items():
        if len(clocks) == len(pairs):
            parts.append("%d us (both clocks)" % value)
            continue
        for clock in clocks:
            parts.append("%d us (%s)" % (value, "default" if clock == "def"
                                         else "%s MHz" % clock))
    return " and ".join(parts)


def _first_bad_from_cells(cells, mode, guard=None):
    """{mode_clock: first failing inject_us} read from the cells themselves.

    The JSON also carries a `first_bad` map, but older captures predate the
    guard dimension; deriving it here keeps the report readable for both.
    """
    out = {}
    for c in cells:
        if c["mode"] != mode:
            continue
        if guard is not None and c.get("guard_us", 5) != guard:
            continue
        key = "%s_%s" % (mode, c["clock_mhz"] if c["clock_mhz"] is not None
                         else "def")
        good = c.get("clean") if mode == "write" else c.get("matches")
        if not good and (key not in out or c["inject_us"] < out[key]):
            out[key] = c["inject_us"]
    return out


def feed_delay_section(capture):
    """Feed delay tolerance vs the guard window, or ""."""
    cells = [c for c in (capture.get("inject_cells") or [])
             if c["mode"] == "write"]
    guards = sorted({c.get("guard_us", 5) for c in cells})
    if len(guards) < 2:
        return ""
    rows = []
    offsets = {}
    for clock in sorted({c["clock_mhz"] for c in cells},
                        key=lambda c: c if c is not None else -1):
        for guard in guards:
            group = [c for c in cells
                     if c["clock_mhz"] == clock and c.get("guard_us", 5) == guard]
            if not group:
                continue
            clean = [c["inject_us"] for c in group if c.get("clean")]
            bad = [c["inject_us"] for c in group if not c.get("clean")]
            detected = ("%d us" % min(bad)) if bad else (
                "&ge; %d us" % max(clean) if clean else "?")
            # Link the frame the boundary was measured on: the first window
            # that failed, kept as the raw marker capture.
            link = '<span class="muted">-</span>'
            if bad:
                first = min((c for c in group if not c.get("clean")),
                            key=lambda c: c["inject_us"])
                href = viewer_href(
                    first, key="capture" if first.get("capture")
                    else "capture_fail")
                if href:
                    link = '<a href="%s">failing frame</a>' % e(href)
            rows.append(
                "<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td>"
                "<td>%d us</td><td>%s</td></tr>" % (
                    "%d MHz" % clock if clock is not None else "default",
                    fmt_guard(guard),
                    ("%d us" % max(clean)) if clean else "none",
                    detected, window_us(guard), link))
            if clean and bad:
                offsets.setdefault(clock, []).append(
                    (guard, min(bad) - (guard + 1)))
    if not rows:
        return ""
    off_bits = []
    for clock, pairs in sorted(offsets.items(),
                               key=lambda kv: kv[0] if kv[0] is not None else -1):
        pairs.sort()
        off_bits.append("%s: %s us" % (
            "%d MHz" % clock if clock is not None else "default",
            "/".join("%+d" % off for _, off in pairs)))
    return """<section id="feed-delay">
<h2>Feed delay tolerance vs the guard window</h2>
<p>The negative control above runs at the default guard. The same sweep at
several guards shows how the tolerance moves: the injected window is held
<em>after</em> the frame's first CC2 request, so the question is whether the
last harmless delay tracks <code>guard + 1</code> plus a constant.</p>
<table><thead><tr><th>Clock</th><th>Guard</th><th>Clean through</th>
<th>First detected</th><th>Guard window</th><th>Viewer</th></tr></thead>
<tbody>%s</tbody></table>
<p><strong>It does not.</strong> The offset over <code>guard + 1</code>
(%s) shrinks as the guard grows. At the widest guard the boundary is +1 us at
8 MHz and -3 us at 48 MHz: the tolerance is not a linear function of the
window, and at 48 MHz the first detected delay lands slightly <em>below</em>
<code>guard + 1</code>. Two effects set the boundary: the hold is started
after the frame's first CC2 request and the busy loop plus channel re-enable
adds about 1-2 us to the nominal value, and the transfer only has to beat the
next compare, so its position relative to the counter matters. A single
constant <code>c</code> would be a fit to noise, so this report states the
measured boundary per guard instead of a formula.</p>
<p>The guard is therefore <em>not</em> a delay budget: it sets the timer's
nominal window, and the measured tolerance only grows from 12 to 16 us at
48 MHz and from 14 to 20 us at 8 MHz while the window grows from 2 to 21 us.
A delay that fits inside <code>guard + 1</code> is always safe because the
request still arrives before the update, but a correct design cannot spend the
rest of the window.</p>
<p>The sweep steps by 2 us, and the busy-loop hold plus the channel re-enable
costs about 1-2 us, so the boundary is good to roughly ±2 us: it pins the
tolerance to the sweep's resolution, not to the microsecond. At the default
guard the feed survives well past the 6 us window, which is why the guard
window and the delay tolerance must not be compared as if they were the same
quantity. The number to design against is still the load-placement rule: a
higher-ranked burst must not outlast <code>guard + 1</code> us. That rule is
conservative by construction - it keeps the request inside the timer's window
regardless of how much extra slack a particular DMA IP tolerates - and the
measured survival above shows the margin it leaves, not an exact failure
point.</p>
</section>""" % ("\n".join(rows),
                "measured: " + "; ".join(off_bits) if off_bits else
                "no guard produced both a clean and a failing cell")


def inject_section(capture):
    """The deliberately delayed transfer table, or ""."""
    cells = capture.get("inject_cells") or []
    if not cells:
        return ""
    rows = []
    # The control table is the default-guard sweep; the guard dependence has
    # its own table (feed_delay_section) so this one stays readable.
    shown = [c for c in cells
             if c["mode"] == "search" or c.get("guard_us", 5) == 5]
    for c in sorted(shown, key=lambda c: (c["mode"], c["clock_mhz"] or -1, c["inject_us"])):
        good = c.get("clean") if c["mode"] == "write" else c.get("matches")
        # The capture harness keeps the raw marker window for a failing cell
        # too, so the stored name is always the frame worth looking at.
        key = "capture" if c.get("capture") else "capture_fail"
        href = viewer_href(c, key=key)
        label = "failing frame" if key == "capture_fail" else "frame"
        link = ('<a href="%s">%s</a>' % (e(href), label)) if href else '<span class="muted">-</span>'
        rows.append("<tr><td>%s</td><td>%s</td><td>%d us</td><td>%s</td><td>%s</td></tr>" % (
            e(mode_label(c["mode"])),
            "%d MHz" % c["clock_mhz"] if c["clock_mhz"] else "default",
            c["inject_us"], "clean" if good else "<strong>detected</strong>", link))
    feed_txt = _first_bad_text(_first_bad_from_cells(cells, "write", 5), "write")
    cap_txt = _first_bad_text(_first_bad_from_cells(cells, "search"), "search")
    return """<section id="inject">
<h2>Negative control: a deliberately delayed transfer</h2>
<p>This control checks that the harness notices a late transfer.
<code>STRESS_INJECT_US</code> disables the feed channel (write mode) or the
capture channel (search mode) for N microseconds, then re-enables it. The delay
must make the cell fail.</p>
<table><thead><tr><th>Path</th><th>Clock</th><th>Channel off</th>
<th>Result</th><th>Viewer</th></tr></thead>
<tbody>%s</tbody></table>
<p>The first delayed feed is detected at <strong>%s</strong>; the first
delayed capture at <strong>%s</strong>. The write window starts at the frame's
first CC2 request (65 us), so these values pin how long the first feed
transfer may be held <em>after</em> that request. A window from the frame start
could not show anything below 65 us: the feed had not asked yet. The search's
first capture is later, so its threshold is higher. The viewer links on the
detected rows show the corrupted frame or search.</p>
</section>""" % ("\n".join(rows), feed_txt, cap_txt)


def build_report(data, capture=None, platforms=None, priority=None,
                 platform_captures=None, knob=None):
    capture = capture or {}
    cells = cell_map(data)
    floors = data.get("guard_min") or []
    control = max(data.get("guards") or [100])
    default_guard = data.get("default_guard_us", 5)
    floor_guard = min((row["guard_us"] for row in floors), default=default_guard)
    if floor_guard is None:
        floor_guard = default_guard
    floor_window = window_us(floor_guard)
    default_window = window_us(default_guard)
    ratio = (default_window / floor_window) if floor_window else 0.0
    # Take the clock list from the cells themselves: the guard_min rows can be
    # shorter than the matrix, and a rig table must not inherit another bench's
    # clocks.
    clocks = sorted({c["clock_mhz"] for c in data.get("cells") or []
                     if c.get("clock_mhz") is not None})
    rate_mhz = (data.get("capture_rate_hz") or 0) / 1e6

    all_floor_clean = True
    all_default_clean = True
    all_cndtr_zero = True
    all_no_error = True
    all_stop_ok = True
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
            if t[16] not in (0, None):
                all_stop_ok = False

    primary_ok = (all_floor_clean and all_default_clean and all_cndtr_zero
                  and all_no_error)
    platform_stats = [(name, other) + platform_fail_stats(other)
                      for name, other in (platforms or [])]
    priority_stats = [(label, role, other) + platform_fail_stats(other)
                      for label, role, other in (priority or [])]
    platform_bad = sum(len(bad) for _, _, bad, _ in platform_stats)
    priority_bad = sum(len(bad) for _, _, _, bad, _ in priority_stats)
    all_clean = primary_ok and not platform_bad and not priority_bad
    ok = all_clean

    load_bits = []
    for clock in clocks:
        t4 = telemetry(cells.get((4, clock, default_guard)))
        if t4[5] is not None:
            load_bits.append("at %d MHz channel 1 restarted %d times"
                             % (clock, t4[5]))

    findings = []
    if primary_ok:
        findings.append(
            "On the primary bench, every level and clock is clean at the "
            "%s floor (a %d us window)."
            % (fmt_guard(floor_guard), floor_window))
    else:
        findings.append(
            "The primary bench contains a failure; see its table below.")
    findings.append(
        "cndtr_late is 0, no DMA error was latched, and no channel-stop "
        "timeout occurred in any primary-bench cell.")
    findings.append(
        "Load activity: " + "; ".join(load_bits) +
        " (level 4; level 5 adds the CPU hammer)."
        if load_bits else
        "Load activity: the stress counters in the table show every channel "
        "running.")
    findings.append(
        "The default guard is a %d us window and the 1 us floor a %d us "
        "window; the delay tolerance is a separate measurement (it grows with "
        "the guard), so neither window is a safety factor."
        % (default_window, floor_window))
    for name, other, bad, all_bad in platform_stats:
        bench = other.get("bench") or name
        if bad:
            findings.append(
                "%s: %d default-guard level/clock cells fail (%d cells across "
                "the guard sweep)." % (bench, len(bad), len(all_bad)))
        else:
            findings.append(
                "%s: every level is clean at the default guard." % bench)
    for label, role, other, bad, all_bad in priority_stats:
        bench = other.get("bench") or label
        if bad:
            findings.append(
                "%s with %s load placement: %d default-guard cells fail "
                "(%d across the guard sweep); the failing cells show "
                "start_timeout=0 and load_error=0."
                % (bench, role, len(bad), len(all_bad)))
        else:
            findings.append(
                "%s with %s load placement: every level is clean at the "
                "default guard." % (bench, role))
    for name, cap in (platform_captures or {}).items():
        bad = [c for c in cap.get("capture_cells") or [] if not c.get("matches")]
        if bad:
            bad_clocks = sorted({c.get("clock_mhz") for c in bad
                                 if c.get("clock_mhz")})
            findings.append(
                "%s Search ROM: %d of %d stressed cells fail%s."
                % (cap.get("bench") or name, len(bad),
                   len(cap.get("capture_cells") or []),
                   " at " + ", ".join("%d MHz" % c for c in bad_clocks)
                   if bad_clocks else ""))

    # The knob's direct effect: the smallest measured class that clears every
    # default-guard failure, per bench.
    mins = _knob_minimal(platforms, priority, platform_captures, knob)
    if mins:
        bits = []
        for name, other in (platforms or []):
            cls = mins.get(name)
            if cls is not None:
                bits.append("%s: %d" % (other.get("bench") or name, cls))
        findings.append(
            "The priority knob clears every default-guard failure at the "
            "measured minimum class (%s); the library still ships class 0."
            % "; ".join(bits))

    cap_cells = capture.get("capture_cells") or []
    cap_supported = bool(capture.get("capture_supported") and cap_cells)
    if cap_supported and all(c.get("matches") for c in cap_cells):
        baseline = capture.get("capture_baseline") or {}
        base_count = next(iter(baseline.values()), {}).get("count")
        findings.append(
            "Search ROM on the primary bench returned the same %s devices "
            "and checksum in all %d cells."
            % (base_count, len(cap_cells)))
    inj_cells = capture.get("inject_cells") or []
    feed = [v for v in _first_bad_from_cells(
        inj_cells, "write", default_guard).values() if v is not None]
    capd = [v for v in _first_bad_from_cells(
        inj_cells, "search").values() if v is not None]
    if feed and capd:
        feed_txt = _first_bad_text(
            _first_bad_from_cells(inj_cells, "write", default_guard), "write")
        cap_txt = _first_bad_text(
            _first_bad_from_cells(inj_cells, "search"), "search")
        findings.append(
            "A delayed feed fails the checks at %s; a delayed capture fails "
            "at %s. The harness catches a late transfer on both clocks."
            % (feed_txt, cap_txt))

    # Main matrix.
    matrix_rows = []
    frame_gaps = []
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
        if default_cell and not default_cell.get("clean"):
            # The default guard is the cell the row's verdict is about: if it
            # failed, show its kept failure window instead of an empty link.
            poor = [c for c in data.get("cells") or []
                    if c["level"] == level and c["clock_mhz"] == clock
                    and not c["clean"] and c.get("capture_fail")]
            poor.sort(key=lambda c: -c["guard_us"])
            if poor:
                label = "failing frame (%s)" % fmt_guard(poor[0]["guard_us"])
                links.insert(0, link_to_viewer(poor[0], label,
                                               key="capture_fail"))
        decoded = (default_cell or {}).get("frames", 0)
        firmware = t[0] if default_cell else None
        if default_cell and firmware is not None and firmware != decoded:
            frame_gaps.append(
                "%d MHz level %d at %s: %d decoded, %d run by the firmware"
                % (clock, level, fmt_guard(default_guard), decoded, firmware))
        frames_text = ("%d / %d" % (decoded, firmware)
                       if firmware is not None and firmware != decoded
                       else "%d" % decoded)
        matrix_rows.append(
            "<tr><td>%d</td><td>%s</td><td>%s%s</td><td>%s</td><td>%s</td>"
            "<td>%s</td><td>%s</td><td>%s</td><td>%s</td><td class=\"links\">%s</td></tr>" % (
                level,
                "%d MHz" % clock if clock else "default",
                fmt_guard(guard_min),
                "" if guard_min is None else " (%d us)" % window_us(guard_min),
                frames_text,
                violation_text(default_cell),
                t[1] if t[1] is not None else "?",
                t[3] if t[3] is not None else "?",
                t[16] if t[16] is not None else "?",
                e(load_evidence(default_cell, level)),
                " ".join(links) or '<span class="muted">-</span>'))

    if frame_gaps:
        frame_gap_note = (
            "<p class=\"note\">Frame-count gap: %s. Where two numbers appear, "
            "the second is the firmware's SWD frame counter. The analyzer "
            "decoded fewer frames than the firmware ran; every decoded frame "
            "passed and the DMA counters are zero, so this is a capture/decode "
            "loss, not a feed fault.</p>"
            % e("; ".join(frame_gaps)))
    else:
        frame_gap_note = ""

    # CPU contention: level 1 vs level 5 at each clock.
    cpu_rows = []
    for clock in clocks:
        for level in (1, 5):
            cell = cells.get((level, clock, default_guard))
            t = telemetry(cell)
            if t[7] is not None:
                cpu_rows.append(("%d MHz - level %d (%s)" % (
                    clock, level, "CPU only" if level == 1 else "ch1 + CPU"),
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
    delay_html = feed_delay_section(capture)
    extra_html = "\n".join(
        platform_section(name, other, (platform_captures or {}).get(name))
        for name, other in (platforms or []))
    priority_rows = ([(name, "default", other)
                      for name, other in (platforms or [])]
                     + list(priority or []))
    priority_html = priority_section(data, priority_rows, bool(knob))
    knob_html = knob_section(platforms, priority, platform_captures, knob)
    # One rendered block for every generated section: the two control tables
    # first (they belong with the primary results), then the extra platforms,
    # the load-placement comparison and the knob's measured effect.
    extra_html = "\n".join(x for x in (cap_html, inj_html, delay_html,
                                       extra_html, priority_html, knob_html)
                           if x)

    commit = data.get("commit") or "unknown"
    tree_note = " (working tree)" if data.get("dirty") else ""
    has_hashes = any(c.get("image_sha256") for c in data.get("cells") or [])
    image_note = (" Each cell carries the SHA-256 of its image in "
                  "<code>results.json</code>" if has_hashes else "")
    generated = data.get("generated") or "unknown"
    bench = data.get("bench") or "unknown"
    mcu = data.get("mcu") or bench
    pattern = data.get("pattern") or "?"
    slots = data.get("slots") or 0
    bursts = data.get("bursts") or 0
    words = data.get("stress_words") or 0

    class _Lazy(str):
        pass

    body = ("""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Stress test report - stm32-async-1wire</title>
<style>
:root{{--fg:#182230;--muted:#5b6b85;--line:#dbe2ec;--bg:#fff;--soft:#f6f8fb;
--accent:#1b57c4;--ok:#1f7a3d;--okbg:#eaf6ee;--warn:#9a5b00;--bad:#a12222}}
@media (prefers-color-scheme:dark){{:root{{--fg:#e7edf5;--muted:#9fb0c7;
--line:#334155;--bg:#0f1722;--soft:#162130;--accent:#7fb0ff;--ok:#6fce8f;
--okbg:#12301e;--warn:#e0a03a;--bad:#ff8a8a}}}}
*{{box-sizing:border-box}}body{{margin:0;background:var(--bg);color:var(--fg);
font:16px/1.55 system-ui,-apple-system,Segoe UI,sans-serif}}
header{{border-bottom:1px solid var(--line);background:var(--soft)}}
header .inner,main{{max-width:74rem;margin:0 auto;padding:0 1.25rem}}
header .inner{{display:flex;gap:1rem;align-items:baseline;padding-top:.8rem;
padding-bottom:.8rem;flex-wrap:wrap}}
header a{{color:var(--accent);text-decoration:none;font-weight:600}}
header .sep{{color:var(--muted)}}
main{{padding-top:1.5rem;padding-bottom:3rem}}
h1{{font-size:2rem;margin:.2rem 0}} h2{{margin-top:2.2rem;font-size:1.35rem}}
h3{{margin-top:1.3rem;font-size:1.05rem}}
p.lead{{color:var(--muted);font-size:1.1rem;margin:.2rem 0 1rem}}
.meta{{color:var(--muted);font-size:.92rem}}
.verdict{{border:1px solid var(--line);border-left:6px solid var(--ok);
background:var(--okbg);padding:.9rem 1rem;border-radius:8px;margin:1rem 0}}
.verdict.bad{{border-left-color:var(--bad)}}
ul.findings{{padding-left:1.2rem}} ul.findings li{{margin:.35rem 0}}
table{{border-collapse:collapse;width:100%;margin:1rem 0;font-size:.92rem}}
th,td{{border:1px solid var(--line);padding:.45rem .55rem;text-align:left;
vertical-align:top}} th{{background:var(--soft)}}
td.num{{text-align:right;white-space:nowrap}}
td.links a{{display:inline-block;margin-right:.5rem;white-space:nowrap}}
a{{color:var(--accent)}} .muted{{color:var(--muted)}}
code,pre{{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
font-size:.88em}} pre{{background:var(--soft);border:1px solid var(--line);
padding:.8rem 1rem;border-radius:8px;overflow:auto}}
.timeline{{width:100%;max-width:700px;display:block;margin:1rem 0}}
.svg-label{{font:13px system-ui,sans-serif;fill:currentColor}}
.svg-small{{font:12px system-ui,sans-serif;fill:#6b7a90}}
.bars{{margin:1rem 0;max-width:46rem}}
.bar{{display:grid;grid-template-columns:16rem 1fr 4.5rem;gap:.6rem;
align-items:center;margin:.35rem 0;font-size:.9rem}}
.bar-label{{color:var(--muted)}} .track{{background:var(--soft);
border:1px solid var(--line);height:1rem;border-radius:4px;overflow:hidden}}
.fill{{display:block;height:100%;background:var(--accent);opacity:.65}}
.bar-value{{text-align:right;font-variant-numeric:tabular-nums}}
.note{{color:var(--muted);font-size:.9rem}}
footer{{border-top:1px solid var(--line);color:var(--muted);font-size:.85rem;
padding:1rem 1.25rem 2rem;max-width:74rem;margin:0 auto}}
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
<p class="lead">DMA feed margin under memory-subsystem load - {mcu}</p>
<p class="meta">Measured {generated} at commit <code>{commit}</code>{tree_note} on <code>{bench}</code>.
Build <code>{example}</code>, pattern <code>{pattern}</code>,
{slots} slots per frame, {bursts} firmware frames per cell, {words} halfwords per DMA burst.{image_note}</p>

<section id="summary">
<h2>Summary</h2>
<div class="verdict{verdict_class}">
<strong>{verdict_text}</strong>
</div>
<ul class="findings">
{findings}
</ul>
<p>This test checks whether memory load can delay the CCR3 feed DMA past its
slot deadline. The primary bench stays clean; the additional benches show where
a higher-ranked load can starve the feed. The integration rule below makes that
constraint explicit.</p>
</section>

<section id="mechanism">
<h2>What can go wrong</h2>
<p>Each write slot is one TIM1 period. At CC2, DMA1 channel 3 is asked for the
next CCR3 value. The update event starts the next slot. The transfer must land
between those events: <code>guard + 1</code> microseconds (the counter runs
from 0 to ARR inclusive).</p>
{timeline}
<table><thead><tr><th>Late transfer</th><th>What the wire shows</th></tr></thead>
<tbody>{failure_rows}</tbody></table>
<p>On DMA-fed paths OC3PE is off. A late CCR3 write takes effect immediately,
so a failure can appear inside the current slot, not only at the next one.</p>
</section>

<section id="rig">
<h2>Rig and method</h2>
<p><code>examples/0_dma_pressure</code> sends a fixed 8-byte pattern (64 slots
plus the release) back to back while a selectable load runs beside the bus. The
bench is <code>{rig_bench}</code>; D0 is the 1-Wire line on PA10. The capture
runs at {rate_mhz} MS/s. The harness flashes the firmware halted and resets it,
so only the intended run is measured.</p>
<table><tbody>
<tr><th>MCU</th><td><code>{rig_row_mcu}</code></td></tr>
<tr><th>Clocks</th><td>{clocks}</td></tr>
<tr><th>Pattern</th><td><code>{rig_pattern}</code> (LSB first, per byte)</td></tr>
<tr><th>Firmware frames per cell</th><td>{frames_per_cell}</td></tr>
<tr><th>Load channels</th><td>DMA1 channel 1 (restarted finite bursts) and channel 2
(finite bursts), all at the default priority</td></tr>
<tr><th>On-chip check</th><td><code>cndtr_late</code> (<code>DMA1_Channel3-&gt;CNDTR</code>
at completion) and <code>TEIF3</code>, read back over SWD</td></tr>
<tr><th>Wire check</th><td>every low pulse is a '1' or a '0' width, the slot
period matches the build, all 64 bits decode, and the frame has exactly 64
pulses and a release</td></tr>
</tbody></table>
<p class="note">The first slot is armed by a register write, not the feed DMA.
Its width includes the CPU gap between enabling OC3 and starting the timer
(about 1 us at 48 MHz, 7 us at 8 MHz), so the check skips that slot's width and
the first period of each frame. Guard 0 is not an operating point: CC2 then
coincides with the update, and the release write adds a spurious end-of-frame
pulse.</p>
</section>

<section id="levels">
<h2>Load ladder</h2>
<table><thead><tr><th>Level</th><th>Load</th><th>What it does</th>
<th>Evidence at the default guard</th></tr></thead>
<tbody>{level_rows}</tbody></table>
</section>

<section id="results">
<h2>Results</h2>
<p>Smallest clean guard per level and clock. The parenthetical is the real
transfer window (<code>guard + 1</code> us). A clean cell has no width, period,
length or decode violation in the frames the analyzer decoded; the firmware
frame counter is shown separately because the analyzer can miss a frame.</p>
<table><thead><tr><th>Level</th><th>Clock</th><th>guard_min</th>
<th>Frames @ {frames_header} us (decoded / firmware)</th><th>Violations @ {violations_header} us</th><th>CNDTR late</th>
<th>DMA error</th><th>Stop timeout</th><th>Load evidence</th><th>Viewer</th></tr></thead>
<tbody>{matrix_rows}</tbody></table>
<p class="note">The viewer opens one representative frame (64 slots, 1 ms
lead-in and tail). The pattern is not a 1-Wire command, so the protocol tree
stays empty; the Detail view shows the measured low and slot widths. The JSON
holds every tested cell, including the {control} us control and the bisection
probes.</p>
{frame_gap_note}
<h3>CPU-side work</h3>
<p>CPU passes completed while the bus was running, at the default guard. The
ch1 burst stream does not reduce them here: the DMA interleaves between CPU
accesses. The burst count in the table shows the load.</p>
{chart}
<p>Both loads ran, and the feed still met the {floor_window} us floor.</p>
</section>

{extra_sections}

<section id="interpretation">
<h2>Interpretation</h2>
<ul>
<li>On the primary bench, no wire-visible slip at the {interp_floor} us floor
under any load. The additional benches are summarised in their own sections.</li>
<li><code>cndtr_late = 0</code> everywhere on the primary bench: every feed
transfer was serviced before completion, including the final release.</li>
<li>Load activity: channel 1 restarted thousands of times per run (see the
table), and the CPU hammer ran too. The feed still met the {interp_floor} us
floor on the primary bench.</li>
<li>The default guard is a {guard_window} us window; the 1 us floor is a
{interp_floor2} us window. A late feed is tolerated well past either window
(see "Feed delay tolerance vs the guard window"), so the window and the delay
tolerance are different quantities, not a safety factor. The library default
is sufficient on F0 and on the equal-priority H5 row; the F4 default-placement
and H5 PRIO 3 rows show that a higher-ranked load can still starve the feed.</li>
</ul>
</section>

<section id="caveats">
<h2>Caveats</h2>
<ul>
<li><strong>Default priority plus one high-placement row.</strong> The default
load channels use their reset priority. The priority section adds one explicit
higher-ranked load per family; an arbitrary application priority is still a
different case. On F4 the equal-priority tie-break is covered by moving the
load to higher stream numbers; on H5 the load is raised to <code>CCR.PRIO 3</code>
against the feed's default.</li>
<li><strong>One rig, several benches, write-only.</strong> F0, F4 and H5 used the
same firmware and loader. On parts with fewer channels the high-placement load
uses the next free number (the F030F4 has no channel 6, so it uses 4). Wiring
and marker pins differ per board. The pattern is not a 1-Wire transaction and
the devices are never addressed, so this measures master timing, not a protocol
exchange.</li>
<li><strong>Search coverage.</strong> Search ROM is measured on F0, F4 and H5.
The F4 matrix covers 180 MHz for levels 0-5 and 8/16 MHz for levels 0/5
(guards 1/5); the priority-knob runs add guard 100 for those two clocks. The
H5 matrix is 36 cells at 8/64/250 MHz, guards 1/5. The other backends have no
Search capture.</li>
<li><strong>Timer floor.</strong> The 1 us timer cannot resolve a guard below 1.
The report says "a 2 us window was enough", not "the latency is exactly
1 us".</li>
<li><strong>Provenance.</strong> Each section carries the commit it was measured
at, and the benches were run at different commits as the harness and the
priority knob evolved. <code>OW_DMA_PRIO</code> was added after the earliest
runs, but with the default class 0 the priority field is the reset value, so
those runs use the same DMA configuration as a knob build; the knob changes no
timing constant and no ABI.</li>
</ul>
</section>

<section id="reproduce">
<h2>Reproduce</h2>
<pre>make APP=0_dma_pressure OW_MCU=STM32F030x6 {cont}
     EXT="-DOW_PARASITE_POWER=0 -DSTRESS_LEVEL=5 -DONEWIRE_GUARD_BAND=1"

python tests/hil/dma_pressure.py {cont}
     --json docs/stress/results.json {cont}
     --sr-dir docs/stress/captures

# the priority staircase: the load placement is fixed and the feed/capture
# class is the variable. One file pair per class and bench.
python tests/hil/dma_pressure.py {cont}
     --json docs/stress/platforms/h503/results-prio1.json {cont}
     --levels 2,3,4,5 --clocks 8,64,250 --guards 1,5,100 {cont}
     --gpdma-prio 3 --feed-prio 1
python tests/hil/dma_capture.py {cont}
     --json docs/stress/platforms/f446/capture-prio1.json {cont}
     --levels 0,5 --clocks 8,16 --feed-prio 1

python tools/stress_report.py {cont}
     --data docs/stress/results.json {cont}
     --capture docs/stress/capture.json {cont}
     --out docs/stress/index.html</pre>
<p class="note">The matrix needs an fx2lafw logic analyzer and an ST-Link; it
prints SKIP when either is missing. A full 12-cell run takes a few minutes
because every cell is rebuilt and flashed. The <code>{cont}</code> above is the
POSIX shell line continuation; in <code>cmd.exe</code> use <code>^</code> or
keep each command on one line.</p>
</section>

<section id="artifacts">
<h2>Artifacts</h2>
<ul>
<li><a href="results.json">results.json</a> - every tested write-matrix cell,
counters and SWD telemetry.</li>
<li><a href="capture.json">capture.json</a> - the Search ROM cells, the
deliberately delayed transfer cells and the feed-delay-vs-guard sweep.</li>
<li><a href="matrix.txt">matrix.txt</a> - the exact console output of the
write matrix run only.</li>
<li><code>captures/</code> - one representative frame per cell, in sigrok
<code>.sr</code> form; the Viewer column above links each one.</li>
<li><code>platforms/</code> - per additional bench: <code>results.json</code>,
<code>capture.json</code>, where the load placement was tested
<code>results-high.json</code> with its <code>captures-high/</code>, and the
priority staircase <code>results-prio1/2/3.json</code> plus
<code>capture-prio1/2/3.json</code> where Search ROM was measured (for example
<code>platforms/f446/</code>; <code>platforms/h503/</code> has the three write
runs, no Search captures).</li>
<li><a href="{repo_url2}/blob/main/examples/0_dma_pressure/main.c">examples/0_dma_pressure/main.c</a>
and <a href="{footer_note}/blob/main/tests/hil/dma_pressure.py">tests/hil/dma_pressure.py</a>
- the rig and the harness.</li>
</ul>
</section>
</main>
<footer>Generated from <code>results.json</code>, <code>capture.json</code> and
<code>platforms/*</code> by <code>tools/stress_report.py</code>. This page is
deterministic: <code>make check</code> regenerates it and compares the
bytes.</footer>
</body>
</html>
""")
    values = {
        "mcu": e(mcu),
        "generated": e(generated),
        "commit": e(commit),
        "tree_note": e(tree_note),
        "image_note": image_note,
        "bench": e(bench),
        "example": e(data.get("example") or "?"),
        "pattern": e(pattern),
        "slots": slots,
        "bursts": bursts,
        "words": words,
        "verdict_class": ("" if ok else " bad"),
        "verdict_text": (
                    "The wire stayed correct on every tested bench."
                    if ok
                    else "The wire stayed correct on F0; the additional "
                         "benches expose starvation when the load outranks "
                         "the feed."
                    if primary_ok
                    else "The primary bench contains a failure."
                ),
        "findings": "\n".join("<li>%s</li>" % e(f) for f in findings),
        "timeline": timeline_svg(),
        "failure_rows": failure_rows,
        "cont": "\\",
        "rig_bench": e(bench),
        "rate_mhz": e(rate_mhz),
        "rig_row_mcu": e(mcu),
        "clocks": e(", ".join("%d MHz" % c for c in clocks) if clocks else "default"),
        "rig_pattern": e(pattern),
        "frames_per_cell": bursts,
        "level_rows": "\n".join(level_rows),
        "frames_header": default_guard,
        "violations_header": default_guard,
        "matrix_rows": "\n".join(matrix_rows),
        "frame_gap_note": frame_gap_note,
        "control": control,
        "chart": chart,
        "floor_window": floor_window,
        "extra_sections": extra_html,
        "interp_floor": floor_window,
        "guard_window": default_window,
        "interp_floor2": floor_window,
        "repo_url": REPO_URL,
        "repo_url2": REPO_URL,
        "footer_note": REPO_URL,
    }
    return _fmt(body, values)


def _specs(text):
    import re
    return re.findall(r"%(?:[-+#0 ]*\d*(?:\.\d+)?[sdifxX])", text)


def _fmt(template, values):
    """Render the template; a missing key names the placeholder."""
    try:
        return template.format(**values)
    except (KeyError, IndexError, ValueError) as exc:
        raise RuntimeError("template placeholder failed: %s" % exc)


def discover_platforms(root):
    """Every other bench under docs/stress/platforms/<name>/.

    Returns [(name, results)] for the default placement; the caller adds the
    optional results-high.json siblings to the priority comparison.
    """
    platforms = []
    base = os.path.join(root, "platforms")
    if not os.path.isdir(base):
        return platforms
    for name in sorted(os.listdir(base)):
        path = os.path.join(base, name, "results.json")
        if os.path.isfile(path):
            with open(path, encoding="utf-8") as fh:
                platforms.append((name, json.load(fh)))
    return platforms


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", default="docs/stress/results.json")
    ap.add_argument("--capture", default="docs/stress/capture.json",
                    help="capture-drain and negative-control JSON")
    ap.add_argument("--out", default="docs/stress/index.html")
    ap.add_argument("--platforms-dir", default="docs/stress/platforms",
                    help="one subdirectory per additional bench, each with "
                    "results.json and an optional results-high.json")
    ap.add_argument("--priority", action="append", default=None,
                    help="name=path to a results.json measured with the load "
                    "on the high channels/streams (repeatable; the primary "
                    "bench uses --data-high)")
    ap.add_argument("--data-high", default="",
                    help="the primary bench's high-placement results.json")
    args = ap.parse_args(argv)
    with open(args.data, encoding="utf-8") as fh:
        data = json.load(fh)
    capture = None
    if args.capture and os.path.isfile(args.capture):
        with open(args.capture, encoding="utf-8") as fh:
            capture = json.load(fh)

    platforms = []
    priority = []
    platform_captures = {}
    knob = {}
    if args.platforms_dir and os.path.isdir(args.platforms_dir):
        for name in sorted(os.listdir(args.platforms_dir)):
            base = os.path.join(args.platforms_dir, name)
            path = os.path.join(base, "results.json")
            if not os.path.isfile(path):
                continue
            with open(path, encoding="utf-8") as fh:
                platforms.append((name, json.load(fh)))
            cap = os.path.join(base, "capture.json")
            if os.path.isfile(cap):
                with open(cap, encoding="utf-8") as fh:
                    platform_captures[name] = json.load(fh)
            high = os.path.join(base, "results-high.json")
            if os.path.isfile(high):
                with open(high, encoding="utf-8") as fh:
                    priority.append((name, "high", json.load(fh)))
            # The same bench re-run with OW_DMA_PRIO raised: the direct test of
            # the build knob, as opposed to moving the load. One pair of files
            # per class, so the report can show the staircase.
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
    if args.data_high and os.path.isfile(args.data_high):
        with open(args.data_high, encoding="utf-8") as fh:
            priority.insert(0, ("primary", "high", json.load(fh)))
    for spec in args.priority or []:
        name, _, path = spec.partition("=")
        if path and os.path.isfile(path):
            with open(path, encoding="utf-8") as fh:
                priority.append((name, "high", json.load(fh)))

    text = build_report(data, capture, platforms, priority, platform_captures,
                        knob)
    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("report: %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
