"""Shared helpers for the hardware-in-the-loop harness in tests/hil/.

Identifies which bench is attached (the F103 reference board or the H503 WeAct
the harness was written on; see BENCH_PROFILES), builds the chosen example for
it, flashes it with the STM32CubeProgrammer CLI and reads the example's UART
output. Nothing here is wired into CI: `make test-hil` is the only entry
point, and it skips when no ST-Link or target is attached.
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

FLASH_ADDR = "0x08000000"

# The benches the harness knows, keyed by the OW_MCU device macro. STM32F103xB
# comes first: it is the project's default build part (the Makefile's OW_CHIP
# fallback and README "STM32F103xB is the default") and the reference bench in
# the supported-families table; the H503 WeAct board behind it is the
# parasite-powered bench this harness was written on. run.py reads the profile
# off the target the probe attached to; HIL_MCU pins one explicitly.
#
# Both benches run a parasite-powered bus, so both profiles build with the
# strong pull-up enabled: without it the sensors brown out mid-conversion
# (127.9 C) and report their power-on-reset value (85.0 C). HIL_EXT= (empty)
# opts out for an externally powered bus.
BENCH_PROFILES = {
    "STM32F103xB": {
        # CubeProgrammer prints a family-level name for this part ("STM32F10x
        # Medium-density devices" on Blue Pill-class boards), so the hints
        # carry both spellings; device_id is the DBGMCU fallback when the
        # name line is absent.
        "hints": ("STM32F103", "STM32F10x"),
        "device_id": "0x410",
        "ext": "-DOW_PARASITE_POWER=1",
    },
    "STM32H503xx": {
        "hints": ("STM32H503", "STM32H50x"),
        "device_id": "0x474",
        "ext": "-DOW_PARASITE_POWER=1",
    },
}

DEVICE_NAME_RE = re.compile(r"Device name\s+:\s*(.+)")
DEVICE_ID_RE = re.compile(r"Device ID\s+:\s*(0x[0-9A-Fa-f]+)")

# The bench the build targets; select_bench() sets both before any build.
MCU = None
BUILD_EXT = None

# Examples that need an extra define to exercise the path the check is about.
# 7_low_power only prints its WFE mode line (and only sleeps) when
# OW_LOW_POWER is set, which is what the low-power check asserts.
APP_DEFINES = {
    "7_low_power": "-DOW_LOW_POWER=1",
}


def build_ext(app):
    extra = APP_DEFINES.get(app, "")
    if BUILD_EXT and extra:
        return (BUILD_EXT + " " + extra).strip()
    return BUILD_EXT or extra


def select_bench(mcu):
    """Point the build at a bench profile: the OW_MCU macro and its EXT."""
    global MCU, BUILD_EXT
    MCU = mcu
    BUILD_EXT = (
        os.environ["HIL_EXT"] if "HIL_EXT" in os.environ else BENCH_PROFILES[mcu]["ext"]
    )


def bench_from_probe(stdout):
    """Map the CubeProgrammer connect output to a bench profile key, or None.

    CubeProgrammer prints both a device name and a DBGMCU device id; either
    spelling is accepted, because the name is family-level rather than exact
    on some exports.
    """
    m = DEVICE_NAME_RE.search(stdout)
    tag = m.group(1).strip() if m else ""
    ident = DEVICE_ID_RE.search(stdout)
    for mcu, profile in BENCH_PROFILES.items():
        if any(hint in tag for hint in profile["hints"]):
            return mcu
        if (
            ident is not None
            and profile["device_id"]
            and ident.group(1).lower() == profile["device_id"]
        ):
            return mcu
    return None

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
# The census line's count is what the checks need, and a torn line can cut it
# anywhere after the number - so the match stops at the digits.
FOUND_RE = re.compile(r"^Found (\d+)")

# The error values the examples write after the ROM prefix. The wording differs
# per example: "no sensor detected." / "CRC check failed." / "generic failure."
# in 2/4/7, the short "no sensor" / "CRC fail" / "error" in 6.
#
# This list is what separates a device error from a torn line. Anything else
# that is not a temperature was cut off by the capture and says nothing about
# the device - see parse_measurement().
ERROR_VALUES = (
    "no sensor detected.",
    "CRC check failed.",
    "generic failure.",
    "no sensor",
    "CRC fail",
    "error",
)

# Anything every example can print: the boot banner, the census line, a sweep
# separator, or a hex ROM/measurement/statistics line. This is what find_port()
# looks for among the COM ports when HIL_PORT was left unset.
FINGERPRINT_RE = re.compile(
    r"DS18B20|1-Wire|Found \d+ device|^-{3,}$|(?:[0-9A-F]{2} ){7}[0-9A-F]{2}"
)


class Budget:
    """The capture window for one run.

    A fixed window sized for the H503 bench's six sensors is tight for the
    eight-sensor F103 reference bench, so by default the window grows with the
    firmware's own `Found N device(s)` census: LineReader hands every line to
    note_line(), which widens the window to cover what the bus actually
    reports. An explicit HIL_TIMEOUT overrides the whole scheme with a fixed
    window, as before.
    """

    def __init__(self, base=0.0, per_device=0.0, expected=0, fixed=None):
        self.base = base
        self.per_device = per_device
        self.devices = max(expected, 0)
        self.fixed = fixed
        self.start = None

    def remaining(self):
        now = time.monotonic()
        if self.start is None:
            self.start = now
        spent = now - self.start
        if self.fixed is not None:
            return self.fixed - spent
        return self.base + self.per_device * self.devices - spent

    def note_line(self, line):
        if self.fixed is not None:
            return
        m = FOUND_RE.match(line)
        found = int(m.group(1)) if m else 0
        if found > self.devices:
            self.devices = found


class Ctx:
    """Everything a check needs: identity, expectations and the captured log."""

    def __init__(self, script, app, expected_devices, port, baud, window,
                 seed=b""):
        self.script = script
        self.app = app
        self.expected_devices = expected_devices
        self.port = port
        self.baud = baud
        self.window = window
        # Bytes read by wait_for_banner() up to and including the boot banner;
        # every check starts its LineReader from here so the new run's banner
        # and the lines that arrived with it are not lost.
        self.seed = seed
        self.lines = []


def default_port():
    """The port the caller pinned, or None when the caller wants a scan.

    A wrong pin is worse than a scan: with HIL_PORT set but silent, opening it
    must fail loudly instead of quietly binding to another bench's console.
    """
    for key in ("HIL_PORT", "UART_PORT"):
        if os.environ.get(key):
            return os.environ[key]
    return None


def open_uart(port=None, baud=None, timeout=0.2):
    import serial

    port = port or default_port()
    baud = int(baud or os.environ.get("HIL_BAUD", "115200"))
    return serial.Serial(port, baud, timeout=timeout)


def parse_temp(value):
    m = TEMP_RE.match(value.strip())
    return float(m.group(1)) if m else None


def line_has(line, marker):
    """True if `line` contains `marker`.

    The capture can mangle a line in more than one way. The tail can be lost
    (the next line then follows it), or noise on the wire can corrupt a run of
    characters in the middle of it - on the F103 bench both were seen, the
    second one replacing "nabled - WFE " with eight bytes of rubbish.

    Either way the line's *marker* - the distinctive part that says which
    line this is - is what the check is about, and it usually survives. A
    line that lost its marker proves nothing and is not accepted.
    """
    return bool(marker) and marker in line


def parse_measurement(line):
    """Return (rom, temp, error) for a `ROM: value` line, else None.

    Exactly one of temp/error is non-None. The error wording differs per
    example (`no sensor detected.` / `CRC check failed.` / `generic failure.`
    in 2/4/7, the short `no sensor` / `CRC fail` / `error` in 6), so a value
    that is not a temperature is compared against those phrases.

    A value that is neither a temperature nor one of those phrases was cut
    off by the capture. It is not a device error, and it is not a reading
    either - the line is skipped, so a torn burst cannot be reported as a
    sensor that failed.
    """
    m = MEAS_RE.match(line)
    if not m:
        return None
    rom, value = m.group(1), m.group(2)
    temp = parse_temp(value)
    if temp is not None:
        return rom, temp, None
    if any(value.strip().startswith(e) for e in ERROR_VALUES):
        return rom, None, value
    return None  # torn: the value was cut off mid-flight


def wait_for_banner(uart, banner, timeout=5.0):
    """Read until the example's boot banner arrives; return the bytes after it.

    The previous firmware keeps transmitting while the new image is being
    flashed, so its tail would otherwise be captured as this run's output.
    Instead of sleeping a fixed interval and hoping the tail has ended before
    the new firmware's banner does, this reads and throws away complete lines
    until the banner turns up - the proof that the new image has booted and
    its own output is now arriving. Returns the banner line plus everything
    already read after it, so the caller can seed the check's LineReader with
    it; returns None on timeout (the caller falls back to clearing the RX
    buffer and proceeding as before).
    """
    buf = b""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        uart.timeout = 0.2
        chunk = uart.read(4096)
        if chunk:
            buf += chunk
        while b"\r\n" in buf:
            raw, rest = buf.split(b"\r\n", 1)
            if line_has(raw.decode("ascii", "replace").strip(), banner):
                # Keep the banner line and every byte already read after it.
                return raw + b"\r\n" + rest
            buf = rest
    return None


class LineReader:
    """Reads UART lines, tolerating CRLF, partial writes and overrun-split lines.

    The examples drive UART through a small non-blocking ring buffer drained one
    byte per main-loop poll, so a burst can tear a line in two - seen on real
    H503 hardware as `  ROM: 28 F1 39` followed by ` AD 0B 00 00 D9`, and on
    the F103 bench as a mode line that lost its tail and took the next line
    with it, and as one whose middle was replaced by noise. Lines are
    therefore matched independently, and line_has() accepts a line that
    still carries its marker; a fragment that lost its marker matches
    nothing, as it should.
    """

    def __init__(self, ser, flush_timeout=1.0, initial=b""):
        self.ser = ser
        self.buf = initial
        self.flush_timeout = flush_timeout
        self.last_data = time.monotonic()

    def iter_lines(self, window, sink=None):
        """Yield (poll_stamp, line) while `window` lasts.

        `window` is a number of seconds, or a Budget - usually ctx.window, and
        always a Budget when the call site left HIL_TIMEOUT unset. A Budget is
        consulted on every poll instead of a fixed deadline and every
        delivered line is shown to budget.note_line() first, so the firmware's
        census line, arriving mid-capture, can extend the window for the
        devices it reports.
        """
        budget = window if hasattr(window, "remaining") else None
        end = None if budget is not None else time.monotonic() + window
        while True:
            remaining = (
                budget.remaining()
                if budget is not None
                else end - time.monotonic()
            )
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
                if budget is not None:
                    budget.note_line(line)
                if sink is not None:
                    sink.append((now, line))
                yield now, line
            if self.buf and (now - self.last_data) >= self.flush_timeout:
                raw, self.buf = self.buf, b""
                self.last_data = now
                line = raw.decode("ascii", "replace").strip()
                if budget is not None:
                    budget.note_line(line)
                if sink is not None:
                    sink.append((now, line))
                yield now, line


def find_port(baud=None, window=2.0):
    """Scan the COM ports for one that carries example output.

    Returns (device, tried); device is None when no port matched. The firmware
    is transmit-only, so reading a port cannot disturb it, and ports that
    refuse to open belong to someone else's session and are skipped. The scan
    runs twice: a short pass for consoles that stream steadily, then a longer
    one for the gaps between (7_low_power prints every ~5 s). `tried` lists
    every candidate, for the caller's error message.
    """
    import serial
    from serial.tools import list_ports

    baud = int(baud or os.environ.get("HIL_BAUD", "115200"))
    tried = []
    for pass_window in (window, window * 3.0):
        for info in sorted(list_ports.comports(), key=lambda p: p.device):
            if info.device not in tried:
                tried.append(info.device)
            try:
                ser = serial.Serial(info.device, baud, timeout=0.2)
            except Exception:
                continue
            try:
                reader = LineReader(ser, flush_timeout=0.8)
                for _, line in reader.iter_lines(pass_window):
                    if FINGERPRINT_RE.search(line):
                        return info.device, tried
            except Exception:
                continue
            finally:
                ser.close()
    return None, tried


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
    if not MCU:
        raise RuntimeError("no bench selected; select_bench() before building")
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
