"""DS2431 check: examples/8_ds2431.

The example runs the full EEPROM cycle on every DS2431 (family 0x2D) the
family-filtered search finds: Read Memory baseline, Write Scratchpad, Read
Scratchpad, Copy Scratchpad (t_PROG hold-off) and Read Memory again. The check
walks the output per device - each `Selected ROM:` opens a device, the EEPROM
lines that follow belong to it - and requires every device to be served:

  * the number of Selected ROMs equals the firmware's `Found N device(s)`
    census (so a regression that cycles only the first device is caught);
  * all Selected ROMs are distinct (a silently rejected ds2431_select would
    otherwise re-cycle the first device under the same ROM);
  * the family byte is 0x2D;
  * no abort, and the read-back after each copy equals the pattern the
    firmware wrote, which pins that the strong pull-up held through t_PROG on a
    parasite-powered bench and that the EEPROM really took the data.

Known limitation: the firmware's write pattern is fixed, so a broken write
path only fails the check while the EEPROM does not already hold that pattern -
after one successful pass a later lost write re-reads the stored pattern and
would slip through. Presence itself (ROM family + completion) is unaffected.

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
    devices = []
    found = 0
    missing = False
    abort = False
    for t, line in reader.iter_lines(ctx.window, sink=ctx.lines):
        m = common.FOUND_RE.match(line)
        if m:
            found = int(m.group(1))
            continue
        m = SELECTED_RE.match(line)
        if m:
            devices.append(
                {
                    "rom": [int(x, 16) for x in m.group(1).split()],
                    "reads": [],
                }
            )
            continue
        m = EEPROM_RE.match(line)
        if m:
            if devices:
                devices[-1]["reads"].append(
                    [int(x, 16) for x in m.group(1).split()]
                )
            continue
        if line == DONE:
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
        return False, "operation aborted (a device stopped answering)"
    if not devices:
        return False, "no Selected ROM line"
    if found and len(devices) != found:
        return False, "cycled %d device(s), the bus found %d" % (
            len(devices),
            found,
        )
    roms = [tuple(d["rom"]) for d in devices]
    if len(set(roms)) != len(roms):
        return False, "a Selected ROM repeated (ds2431_select likely rejected)"
    for idx, dev in enumerate(devices):
        if dev["rom"][0] != FAMILY_CODE:
            return False, "device %d family 0x%02X is not DS2431" % (
                idx + 1,
                dev["rom"][0],
            )
        if not dev["reads"]:
            return False, "device %d produced no EEPROM read" % (idx + 1)
        if dev["reads"][-1] != PATTERN:
            return False, "device %d read-back does not match the written pattern: got %s" % (
                idx + 1,
                " ".join("%02X" % b for b in dev["reads"][-1]),
            )
    return True, "%d DS2431 device(s), %d EEPROM read(s), read-back matches" % (
        len(devices),
        sum(len(d["reads"]) for d in devices),
    )