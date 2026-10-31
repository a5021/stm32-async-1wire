#!/usr/bin/env python3
"""Turn sigrok-cli onewire_link annotations into a compact 1-Wire transcript.

The bundled onewire_network decoder in the local sigrok build crashes with an
API mismatch, so this script does the network layer itself: it groups slot bits
in transmission order, LSB first, and prints one line per transaction.

The transcript mixes master writes and device read-back bits; use the UART log
for the semantic reference and this capture for timing/protocol proof.

Usage:
    sigrok-cli -i tools/la_boot.sr -C D0=owr -P onewire_link \
        -A onewire_link > tools/la_boot_link.txt
    python tools/la_decode.py tools/la_boot_link.txt
"""

import sys

PREFIX = "onewire_link-1: "


def pack(bits: list) -> str:
    out = []
    for index in range(len(bits) // 8):
        value = 0
        for i, bit in enumerate(bits[index * 8:index * 8 + 8]):
            value |= bit << i
        out.append("%02X" % value)
    rest = len(bits) % 8
    if rest:
        out.append("(%d bits left)" % rest)
    return " ".join(out)


def main(path: str) -> int:
    bits: list = []
    presence = ""
    transaction = 0
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if PREFIX not in line:
                continue
            text = line.split(PREFIX, 1)[1].strip()
            if text == "Reset":
                if transaction:
                    print("T%-2d %-14s %s" % (transaction, presence, pack(bits)))
                bits = []
                presence = ""
                transaction += 1
            elif text.startswith("Presence"):
                presence = text
            elif text.startswith("Bit: "):
                bits.append(int(text[-1]))
    if transaction:
        print("T%-2d %-14s %s" % (transaction, presence, pack(bits)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1] if len(sys.argv) > 1 else "tools/la_boot_link.txt"))
