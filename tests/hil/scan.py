"""Scan check: examples/4_scan_mode.

A round is the batch between dash separators. Requires at least one complete
round in which every device the search found appears with a plausible
temperature, and rejects the DS18B20 power-on-reset (85.0 C) and brownout
(127.9 C) sentinels.
"""

import common

BANNER = "4_scan_mode"


def run(uart, ctx):
    reader = common.LineReader(uart, initial=ctx.seed)
    rounds = []
    current = []
    errors = []
    search_count = 0
    for t, line in reader.iter_lines(ctx.window, sink=ctx.lines):
        m = common.FOUND_RE.match(line)
        if m:
            search_count = int(m.group(1))
            continue
        if common.SEPARATOR_RE.match(line):
            if current:
                rounds.append(current)
                current = []
            continue
        m = common.parse_measurement(line)
        if m is None:
            continue
        rom, temp, err = m
        if err is not None:
            errors.append(line)
        else:
            current.append((rom, temp))
    if current:
        rounds.append(current)

    if errors:
        return False, "error line(s): %s" % errors[0]
    if not rounds:
        return False, "no complete scan round"
    target = max(search_count, ctx.expected_devices)
    best = max(len(set(rom for rom, _ in rd)) for rd in rounds)
    if target and best < target:
        return False, "best round saw %d/%d devices" % (best, target)
    if best == 0:
        return False, "no devices measured"
    bad = [temp for rd in rounds for _, temp in rd if not (10.0 <= temp <= 40.0)]
    if bad:
        return False, "temperature(s) out of range: %s" % bad[:3]
    return True, "%d round(s), up to %d devices" % (len(rounds), best)
