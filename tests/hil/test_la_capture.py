#!/usr/bin/env python3
"""Host-side tests for the logic-analyzer wait check - no bench needed.

Run: python tests/hil/test_la_capture.py   (make test-hil-parsing)

Why this exists: --wait-ms is the only check that can catch a *shared*
misreading of the repetition counter. The engine and the host model both read
RCR as "periods - 1", so a consistently wrong convention passes every host
test - the two agree by construction, and only the wire can tell. That makes
this the one check whose own failure has to be demonstrated: a check that
cannot go red proves nothing. The cases below feed the analysis synthetic
captures whose answer is known in advance.
"""

import json
import os
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import la_capture  # noqa: E402

RATE = 10000.0  # 100 us per sample; the analysis itself is rate-agnostic
US_PER_SAMPLE = 1e6 / RATE

failures = []


def check(what, condition):
    if condition:
        print("  ok   %s" % what)
    else:
        print("  FAIL %s" % what)
        failures.append(what)


def synthetic_capture(tmpdir, gaps_us, low_us=100.0):
    """Write a sigrok "binary" capture with the given falling-edge spacings.

    One byte per sample, D0 in the least significant bit, the unused channels
    high (as on an idle analyzer). Each entry of `gaps_us` is the distance to
    the next falling edge, so the analysis must report exactly these back as
    the inter-pulse intervals.
    """
    low = max(1, int(round(low_us / US_PER_SAMPLE)))
    samples = bytearray()
    samples.extend([0xFF] * low)  # idle high first, so the first fall is an edge
    for gap_us in gaps_us:
        gap = int(round(gap_us / US_PER_SAMPLE))
        samples.extend([0xFE] * low)  # master pulls the line low
        samples.extend([0xFF] * (gap - low))
    samples.extend([0xFE] * low)  # the pulse that closes the last gap
    samples.extend([0xFF] * low)
    path = os.path.join(tmpdir, "la.bin")
    with open(path, "wb") as f:
        f.write(samples)
    return path


def test_gaps_are_read_back():
    print("the measured gaps")
    with tempfile.TemporaryDirectory() as d:
        path = synthetic_capture(d, [750000.0, 5000000.0])
        widths, gaps = la_capture.analyse(path, RATE, 0)
    check("the long gaps come back as they went in",
          len(gaps) == 2
          and abs(gaps[0] - 750000.0) <= US_PER_SAMPLE
          and abs(gaps[1] - 5000000.0) <= US_PER_SAMPLE)
    check("the low pulse width comes back",
          len(widths) == 3 and abs(widths[0] - 100.0) <= US_PER_SAMPLE)


def test_correct_wait_is_found():
    print("a correct wait")
    measured, doubled = la_capture.wait_measurements(
        np.array([750000.0, 5000000.0]), 750000.0)
    check("the 750 ms wait is found",
          abs(measured - 750000.0) <= la_capture.WAIT_TOLERANCE_US)
    check("no gap sits at twice the wait", doubled is None)


def test_doubled_wait_is_flagged():
    print("a wait one period too long")
    measured, doubled = la_capture.wait_measurements(
        np.array([1500000.0, 5000000.0]), 750000.0)
    check("it is measured as twice the request",
          abs(measured - 1500000.0) <= US_PER_SAMPLE)
    check("and it is flagged as the doubling", doubled is not None
          and abs(doubled - 1500000.0) <= US_PER_SAMPLE)


def test_a_correct_gap_does_not_hide_a_doubled_one():
    print("a capture that spans a resolution change")
    # A correct wait and a doubled one in the same capture: taking the nearest
    # gap would pass the run, so the doubling has to be checked separately.
    measured, doubled = la_capture.wait_measurements(
        np.array([750000.0, 1500000.0, 5000000.0]), 750000.0)
    check("the correct gap is still found",
          abs(measured - 750000.0) <= la_capture.WAIT_TOLERANCE_US)
    check("the doubled gap is still reported", doubled is not None)


def test_a_slow_poll_loop_needs_a_wider_tolerance():
    print("a firmware that polls slowly")
    # 760 ms measured against a 750 ms plan: the wait itself is right, but the
    # application notices the completion on its 10 ms poll boundaries.
    measured, _ = la_capture.wait_measurements(
        np.array([760000.0, 5000000.0]), 750000.0, 2000.0)
    check("the default tolerance would reject it",
          abs(measured - 750000.0) > la_capture.WAIT_TOLERANCE_US)
    check("a wider tolerance accepts it",
          abs(measured - 750000.0) <= 20000.0)


def test_the_doubling_check_follows_the_tolerance():
    print("the tolerance on the doubling check")
    # 1.52 s is 20 ms above twice the request: not a doubling at +-2 ms, but
    # it is at +-30 ms. The tolerance must reach both checks, not only one.
    gaps = np.array([750000.0, 1520000.0])
    _, strict = la_capture.wait_measurements(gaps, 750000.0, 2000.0)
    _, wide = la_capture.wait_measurements(gaps, 750000.0, 30000.0)
    check("20 ms off the doubling is not flagged at +-2 ms", strict is None)
    check("and is flagged at +-30 ms", wide is not None)


def test_capture_without_the_wait_cannot_pass():
    print("a capture without the conversion wait")
    measured, _ = la_capture.wait_measurements(
        np.array([166.0, 166.0, 5000000.0]), 750000.0)
    check("the nearest gap is nowhere near the request",
          abs(measured - 750000.0) > la_capture.WAIT_TOLERANCE_US)


def test_empty_capture():
    print("an idle capture")
    measured, doubled = la_capture.wait_measurements(np.array([]), 750000.0)
    check("there is nothing to measure", measured is None and doubled is None)


def test_capture_meta_records_what_the_samples_cannot():
    print("the capture sidecar")
    check("the rate spelling parses",
          la_capture.parse_rate("4M") == 4e6
          and la_capture.parse_rate("8k") == 8e3
          and la_capture.parse_rate("4000000") == 4e6)
    with tempfile.TemporaryDirectory() as d:
        path = synthetic_capture(d, [750000.0])
        meta_path = la_capture.write_meta(
            path, 4000000.0, 0, 30, "sigrok-cli")
        with open(meta_path, encoding="utf-8") as fh:
            meta = json.load(fh)
        check("the sidecar is tied to the bytes",
              meta["sha256"] == la_capture.sha256_file(path)
              and meta["bytes"] == os.path.getsize(path))
    check("the sidecar names the rate and the channel",
          meta["rate_hz"] == 4000000.0 and meta["channel"] == 0)
    check("the duration and the tool are kept",
          meta["seconds"] == 30 and meta["tool"] == "sigrok-cli")


def main():
    test_gaps_are_read_back()
    test_correct_wait_is_found()
    test_doubled_wait_is_flagged()
    test_a_correct_gap_does_not_hide_a_doubled_one()
    test_a_slow_poll_loop_needs_a_wider_tolerance()
    test_the_doubling_check_follows_the_tolerance()
    test_capture_without_the_wait_cannot_pass()
    test_empty_capture()
    test_capture_meta_records_what_the_samples_cannot()
    if failures:
        print("test_la_capture: FAILED (%d)" % len(failures))
        return 1
    print("test_la_capture: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
