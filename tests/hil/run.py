"""HIL orchestrator: `python tests/hil/run.py <script>`.

Maps a script name to an example, identifies the attached bench (the F103
reference board or the H503 the harness was written on), builds the example
for it, flashes it with the STM32CubeProgrammer CLI, reads the UART and saves
the capture under bench_logs/hil/. Skips (exit 0) when no ST-Link or target is
attached.

Scripts: smoke (2_device_search), scan (4_scan_mode), stats (6_statistics),
lowpower (7_low_power), ds2431 (8_ds2431).
"""

import os
import sys

import common
import ds2431
import lowpower
import scan
import smoke
import stats

SCRIPTS = {
    "smoke": ("2_device_search", smoke.run, smoke.BANNER),
    "scan": ("4_scan_mode", scan.run, scan.BANNER),
    "stats": ("6_statistics", stats.run, stats.BANNER),
    "lowpower": ("7_low_power", lowpower.run, lowpower.BANNER),
    "ds2431": ("8_ds2431", ds2431.run, ds2431.BANNER),
}

# Capture windows for a bus of unknown size: the first number covers the boot
# banner, the search and everything fixed; the second is added per sensor once
# the firmware's `Found N device(s)` census lands (common.Budget). Sized so the
# H503 bench's usual six sensors get roughly the windows the checks were tuned
# on, and the F103 reference bench's eight fit too.
WINDOWS = {
    "smoke": (20.0, 8.0),
    "scan": (5.0, 2.0),
    "stats": (15.0, 9.0),
    "lowpower": (15.0, 2.5),
    "ds2431": (15.0, 0.0),
}


def main(argv):
    script = argv[1] if len(argv) > 1 else os.environ.get("HIL_SCRIPT", "smoke")
    if script not in SCRIPTS:
        sys.stderr.write("usage: run.py {%s}\n" % "|".join(SCRIPTS))
        return 2
    app, check, banner = SCRIPTS[script]

    status, detail = common.probe()
    if status == "absent":
        print("SKIP: %s" % detail)
        return 0
    if status == "error":
        print("FAIL: %s" % detail)
        return 1

    requested = os.environ.get("HIL_MCU")
    if requested and requested not in common.BENCH_PROFILES:
        print(
            "FAIL: HIL_MCU=%s is not a bench profile; known: %s"
            % (requested, ", ".join(common.BENCH_PROFILES))
        )
        return 1
    detected = common.bench_from_probe(detail)
    if requested and detected != requested:
        print(
            "FAIL: %s is attached but HIL_MCU=%s was requested"
            % (detected or "an unrecognised target", requested)
        )
        return 1
    if detected is None:
        print(
            "FAIL: cannot identify the attached target; set HIL_MCU= (known: %s)"
            % ", ".join(common.BENCH_PROFILES)
        )
        return 1
    common.select_bench(detected)
    print("bench: %s" % detected)

    expected = int(os.environ.get("EXPECTED_DEVICES") or 0)
    baud = int(os.environ.get("HIL_BAUD", "115200"))
    fixed = os.environ.get("HIL_TIMEOUT")
    window = common.Budget(
        base=WINDOWS[script][0],
        per_device=WINDOWS[script][1],
        expected=expected,
        fixed=float(fixed) if fixed else None,
    )

    try:
        hexpath = common.build_hex(app)
    except RuntimeError as exc:
        print("FAIL: %s" % exc)
        return 1
    print("built %s" % os.path.relpath(hexpath, common.REPO))

    port = common.default_port()
    if port is None:
        port, tried = common.find_port(baud)
        if port is None:
            print(
                "FAIL: no COM port carries example output (tried: %s); set HIL_PORT="
                % (", ".join(tried) or "none")
            )
            return 1
        print("uart: %s (scanned)" % port)

    try:
        uart = common.open_uart(port, baud)
    except Exception as exc:
        print("FAIL: cannot open %s: %s" % (port, exc))
        return 1

    ctx = common.Ctx(script, app, expected, port, baud, window)
    try:
        uart.reset_input_buffer()
        common.flash(hexpath)
        # The previous firmware keeps transmitting while the new image is being
        # flashed, so its tail would otherwise be captured as this run's
        # output. Read until the new firmware's banner proves it has booted;
        # wait_for_banner() discards everything before the banner and hands
        # back the banner-plus-tail it already consumed, with which the
        # check's LineReader is seeded. On timeout, fall back to clearing the
        # RX buffer and letting the check decide whether what follows works.
        leftover = common.wait_for_banner(uart, banner)
        if leftover is None:
            print("warn: boot banner not seen within the wait; press on anyway")
            leftover = b""
        uart.reset_input_buffer()
        ctx.seed = leftover
        ok, message = check(uart, ctx)
    except Exception as exc:
        ok, message = False, str(exc)
    finally:
        uart.close()

    log_path = common.save_log(script, ctx.lines, ctx.app, hexpath)
    print("log: %s" % os.path.relpath(log_path, common.REPO))
    print(("PASS: " if ok else "FAIL: ") + message)
    if not ok and window.fixed is not None:
        # The window was pinned by hand, so a too-short pin is a plausible
        # cause of its own - name it before the bench gets blamed.
        print(
            "note: fixed HIL_TIMEOUT=%g window; the default scheme grows with "
            "the bus if left unset" % window.fixed
        )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
