"""Low-power check: examples/7_low_power.

Requires the boot banner and the WFE mode line, then at least one temperature
reading and no error line. The device count is deliberately not asserted: the
WFE path can tear a ROM line (UART ring overrun), and this check is about the
device staying alive and converting, not about an exact census.

The lines are matched by marker, not by equality: the capture can lose a
line's tail or have a run of characters corrupted by noise on the wire, and
the marker is the part that says which line this is.
"""

import common

BANNER = "7_low_power"
# The firmware announces the low-power mode with one of two lines, depending
# on the build define: "OW_LOW_POWER enabled - WFE sleep on stages > 1ms" or
# "OW_LOW_POWER disabled - busy-poll only". The marker is the common part, and
# the enabled/disabled wording is checked separately - the line's middle can
# be mangled, and "OW_LOW_POWER enabled" did not survive one capture.
MODE = "OW_LOW_POWER"
MODE_DISABLED = "OW_LOW_POWER disabled"


def run(uart, ctx):
    reader = common.LineReader(uart, initial=ctx.seed)
    saw_banner = False
    saw_mode = False
    saw_disabled = False
    temps = []
    errors = []
    for t, line in reader.iter_lines(ctx.window, sink=ctx.lines):
        if common.line_has(line, BANNER):
            saw_banner = True
        if common.line_has(line, MODE_DISABLED):
            saw_disabled = True
        elif common.line_has(line, MODE):
            saw_mode = True
        m = common.parse_measurement(line)
        if m is None:
            continue
        rom, temp, err = m
        if err is not None:
            errors.append(line)
        else:
            temps.append(temp)

    if errors:
        return False, "error line(s): %s" % errors[0]
    if saw_disabled:
        return False, "firmware reports the low-power mode disabled"
    if not saw_banner:
        return False, "no boot banner"
    if not saw_mode:
        return False, "no low-power mode line"
    if not temps:
        return False, "no temperature read"
    return True, "%d reading(s), %.1f..%.1f C" % (
        len(temps),
        min(temps),
        max(temps),
    )
