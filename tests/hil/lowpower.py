"""Low-power check: examples/7_low_power.

Requires the boot banner and the WFE mode line, then at least one temperature
reading and no error line. The device count is deliberately not asserted: the
WFE path can tear a ROM line (UART ring overrun), and this check is about the
device staying alive and converting, not about an exact census.
"""

import common

BANNER = "DS18B20 7_low_power (low power) starting..."
MODE = "OW_LOW_POWER enabled - WFE sleep on stages > 1ms"


def run(uart, ctx):
    reader = common.LineReader(uart, initial=ctx.seed)
    saw_banner = False
    saw_mode = False
    temps = []
    errors = []
    for t, line in reader.iter_lines(ctx.window, sink=ctx.lines):
        if common.line_matches(line, BANNER):
            saw_banner = True
        elif common.line_matches(line, MODE):
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
