#!/usr/bin/env python3
"""Host-side tests for the DMA-pressure capture analysis - no bench needed.

Run: python tests/hil/test_dma_pressure.py   (make test-hil-parsing)

Why this exists: the HIL matrix decides whether the feed DMA met its deadline
from the wire alone, so the analyser has to go red on every failure shape the
bench can produce: a stretched pulse, a doubled edge, a missing slot, a lost
bit, a stalled line and a build with the wrong guard band. The cases below
synthesize each shape at a known geometry and check the verdict.
"""

import os
import sys

try:
    import numpy as np
except ImportError:
    print("test_dma_pressure: SKIP (numpy not installed)")
    sys.exit(0)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import dma_pressure as dp  # noqa: E402

RATE = 1_000_000.0
US_PER_SAMPLE = 1e6 / RATE  # 1 us
SLOT_US = dp.ONE_US + dp.ZERO_US + 2  # one guard tick plus the 0..ARR counter

failures = []


def check(what, condition):
    if condition:
        print("  ok   %s" % what)
    else:
        print("  FAIL %s" % what)
        failures.append(what)


def waveform(frames=3, slot_us=SLOT_US, one_us=dp.ONE_US, zero_us=dp.ZERO_US,
             gap_us=1000.0, edit=None, stuck=False, marker=False):
    """One byte per sample, D0 in bit 0, like sigrok's `-O binary` output.

    `marker=True` drives bit 1 high for every frame sample, so the analyser can
    bracket frames by D1 instead of by the idle gap.
    """
    bits = [1] * int(round(gap_us / US_PER_SAMPLE))
    total = len(dp.PATTERN) * 8
    for frame in range(frames):
        for idx in range(total):
            bit = (dp.PATTERN[idx // 8] >> (idx % 8)) & 1
            low = one_us if bit else zero_us
            segs = [(0, low), (1, slot_us - low)]
            if stuck and frame == frames - 1 and idx == total - 1:
                segs = [(0, slot_us)]
            if edit:
                segs = edit(frame, idx, segs)
            for level, us in segs:
                value = (level | 2) if marker else level
                bits.extend([value] * int(round(us / US_PER_SAMPLE)))
        bits.extend([0 if (stuck and frame == frames - 1) else 1]
                    * int(round(gap_us / US_PER_SAMPLE)))
    return np.array(bits, dtype=np.uint8)


def run(raw, **kwargs):
    return dp.analyse(raw, RATE, slot_us=kwargs.pop("slot_us", SLOT_US), **kwargs)


def main():
    clean = run(waveform())
    check("clean waveform: all frames pass", clean["clean"] and clean["frames"] == 3)
    check("clean waveform: no counters move",
          clean["bad_len"] == clean["bad_width"] == clean["bad_bits"]
          == clean["bad_period"] == 0)
    check("clean waveform: no stuck line", not clean["stuck_low"])

    def stretch(frame, idx, segs):
        if frame == 0 and idx == 8:  # first bit of 0xFF: a '1' slot
            return [(0, dp.ZERO_US), (1, SLOT_US - dp.ZERO_US)]
        return segs

    r = run(waveform(edit=stretch))
    check("stretched '1' pulse is caught as a wrong bit",
          not r["clean"] and r["bad_bits"] == 1)

    def shorten(frame, idx, segs):
        if frame == 0 and idx == 0:  # first bit of 0x00: a '0' slot
            return [(0, dp.ONE_US), (1, SLOT_US - dp.ONE_US)]
        return segs

    r = run(waveform(edit=shorten))
    check("one slot replaced by the other bit is caught",
          not r["clean"] and r["bad_bits"] == 1 and r["bad_width"] == 0)

    def invalid_width(frame, idx, segs):
        if frame == 0 and idx == 2:
            return [(0, 30.0), (1, SLOT_US - 30.0)]
        return segs

    r = run(waveform(edit=invalid_width))
    check("a pulse that is neither bit is caught",
          not r["clean"] and r["bad_width"] == 1)

    def double(frame, idx, segs):
        if frame == 0 and idx == 1:
            return [(0, dp.ZERO_US), (1, 2.0), (0, 3.0), (1, 1.0)]
        return segs

    r = run(waveform(edit=double))
    check("doubled edge is caught", not r["clean"] and r["bad_len"] == 1)

    def missing(frame, idx, segs):
        if frame == 0 and idx == 1:
            return [(1, SLOT_US)]
        return segs

    r = run(waveform(edit=missing))
    check("missing slot is caught", not r["clean"] and r["bad_len"] == 1)

    r = run(waveform(frames=2, stuck=True))
    check("line parked low is caught", not r["clean"] and r["stuck_low"])

    r = run(waveform(slot_us=71), slot_us=66)
    check("wrong guard band (stale build) is caught",
          not r["clean"] and r["bad_period"] >= 1)

    r = run(waveform(), min_start_us=6000.0)
    check("frames from before the harness reset are ignored", r["frames"] == 2)

    r = run(waveform(frames=1), min_start_us=1e9)
    check("a capture with no frame after the reset is not clean",
          not r["clean"] and r["frames"] == 0)

    # The first slot is armed by software: its pulse contains the CPU gap
    # between enabling OC3 and starting the timer, so it is wider than a
    # nominal slot and the first period is longer. Neither is a feed
    # violation, and the analyser must skip both.
    def first_slot_wide(frame, idx, segs):
        if idx == 0:
            return [(0, dp.ZERO_US + 7.0), (1, SLOT_US - dp.ZERO_US)]
        return segs

    r = run(waveform(edit=first_slot_wide))
    check("wide first slot is ignored", r["clean"])

    # A board's bus-pin init can emit a lone sub-microsecond pulse well before
    # the first frame (the F446 and H503 both do). It is not a truncated frame:
    # a real truncation is adjacent to the next frame, this is isolated by a gap
    # much longer than a frame boundary.
    def with_leading_glitch(frames=3):
        glitch = np.array(
            [1] * int(round(1000.0 / US_PER_SAMPLE))
            + [0] * int(round(1.0 / US_PER_SAMPLE))
            + [1] * int(round(5000.0 / US_PER_SAMPLE)), dtype=np.uint8)
        return np.concatenate([glitch, waveform(frames=frames)])

    r = run(with_leading_glitch())
    check("a leading startup glitch is a bad frame by default",
          not r["clean"] and r["bad_len"] == 1 and r["frames"] == 3)
    r = run(with_leading_glitch(), ignore_leading_short=True)
    check("a leading startup glitch is ignored on request",
          r["clean"] and r["frames"] == 3 and r["bad_len"] == 0)

    # With the PA11/D1 marker, the frame is defined by the marker edges, not by
    # an idle gap. Traffic outside the marker (startup glitch, capture tail) is
    # not part of any frame and cannot turn a clean run red.
    r = run(waveform(marker=True), marker_bit=1)
    check("marker: frames are bracketed by D1", r["clean"] and r["frames"] == 3)

    marker_glitch = np.concatenate([
        np.array([1] * 200 + [0] * 2 + [1] * 1000, dtype=np.uint8),
        waveform(marker=True),
        np.array([1] * 1000 + [0] * 20 + [1] * 1000, dtype=np.uint8),
    ])
    r = run(marker_glitch, marker_bit=1)
    check("marker: traffic outside the marker is ignored",
          r["clean"] and r["frames"] == 3 and r["bad_len"] == 0)

    r = run(waveform(marker=True, edit=stretch), marker_bit=1)
    check("marker: a bad pulse inside a frame is still caught",
          not r["clean"] and r["bad_bits"] == 1)

    early_marker = np.array([1 | 2] * 400 + [1] * 400, dtype=np.uint8)
    r = run(np.concatenate([early_marker, waveform(marker=True)]),
            marker_bit=1, min_start_us=500.0)
    check("marker: an empty marker before the run is ignored",
          r["clean"] and r["frames"] == 3)

    r = run(waveform(marker=True), marker_bit=1, min_start_us=1e9)
    check("marker: frames before the run are ignored", r["frames"] == 0)

    if failures:
        print("FAIL: %d check(s) failed" % len(failures))
        return 1
    print("PASS: dma_pressure analysis")
    return 0


if __name__ == "__main__":
    sys.exit(main())
