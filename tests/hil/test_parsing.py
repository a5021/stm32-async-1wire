#!/usr/bin/env python3
"""Parsing tests for the HIL harness: a mangled UART line must not fail a run.

Run: python tests/hil/test_parsing.py   (make test-hil-parsing)

Why this exists: the capture can mangle a line in more than one way, and both
were seen on the F103 parasite bench. The tail of the low-power mode line was
lost and the next line followed it; in another capture, noise on the wire
replaced "nabled - WFE " with eight bytes of rubbish. The checks compared
lines for equality, so either one failed the whole run. Worse, a mangled
*measurement* line was reported as a sensor error, because "not a
temperature" was read as "an error" - a line the capture mangled was counted
as a device that failed.

The cases below replay the artefacts that were actually captured
(bench_logs/hil/lowpower_*.log), not synthetic ones.
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

# The other captured artefact: noise on the wire replaced the middle of the
# mode line - "nabled - WFE " - with rubbish. The marker at the start of the
# line survived, which is what the check is about.
CORRUPTED_MODE_LINE = (
    "OW_LOW_POWER e\x05j\x02�\x19\x15�sleep on stages > 1ms"
)

# What the firmware prints when the low-power path is off.
DISABLED_MODE_LINE = "OW_LOW_POWER disabled - busy-poll only"

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


def test_line_has():
    print("line_has")
    check(
        "a corrupted mode line still carries its marker",
        common.line_has(CORRUPTED_MODE_LINE, lowpower.MODE),
    )
    check(
        "a torn mode line still carries its marker",
        common.line_has(TORN_MODE_LINE, lowpower.MODE),
    )
    check(
        "an intact line carries its marker",
        common.line_has(lowpower.MODE, lowpower.MODE),
    )
    check(
        "a corrupted mode line is not read as the disabled variant",
        not common.line_has(CORRUPTED_MODE_LINE, lowpower.MODE_DISABLED),
    )
    check(
        "the disabled variant is recognised",
        common.line_has(DISABLED_MODE_LINE, lowpower.MODE_DISABLED),
    )
    check(
        "the disabled variant carries the common marker too",
        common.line_has(DISABLED_MODE_LINE, lowpower.MODE),
    )
    check(
        "a line that lost its marker does not match",
        not common.line_has("sleep on stages > 1ms", lowpower.MODE),
    )
    check(
        "an unrelated line does not match",
        not common.line_has("Found 7 device(s). Measuring each in turn.",
                            lowpower.MODE),
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


def _capture(mode_line):
    return (
        "DS18B20 7_low_power (low power) starting...\r\n"
        "Searching 1-Wire bus...\r\n" + mode_line + "\r\n"
        "28 20 78 92 07 00 00 67: 26.2 C\r\n"
    ).encode("ascii", "replace")


def test_lowpower_check():
    print("lowpower check, end to end")
    for name, mode_line in (
        ("a corrupted mode line", CORRUPTED_MODE_LINE),
        ("a torn mode line", TORN_MODE_LINE),
        ("an intact mode line", lowpower.MODE + " - WFE sleep on stages > 1ms"),
    ):
        ctx = _Ctx(b"")
        ok, detail = lowpower.run(_StubSerial(_capture(mode_line)), ctx)
        check("the check passes with %s (%s)" % (name, detail), ok)

    # A firmware built without the low-power define must still be rejected:
    # the marker is shared by both variants, so the wording is what tells
    # them apart.
    ctx = _Ctx(b"")
    ok, detail = lowpower.run(_StubSerial(_capture(DISABLED_MODE_LINE)), ctx)
    check("the check rejects the disabled variant (%s)" % detail, not ok)


def test_log_identity():
    print("the run's identity header")
    check("the version comes from inc/ds18b20.h",
          common.library_version() == "2.4.0")
    check("the part comes from the Makefile, not a second table",
          common.chip_for("STM32F103xB") == "f103xb")
    check("the clock comes from chips/<part>.mk",
          common.sysclk_for("f103xb") == 72)

    header = common.log_header("ds2431", "8_ds2431", mcu="STM32F103xB")
    fields = dict(
        word.split("=", 1)
        for line in header
        if line.startswith("# hil: ")
        for word in line[6:].split()
    )
    check("the bench and the clock are named",
          fields.get("mcu") == "STM32F103xB"
          and fields.get("sysclk") == "72MHz")
    check("the tree identifies itself",
          fields.get("fw") == "2.4.0"
          and fields.get("commit") not in (None, "", "?"))
    check("an image hash is only written when there is an image",
          "sha256" not in fields)
    check("a run with no build flags says so",
          fields.get("ext") == "none")
    check("the UART buffer the harness asked for is named",
          fields.get("uart_tx") ==
          (str(common.UART_TX_SIZE) if common.UART_TX_SIZE else "shipped"))

    saved_mcu, saved_ext = common.MCU, common.BUILD_EXT
    saved_env = os.environ.pop("HIL_EXT", None)
    try:
        common.select_bench("STM32F103xB")
        bench = common.log_header("low", "7_low_power", mcu="STM32F103xB")
    finally:
        common.MCU, common.BUILD_EXT = saved_mcu, saved_ext
        if saved_env is not None:
            os.environ["HIL_EXT"] = saved_env
    bench_fields = dict(
        word.split("=", 1)
        for line in bench
        if line.startswith("# hil: ")
        for word in line[6:].split()
    )
    check("the bench's pull-up and the example's define are both named",
          bench_fields.get("ext")
          == "-DOW_PARASITE_POWER=1,-DOW_LOW_POWER=1")


def main():
    test_line_has()
    test_parse_measurement()
    test_census()
    test_log_identity()
    test_lowpower_check()
    if failures:
        print("test_parsing: FAILED (%d)" % len(failures))
        return 1
    print("test_parsing: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
