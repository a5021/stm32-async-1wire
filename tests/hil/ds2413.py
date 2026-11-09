"""DS2413 check: examples/9_multi on the F446 bench, read over SWD.

The F446 console is not always wired to the host COM port, so this check does
not use the generic UART flow. It flashes `9_multi`, waits for the search to
finish, then samples the example's `ds2413_blinks`, `ds2413_last_state` and
`ds2413_errors` globals through the ST-Link (HOTPLUG, no reset) and requires:

  * at least four completed blink cycles;
  * the reported state alternates between PIOA (0x01) and PIOB (0x02);
  * no failed write/read (`ds2413_errors == 0`).

The example blinks only while it waits between measurement cycles, so the
sampling window covers two 2 s cycles.
"""

import os
import re
import subprocess
import sys
import time

import common

BANNER = "9_multi"
SYMBOLS = ("ds2413_tel", "ds2413_errors")


def _nm(elf, name):
    proc = subprocess.run(["arm-none-eabi-nm", elf], capture_output=True,
                          text=True, errors="replace")
    if proc.returncode != 0:
        return None
    m = re.search(r"^([0-9a-fA-F]+)\s+[bBdD]\s+%s$" % re.escape(name),
                  proc.stdout, re.M)
    return int(m.group(1), 16) if m else None


def _read(prog, addr, size):
    proc = subprocess.run(
        [prog, "-c", "port=SWD", "mode=HOTPLUG",
         "-r%d" % size, "0x%08x" % addr, "1"],
        capture_output=True, text=True, errors="replace")
    m = re.search(r"^0x[0-9a-fA-F]+ : ([0-9a-fA-F ]+)$", proc.stdout, re.M)
    if not m:
        return None
    return int(m.group(1).split()[0], 16)


def main(argv):
    status, detail = common.probe()
    if status == "absent":
        print("SKIP: %s" % detail)
        return 0
    if status == "error":
        print("FAIL: %s" % detail)
        return 1

    requested = os.environ.get("HIL_MCU")
    detected = common.bench_from_probe(detail)
    if requested and detected != requested:
        print("FAIL: %s is attached but HIL_MCU=%s was requested"
              % (detected or "an unrecognised target", requested))
        return 1
    if detected is None:
        print("FAIL: cannot identify the attached target")
        return 1
    common.select_bench(detected)
    print("bench: %s" % detected)

    try:
        hexpath = common.build_hex("9_multi")
    except RuntimeError as exc:
        print("FAIL: %s" % exc)
        return 1
    prog = common.find_programmer()
    if not prog:
        print("FAIL: STM32CubeProgrammer CLI not found")
        return 1

    elf = os.path.join(common.BUILD_DIR, "ds18b20_9_multi.elf")
    addrs = {name: _nm(elf, name) for name in SYMBOLS}
    if any(v is None for v in addrs.values()):
        print("FAIL: telemetry symbols missing in %s" % elf)
        return 1

    common.flash(hexpath)
    time.sleep(3.0)  # search + boot, then the first blink window

    prev = None
    states = []
    cmds = []
    errors = None
    deadline = time.time() + 8.0
    while time.time() < deadline:
        tel = _read(prog, addrs["ds2413_tel"], 32)
        errors = _read(prog, addrs["ds2413_errors"], 32)
        if tel is None or errors is None:
            print("FAIL: SWD read failed")
            return 1
        seq = (tel >> 16) & 0xFFFF
        cmd = (tel >> 8) & 0xFF
        state = tel & 0xFF
        if prev is None or seq != prev:
            states.append(state)
            cmds.append(cmd)
            prev = seq
        if errors != 0:
            print("FAIL: %d DS2413 operation(s) failed" % errors)
            return 1
        time.sleep(0.1)

    if errors != 0:
        print("FAIL: %d DS2413 operation(s) failed" % errors)
        return 1
    if len(states) < 4:
        print("FAIL: only %d blink(s) observed" % len(states))
        return 1
    if any(c not in (1, 2) for c in cmds):
        print("FAIL: unexpected commanded state(s): %s" % cmds)
        return 1
    if any(a == b for a, b in zip(cmds, cmds[1:])):
        print("FAIL: PIOA/PIOB did not alternate: %s" % cmds)
        return 1
    for cmd, state in zip(cmds, states):
        if state != cmd:
            print("FAIL: commanded %s but read back %s (%s)"
                  % (cmds, states, state))
            return 1
    print("PASS: %d blink(s), commands %s, reads %s, no errors"
          % (len(cmds), "".join(str(c) for c in cmds),
             "".join(str(s) for s in states)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
