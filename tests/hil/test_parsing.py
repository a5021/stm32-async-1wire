#!/usr/bin/env python3
"""Parsing tests for the HIL harness: a torn UART line must not fail a run.

Run: python tests/hil/test_parsing.py   (make test-hil-parsing)

Why this exists: the capture can tear a line in the middle of a burst - the
tail is lost, or the next line is glued onto it. On the F103 parasite bench
that happened to the low-power mode line, and the checks compared lines for
equality, so one lost tail failed the whole run. Worse, a torn *measurement*
line was reported as a sensor error, because "not a temperature" was read as
"an error" - a line the capture mangled was counted as a device that failed.

The cases below replay the artefact that was actually captured
(bench_logs/hil/lowpower_*.log), not a synthetic one.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import common  # noqa: E402
import lowpower  # noqa: E402

# The captured artefact: the mode line lost its tail and the next line was
# glued onto it. The lone replacement character is the byte the capture could
# not vouch for.
TORN_MODE_LINE = (
    "OW_LOW_POWER enabled - WFE sleep on stages > 1"
    "�"
    "  ROM: 28 20 78 92 07 00 00 67"
)

# A measurement line cut off in the middle of the value.
TORN_MEASUREMENT = "28 20 78 92 07 00 00 67: 26."

# A measurement line whose value is a real error phrase.
REAL_ERROR = "28 20 78 92 07 00 00 67: CRC check failed."

# A measurement line whose value is a real temperature.
REAL_MEASUREMENT = "28 20 78 92 07 00 00 67: 26.2 C"

failures = []


def check(what, condition):
    if condition:
        print("  ok   %s" % what)
    else:
        print("  FAIL %s" % what)
        failures.append(what)


def test_line_matches():
    print("line_matches")
    check(
        "a torn mode line still matches the mode text",
        common.line_matches(TORN_MODE_LINE, lowpower.MODE),
    )
    check(
        "an intact line matches",
        common.line_matches(lowpower.MODE, lowpower.MODE),
    )
    check(
        "a line with the next one glued on still matches",
        common.line_matches(lowpower.MODE + "  ROM: 28 20 78", lowpower.MODE),
    )
    check(
        "a head-torn line does not match",
        not common.line_matches("sleep on stages > 1ms", lowpower.MODE),
    )
    check(
        "an unrelated line does not match",
        not common.line_matches("Found 7 device(s). Measuring each in turn.",
                                lowpower.MODE),
    )
    check(
        "a short fragment does not match a banner",
        not common.line_matches("DS", lowpower.BANNER),
    )


def test_parse_measurement():
    print("parse_measurement")
    m = common.parse_measurement(TORN_MEASUREMENT)
    check("a torn measurement is not a device error", m is None)

    m = common.parse_measurement(REAL_ERROR)
    check("a real error is still an error", m is not None and m[2] is not None)

    m = common.parse_measurement(REAL_MEASUREMENT)
    check(
        "a real reading is still a reading",
        m is not None and m[1] == 26.2 and m[2] is None,
    )

    m = common.parse_measurement(
        "28 20 78 92 07 00 00 67: no sensor detected."
    )
    check("the long error wording is an error", m is not None and m[2] is not None)

    m = common.parse_measurement("28 20 78 92 07 00 00 67: CRC fail")
    check("the short error wording is an error", m is not None and m[2] is not None)


def test_census():
    print("census line")
    m = common.FOUND_RE.match("Found 7 device(s). Measuring each in turn.")
    check("an intact census line gives the count", m is not None and m.group(1) == "7")
    m = common.FOUND_RE.match("Found 7 dev")
    check("a torn census line still gives the count", m is not None and m.group(1) == "7")
    m = common.FOUND_RE.match("Found device(s)")
    check("a census line with no count gives none", m is None)


class _StubSerial:
    """The slice of pyserial the LineReader uses, fed one canned capture."""

    def __init__(self, data):
        self._data = data
        self.timeout = 0.0

    def read(self, _size):
        data, self._data = self._data, b""
        return data


class _Ctx:
    def __init__(self, seed):
        self.seed = seed
        self.window = 0.05
        self.lines = []


def test_lowpower_check_survives_a_torn_banner():
    print("lowpower check, end to end")
    capture = (
        "DS18B20 7_low_power (low power) starting...\r\n"
        "Searching 1-Wire bus...\r\n"
        + TORN_MODE_LINE
        + "\r\n"
        "28 20 78 92 07 00 00 67: 26.2 C\r\n"
    ).encode("ascii", "replace")

    ctx = _Ctx(b"")
    ok, detail = lowpower.run(_StubSerial(capture), ctx)
    check("the check passes on a torn capture (%s)" % detail, ok)

    # The same capture without the tear must pass too - the tolerance must not
    # have broken the ordinary case.
    capture = (
        "DS18B20 7_low_power (low power) starting...\r\n"
        "Searching 1-Wire bus...\r\n"
        "OW_LOW_POWER enabled - WFE sleep on stages > 1ms\r\n"
        "28 20 78 92 07 00 00 67: 26.2 C\r\n"
    ).encode("ascii", "replace")
    ctx = _Ctx(b"")
    ok, detail = lowpower.run(_StubSerial(capture), ctx)
    check("the check passes on an intact capture (%s)" % detail, ok)


def main():
    test_line_matches()
    test_parse_measurement()
    test_census()
    test_lowpower_check_survives_a_torn_banner()
    if failures:
        print("test_parsing: FAILED (%d)" % len(failures))
        return 1
    print("test_parsing: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
