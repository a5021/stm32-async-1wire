"""DS2431 presence check: examples/8_ds2431.

The example drives the full EEPROM cycle on the first DS2431 (family 0x2D)
the search finds: Read Memory baseline, Write Scratchpad, Read Scratchpad,
Copy Scratchpad (t_PROG hold-off) and Read Memory again. The check requires
the cycle to complete without an abort, and requires the read-back after the
copy to equal the pattern the firmware wrote, which pins that the strong
pull-up held through t_PROG on a parasite-powered bench and that the EEPROM
really took the data.

Known limitation: the firmware's write pattern is fixed, so a broken write
path only fails the check while the EEPROM does not already hold that
pattern - after one successful pass a later lost write re-reads the stored
pattern and would slip through. Presence itself (ROM family + completion) is
unaffected.

DS18B20 devices on the same bus do not interfere: the example's search
filters on the DS2431 family code, so sensors never appear in its output and
are never touched.
"""

import re

import common

BANNER = "DS2431 8_ds2431 starting..."

SELECTED_RE = re.compile(r"^Selected ROM: ((?:[0-9A-F]{2} ){7}[0-9A-F]{2})$")
EEPROM_RE = re.compile(r"^EEPROM: ((?:[0-9A-F]{2} ){7}[0-9A-F]{2})$")
MISSING = "No DS2431 found."
ABORT = "no device present; operation aborted"
DONE = "Demo done."

FAMILY_CODE = 0x2D  # DS2431_FAMILY_CODE: the ROM byte the search filters on
# The firmware's own write pattern (examples/8_ds2431/main.c write_data); a
# copy that never reached the EEPROM would read back whatever was there, not
# this.
PATTERN = [0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04]


def run(uart, ctx):
    reader = common.LineReader(uart, initial=ctx.seed)
    reads = []
    selected = None
    found = 0
    missing = False
    abort = False
    saw_done = False
    for t, line in reader.iter_lines(ctx.window, sink=ctx.lines):
        m = common.FOUND_RE.match(line)
        if m:
            found = int(m.group(1))
            continue
        m = SELECTED_RE.match(line)
        if m:
            selected = [int(x, 16) for x in m.group(1).split()]
            continue
        m = EEPROM_RE.match(line)
        if m:
            reads.append([int(x, 16) for x in m.group(1).split()])
            continue
        if line == DONE:
            saw_done = True
            # The example stops streaming once it is done, so nothing more
            # arrives by design; keep the log tidy and the loop short.
            break
        if line == MISSING:
            missing = True
        elif line == ABORT:
            abort = True

    if missing:
        return False, "no DS2431 on the bus"
    if abort:
        return False, "operation aborted (device stopped answering)"
    if selected is None:
        return False, "no Selected ROM line"
    if selected[0] != FAMILY_CODE:
        return False, "selected ROM family 0x%02X is not DS2431" % selected[0]
    if not reads:
        return False, "no EEPROM memory line"
    if reads[-1] != PATTERN:
        return False, "read-back does not match the written pattern: got %s" % (
            " ".join("%02X" % b for b in reads[-1]),
        )
    return True, "%d DS2431 device(s), %d EEPROM read(s), read-back matches" % (
        found,
        len(reads),
    )