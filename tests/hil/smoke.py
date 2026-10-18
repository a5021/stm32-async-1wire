"""Smoke check: examples/2_device_search.

Reads the round-robin measurement lines and requires every device the search
found to be measured with a plausible temperature and no error line. The
device count comes from the firmware's own `Found N device(s)` line, so the
check follows the bus actually attached; EXPECTED_DEVICES is an optional floor.
"""

import common

BANNER = "DS18B20 2_device_search starting..."


def run(uart, ctx):
    reader = common.LineReader(uart, initial=ctx.seed)
    roms = set()
    temps = []
    errors = []
    search_count = 0
    for t, line in reader.iter_lines(ctx.window, sink=ctx.lines):
        m = common.FOUND_RE.match(line)
        if m:
            search_count = int(m.group(1))
            continue
        m = common.parse_measurement(line)
        if m is None:
            continue
        rom, temp, err = m
        if err is not None:
            errors.append(line)
        else:
            roms.add(rom)
            temps.append(temp)

    if errors:
        return False, "error line(s): %s" % errors[0]
    target = max(search_count, ctx.expected_devices)
    if target == 0:
        return False, "no devices found"
    if len(roms) < target:
        return False, "measured %d/%d devices" % (len(roms), target)
    bad = [x for x in temps if not (10.0 <= x <= 40.0)]
    if bad:
        return False, "temperature(s) out of range: %s" % bad[:3]
    return True, "%d devices, %d readings, %.1f..%.1f C" % (
        len(roms),
        len(temps),
        min(temps),
        max(temps),
    )
