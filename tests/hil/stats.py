"""Statistics check: examples/6_statistics.

The example prints one non-blocking dump per STATS_DUMP_SWEEPS sweeps. A dump
has four parts, and the check is built on the relationships *between* them
rather than on any value copied from a bench log:

  `--- stats [N c] ---`          cycle count N (one cycle == one measurement)
  `ROM:min-max n<count> e<err>`  one line per sensor
  `h:B=<count> ...`              pulse-width histogram (non-empty buckets only)
  `t=Nc Ee`                      the same cycle count and the total error count

On a healthy bus every measurement runs `ow_stats_capture_pulse(.., 72, rom)`
once (DS18B20_SCRATCHPAD_BITS pulse slots, attributed to one sensor) and
`ow_stats_tick()` once, which pins four invariants:

    sum(sensor counts) == N          every cycle is attributed to one sensor
    sum(sensor errors) == E          per-sensor errors are the global total
    header N           == totals N   the two prints agree
    histogram sum      == N * 72     every cycle contributes 72 pulse slots

None of these hold if the dump is truncated, a counter is miscomputed or an
error slipped through, so they catch real regressions while still following the
bus actually attached (via the firmware's own `Found N device(s)` line).
Temperatures printed between dumps are checked against the DS18B20 brownout
(127.9 C) and power-on-reset (85.0 C) sentinels.
"""

import re

import common

HEADER_RE = re.compile(r"^--- stats \[(\d+) c\] ---$")
SENSOR_RE = re.compile(
    r"^((?:[0-9A-F]{2} ){7}[0-9A-F]{2}):(\d+)-(\d+) n(\d+) e(\d+)$"
)
HIST_RE = re.compile(r"^h:(.*)$")
BUCKET_RE = re.compile(r"(\d+)=(\d+)")
TOTAL_RE = re.compile(r"^t=(\d+)c (\d+)e$")

# Firmware constants the dump exposes indirectly. Kept here, not as literals, so
# a deliberate change in the firmware points at one place to revisit.
HIST_BUCKETS = 16  # OW_STATS_HIST_BUCKETS: largest valid bucket index is < 16
MAX_SENSORS = 8  # OW_STATS_MAX_SENSORS: the dump cannot list more than eight
PULSES_PER_CYCLE = 72  # DS18B20_SCRATCHPAD_BITS captured per measurement


def run(uart, ctx):
    reader = common.LineReader(uart)
    dumps = []
    cur = None
    temps = []
    found = 0
    for t, line in reader.iter_lines(ctx.timeout, sink=ctx.lines):
        if cur is not None:
            m = SENSOR_RE.match(line)
            if m:
                cur["sensors"].append(
                    (
                        int(m.group(2)),
                        int(m.group(3)),
                        int(m.group(4)),
                        int(m.group(5)),
                    )
                )
                continue
            m = HIST_RE.match(line)
            if m:
                for idx, count in BUCKET_RE.findall(m.group(1)):
                    if not 0 <= int(idx) < HIST_BUCKETS:
                        cur["hist_bad"] = True
                    cur["hist_sum"] += int(count)
                continue
            m = TOTAL_RE.match(line)
            if m:
                cur["total"] = (int(m.group(1)), int(m.group(2)))
                dumps.append(cur)
                cur = None
            # Any other line inside a dump is a torn fragment; it matches
            # nothing, keeping the dump open until its totals line closes it.
            continue
        m = common.FOUND_RE.match(line)
        if m:
            found = int(m.group(1))
            continue
        m = HEADER_RE.match(line)
        if m:
            cur = {
                "n": int(m.group(1)),
                "sensors": [],
                "hist_sum": 0,
                "hist_bad": False,
                "total": None,
            }
            continue
        m = common.parse_measurement(line)
        if m is not None and m[1] is not None:
            temps.append(m[1])

    if not dumps:
        return False, "no stats dump seen"
    for dump in dumps:
        error = _check_dump(dump, found, ctx.expected_devices)
        if error:
            return False, error
    if not temps:
        return False, "no temperature readings"
    bad = [x for x in temps if not (10.0 <= x <= 40.0)]
    if bad:
        return False, "temperature(s) out of range: %s" % bad[:3]
    dump = dumps[0]
    return True, "%d dump(s), %d sensor(s), %d cycle(s), %d pulse(s), 0 error(s)" % (
        len(dumps),
        len(dump["sensors"]),
        dump["n"],
        dump["hist_sum"],
    )


def _check_dump(dump, found, expected):
    n = dump["n"]
    if n <= 0:
        return "dump header has a non-positive cycle count"
    if dump["total"] is None:
        return "dump has no totals line"
    total_cycles, total_errors = dump["total"]
    if total_cycles != n:
        return "totals cycles %d disagree with header %d" % (total_cycles, n)

    sensors = dump["sensors"]
    if not sensors:
        return "dump lists no sensors"
    if found:
        want = min(found, MAX_SENSORS)
        if len(sensors) != want:
            return "dump lists %d sensor(s), bus found %d" % (len(sensors), found)
    elif expected and len(sensors) < expected:
        return "dump lists %d sensor(s) < expected %d" % (len(sensors), expected)
    if n % len(sensors) != 0:
        return "cycles %d are not a whole number of sweeps over %d sensor(s)" % (
            n,
            len(sensors),
        )

    counts = 0
    errors = 0
    for min_pulse, max_pulse, count, err in sensors:
        if err:
            return "sensor error counter non-zero (e%d)" % err
        if count < 1:
            return "sensor reports n%d captures" % count
        if min_pulse < 1 or min_pulse > max_pulse:
            return "sensor pulse range %d-%d is implausible" % (min_pulse, max_pulse)
        counts += count
        errors += err
    if counts != n:
        return "sensor captures sum to %d, header says %d cycles" % (counts, n)
    if errors != total_errors:
        return "per-sensor errors sum to %d, totals say %d" % (errors, total_errors)

    if dump["hist_bad"]:
        return "histogram has an out-of-range bucket index"
    if dump["hist_sum"] <= 0:
        return "histogram summed to zero"
    if dump["hist_sum"] != n * PULSES_PER_CYCLE:
        return "histogram sum %d != %d cycles x %d pulses" % (
            dump["hist_sum"],
            n,
            PULSES_PER_CYCLE,
        )
    return None
