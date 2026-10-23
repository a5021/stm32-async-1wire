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
  * no abort, and each device has a baseline read and a read-back;
  * the read-back equals the baseline with every byte incremented, which is
    the value the example says it writes, and the application's own
    `read-back matches the write` line agrees.

The example writes the row's own next value rather than a fixed pattern, so a
copy that never reached the EEPROM reads back the baseline and fails the
increment - after one successful pass a fixed pattern would have read back
what was already stored. The check still shares the firmware's arithmetic
with the example: it proves that the device holds the value the firmware meant
to write, not that the wire carried the command. `la_decode.py --log` reads
the same run from the wire and proves the copy independently.

DS18B20 devices on the same bus do not interfere: the example's search
filters on the DS2431 family code, so sensors never appear in its output and
are never touched.
"""

import re

import common

BANNER = "8_ds2431"

SELECTED_RE = re.compile(r"^Selected ROM: ((?:[0-9A-F]{2} ){7}[0-9A-F]{2})$")
EEPROM_RE = re.compile(r"^EEPROM: ((?:[0-9A-F]{2} ){7}[0-9A-F]{2})$")
MISSING = "No DS2431 found."
ABORT = "no device present; operation aborted"
DONE = "Demo done."
READ_BACK_OK = "read-back matches the write"
READ_BACK_BAD = "read-back DIFFERS from the write"

FAMILY_CODE = 0x2D  # DS2431_FAMILY_CODE: the ROM byte the search filters on


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
                    "claim": None,
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
        if common.line_has(line, READ_BACK_OK):
            if devices:
                devices[-1]["claim"] = True
            continue
        if common.line_has(line, READ_BACK_BAD):
            if devices:
                devices[-1]["claim"] = False
            continue
        if common.line_has(line, DONE):
            # The example stops streaming once it is done, so nothing more
            # arrives by design; keep the log tidy and the loop short.
            break
        if common.line_has(line, MISSING):
            missing = True
        elif common.line_has(line, ABORT):
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
        if len(dev["reads"]) < 2:
            return False, (
                "device %d produced %d EEPROM read(s), "
                "expected baseline and read-back" % (idx + 1, len(dev["reads"]))
            )
        baseline = dev["reads"][0]
        expected = [(b + 1) & 0xFF for b in baseline]
        if dev["reads"][-1] != expected:
            return False, "device %d read-back %s is not baseline+1 %s" % (
                idx + 1,
                " ".join("%02X" % b for b in dev["reads"][-1]),
                " ".join("%02X" % b for b in expected),
            )
        if dev["claim"] is False:
            return False, "device %d says its read-back DIFFERS from the write" % (
                idx + 1,
            )
        if dev["claim"] is None:
            return False, "device %d printed no read-back verdict" % (idx + 1)
    return True, "%d DS2431 device(s), %d EEPROM read(s), read-back is baseline+1" % (
        len(devices),
        sum(len(d["reads"]) for d in devices),
    )
