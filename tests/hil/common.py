"""Shared helpers for the hardware-in-the-loop harness in tests/hil/.

Builds an example for STM32H503xx, flashes it with the STM32CubeProgrammer CLI
and reads the example's UART output. Nothing here is wired into CI:
`make test-hil` is the only entry point, and it skips when no ST-Link or target
is attached.
"""

import glob
import os
import re
import shutil
import subprocess
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir, os.pardir))
BUILD_DIR = os.path.join(REPO, "build")
LOG_DIR = os.path.join(REPO, "bench_logs", "hil")

MCU = "STM32H503xx"
FLASH_ADDR = "0x08000000"

# The H503CB bench is parasite-powered, so the firmware must engage the strong
# pull-up during conversion or the sensors brown out (127.9 C) and report their
# power-on-reset value (85.0 C). Override with HIL_EXT to build a different
# variant (HIL_EXT= for an externally powered bus).
BUILD_EXT = os.environ.get("HIL_EXT", "-DOW_PARASITE_POWER=1")

# Examples that need an extra define to exercise the path the check is about.
# 7_low_power only prints its WFE mode line (and only sleeps) when
# OW_PORT_LOW_POWER is set, which is what the low-power check asserts.
APP_DEFINES = {
    "7_low_power": "-DOW_PORT_LOW_POWER=1",
}


def build_ext(app):
    extra = APP_DEFINES.get(app, "")
    if BUILD_EXT and extra:
        return (BUILD_EXT + " " + extra).strip()
    return BUILD_EXT or extra

# The search prints every ROM in one burst and the examples' default TX ring
# (256 B) drops the newest bytes when it overflows, so a 7-device bus can lose a
# ROM line before the measurements even start. The harness builds with a larger
# ring so it exercises the bus logic rather than the example's buffer size.
# Override with HIL_UART_TX_SIZE=0 to keep the shipped size.
UART_TX_SIZE = int(os.environ.get("HIL_UART_TX_SIZE", "1024"))

PROGRAMMER_CANDIDATES = (
    os.environ.get("STM32_PROG"),
    r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe",
    "STM32_Programmer_CLI",
)

MEAS_RE = re.compile(r"^((?:[0-9A-F]{2} ){7}[0-9A-F]{2}):\s*(.+)$")
TEMP_RE = re.compile(r"^(-?\d+\.\d)\s+C$")
SEPARATOR_RE = re.compile(r"^-{5,}$")
FOUND_RE = re.compile(r"^Found (\d+) device")


class Ctx:
    """Everything a check needs: identity, expectations and the captured log."""

    def __init__(self, script, app, expected_devices, port, baud, timeout):
        self.script = script
        self.app = app
        self.expected_devices = expected_devices
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.lines = []


def default_port():
    for key in ("HIL_PORT", "UART_PORT"):
        if os.environ.get(key):
            return os.environ[key]
    return "COM5" if os.name == "nt" else "/dev/ttyUSB0"


def open_uart(port=None, baud=None, timeout=0.2):
    import serial

    port = port or default_port()
    baud = int(baud or os.environ.get("HIL_BAUD", "115200"))
    return serial.Serial(port, baud, timeout=timeout)


def parse_temp(value):
    m = TEMP_RE.match(value.strip())
    return float(m.group(1)) if m else None


def parse_measurement(line):
    """Return (rom, temp, error) for a `ROM: value` line, else None.

    Exactly one of temp/error is non-None. The error wording differs per
    example (`no sensor detected.` / `CRC check failed.` / `generic failure.`
    in 2/4/7, the short `no sensor` / `CRC fail` / `error` in 6), so the value
    is compared against the temperature shape rather than against a token list.
    """
    m = MEAS_RE.match(line)
    if not m:
        return None
    rom, value = m.group(1), m.group(2)
    temp = parse_temp(value)
    return rom, temp, (None if temp is not None else value)


class LineReader:
    """Reads UART lines, tolerating CRLF, partial writes and overrun-split lines.

    The examples drive UART through a small non-blocking ring buffer drained one
    byte per main-loop poll, so a burst can tear a line in two - seen on real
    H503 hardware as `  ROM: 28 F1 39` followed by ` AD 0B 00 00 D9`. Lines are
    therefore matched independently; a torn fragment simply matches nothing.
    """

    def __init__(self, ser, flush_timeout=1.0):
        self.ser = ser
        self.buf = b""
        self.flush_timeout = flush_timeout
        self.last_data = time.monotonic()

    def iter_lines(self, duration, sink=None):
        end = time.monotonic() + duration
        while True:
            remaining = end - time.monotonic()
            if remaining <= 0:
                break
            self.ser.timeout = min(0.2, remaining)
            chunk = self.ser.read(4096)
            now = time.monotonic()
            if chunk:
                self.buf += chunk
                self.last_data = now
            while b"\r\n" in self.buf:
                raw, self.buf = self.buf.split(b"\r\n", 1)
                line = raw.decode("ascii", "replace").strip()
                if sink is not None:
                    sink.append((now, line))
                yield now, line
            if self.buf and (now - self.last_data) >= self.flush_timeout:
                raw, self.buf = self.buf, b""
                self.last_data = now
                line = raw.decode("ascii", "replace").strip()
                if sink is not None:
                    sink.append((now, line))
                yield now, line


def find_programmer():
    for cand in PROGRAMMER_CANDIDATES:
        if not cand:
            continue
        if os.path.isfile(cand):
            return cand
        found = shutil.which(cand)
        if found:
            return found
    return None


def run_make(app):
    make = shutil.which("make")
    if not make:
        raise RuntimeError("make not found on PATH")
    cmd = [make, "APP=%s" % app, "OW_MCU=%s" % MCU, "EXT=%s" % build_ext(app)]
    if UART_TX_SIZE:
        cmd.append("UART_TX_SIZE_%s=%d" % (app, UART_TX_SIZE))
    proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            "make failed (%d):\n%s\n%s" % (proc.returncode, proc.stdout, proc.stderr)
        )


def build_hex(app):
    # The object stamp encodes EXT and OPT but not DEF, and UART_TX_BUF_SIZE
    # arrives through DEF, so a stale object could be reused after the harness's
    # buffer size changes. Drop this app's objects to be certain the build matches
    # the configuration the harness asked for.
    for path in glob.glob(os.path.join(BUILD_DIR, "%s_*.o" % app)):
        os.remove(path)
    run_make(app)
    hexpath = os.path.join(BUILD_DIR, "ds18b20_%s.hex" % app)
    if not os.path.isfile(hexpath):
        raise RuntimeError("build did not produce %s" % hexpath)
    return hexpath


def flash(hexpath):
    prog = find_programmer()
    if not prog:
        raise RuntimeError("STM32CubeProgrammer CLI not found")
    cmd = [prog, "-c", "port=SWD", "-w", hexpath, FLASH_ADDR, "-v", "-rst", "-run"]
    proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            "flash failed (%d):\n%s\n%s" % (proc.returncode, proc.stdout, proc.stderr)
        )
    return proc.stdout


def probe():
    """Return (status, detail) with status in {'ok', 'absent', 'error'}.

    'absent' means no probe or no target - the caller skips. 'error' means a
    probe is attached but the target did not answer, which is a real failure.
    """
    prog = find_programmer()
    if not prog:
        return "absent", "STM32CubeProgrammer CLI not found"
    listed = subprocess.run(
        [prog, "-l", "st-link-only"], cwd=REPO, capture_output=True, text=True
    )
    if "ST-LINK SN" not in listed.stdout:
        return "absent", "no ST-Link probe attached"
    conn = subprocess.run(
        [prog, "-c", "port=SWD"], cwd=REPO, capture_output=True, text=True
    )
    if conn.returncode != 0 or "Device name" not in conn.stdout:
        return "error", "ST-Link attached but the target did not respond"
    return "ok", conn.stdout


def save_log(name, lines):
    os.makedirs(LOG_DIR, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    path = os.path.join(LOG_DIR, "%s_%s.log" % (name, stamp))
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# %s\n" % name)
        for t, line in lines:
            fh.write("[%7.2f] %s\n" % (t, line))
    return path
