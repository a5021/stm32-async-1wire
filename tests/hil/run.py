"""HIL orchestrator: `python tests/hil/run.py <script>`.

Maps a script name to an example, builds it for the H503, flashes it with the
STM32CubeProgrammer CLI, reads the UART and saves the capture under
bench_logs/hil/. Skips (exit 0) when no ST-Link or target is attached.

Scripts: smoke (2_device_search), scan (4_scan_mode), stats (6_statistics),
lowpower (7_low_power).
"""

import os
import sys
import time

import common
import lowpower
import scan
import smoke
import stats

SCRIPTS = {
    "smoke": ("2_device_search", smoke.run),
    "scan": ("4_scan_mode", scan.run),
    "stats": ("6_statistics", stats.run),
    "lowpower": ("7_low_power", lowpower.run),
}

TIMEOUTS = {"smoke": 60.0, "scan": 15.0, "stats": 75.0, "lowpower": 20.0}


def main(argv):
    script = argv[1] if len(argv) > 1 else os.environ.get("HIL_SCRIPT", "smoke")
    if script not in SCRIPTS:
        sys.stderr.write("usage: run.py {%s}\n" % "|".join(SCRIPTS))
        return 2
    app, check = SCRIPTS[script]

    expected = int(os.environ.get("EXPECTED_DEVICES") or 0)
    port = common.default_port()
    baud = int(os.environ.get("HIL_BAUD", "115200"))
    timeout = float(os.environ.get("HIL_TIMEOUT", TIMEOUTS[script]))

    status, detail = common.probe()
    if status == "absent":
        print("SKIP: %s" % detail)
        return 0
    if status == "error":
        print("FAIL: %s" % detail)
        return 1

    try:
        hexpath = common.build_hex(app)
    except RuntimeError as exc:
        print("FAIL: %s" % exc)
        return 1
    print("built %s" % os.path.relpath(hexpath, common.REPO))

    try:
        uart = common.open_uart(port, baud)
    except Exception as exc:
        print("FAIL: cannot open %s: %s" % (port, exc))
        return 1

    ctx = common.Ctx(script, app, expected, port, baud, timeout)
    try:
        uart.reset_input_buffer()
        common.flash(hexpath)
        # The previous firmware keeps transmitting while the new image is being
        # flashed, so its tail would otherwise be captured as this run's output.
        # The banner follows the reset by ~200 ms; clear the RX buffer just
        # before it arrives.
        time.sleep(0.15)
        uart.reset_input_buffer()
        ok, message = check(uart, ctx)
    except Exception as exc:
        ok, message = False, str(exc)
    finally:
        uart.close()

    log_path = common.save_log(script, ctx.lines)
    print("log: %s" % os.path.relpath(log_path, common.REPO))
    print(("PASS: " if ok else "FAIL: ") + message)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
