#!/usr/bin/env python3
"""Capture the firmware's USART1 log from a host COM port.

The board has no USB-CDC of its own: COM5 is a USB-UART dongle wired to PA9
(USART1 TX) at 115200 8N1.  Optionally reset the MCU over SWD right after the
port is open, so the capture starts with the firmware's first UART byte.

Usage:
    python tools/uart_capture.py --reset --seconds 12
"""

import argparse
import subprocess
import sys
import time

import serial

PROGRAMMER = (
    r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer"
    r"\bin\STM32_Programmer_CLI.exe"
)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--reset", action="store_true",
                    help="reset the MCU over SWD after opening the port")
    ap.add_argument("--programmer", default=PROGRAMMER)
    args = ap.parse_args()

    with serial.Serial(args.port, args.baud, bytesize=8, parity="N",
                       stopbits=1, timeout=0.2) as port:
        port.reset_input_buffer()

        if args.reset:
            proc = subprocess.run(
                [args.programmer, "-c", "port=SWD", "-rst"],
                capture_output=True, text=True,
            )
            ok = "Software reset is performed" in proc.stdout
            sys.stderr.write("SWD reset: %s\n" % ("ok" if ok else "FAILED"))
            if not ok:
                sys.stderr.write(proc.stdout[-800:] + proc.stderr[-400:])

        start = time.time()
        data = bytearray()
        while time.time() - start < args.seconds:
            chunk = port.read(4096)
            if chunk:
                data.extend(chunk)

    sys.stdout.write(data.decode("utf-8", "replace"))
    sys.stdout.write("\n--- %d bytes in %.1f s ---\n" % (len(data), args.seconds))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
