"""Probe for an attached ST-Link and STM32H503 target.

Standalone entry point for `make test-hil`: prints what it found and exits 0
when the probe or target is absent (the caller skips) or present, 1 when a
probe is attached but the target did not answer.
"""

import sys

import common


def main():
    status, detail = common.probe()
    if status == "ok":
        for line in detail.splitlines():
            if line.startswith(("ST-LINK SN", "Device name", "Device ID", "Voltage")):
                print(line.strip())
        print("probe: ok")
        return 0
    if status == "absent":
        print("probe: absent - %s" % detail)
        return 0
    print("probe: error - %s" % detail)
    return 1


if __name__ == "__main__":
    sys.exit(main())
