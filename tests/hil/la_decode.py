#!/usr/bin/env python3
"""Decode the 1-Wire traffic in a logic-analyzer capture.

Run:
    python tests/hil/la_decode.py bench_logs/hil/la_wait_750.bin
    python tests/hil/la_decode.py cap.bin --rate 4M --channel 0
    python tests/hil/la_decode.py cap.bin --log bench_logs/hil/smoke_<run>.log

What it proves: the *content* on the wire, which the geometry check
(la_capture.py) cannot see. la_capture measures how long the pulses are; this
reads the bits they carry, rebuilds the ROM/search/payload bytes and checks
them against the CRC the device computed. The independent half is --log: the
application prints the temperature it parsed, the analyzer holds the bytes the
device actually sent, and the two computations must agree. The host model
cannot make that comparison - it is the same code on both sides.

The decode is strict on purpose. A slot width in the gap between a '1' and a
'0' is an error, not a guess; an unknown command is an error, not a silently
skipped transaction; a CRC that does not match fails the run. A decoder that
guesses would turn a red capture green.

The DS2431 cycle is checked one step further than "the log and the wire
agree": after Copy Scratchpad the same row is read again and must return what
was written. Without that, an application that prints the pattern it meant to
write agrees with itself while the EEPROM still holds the old byte. When the
row already held that pattern before the write, the report says so: the read
path is proven, the copy is not. The example's own "matches/DIFFERS" line is a
claim, not evidence: it is read separately and held against what the wire
says, so a comparison that stopped working cannot pass on the bytes alone.

The report opens with what makes the run reproducible - the capture and log
hashes, the slot geometry, and the `# hil:` identity the log carries - and
ends with one `ИТОГ: PASS/FAIL` line. A capture on its own says what the wire
did, not which image drove it; the header the HIL run writes into its log
names the version, the commit, the build's EXT flags, the UART buffer size and
the image hash.

Understood: Read ROM / Skip ROM / Match ROM / Search ROM / Alarm Search, the
DS18B20 Convert and Read Scratchpad, and the DS2431 Write Scratchpad, Read
Scratchpad (with its inverted CRC-16), Copy Scratchpad and Read Memory. The
function table is chosen by the ROM family byte, because 0x55 and 0xF0 mean
different things on a DS2431 and a DS18B20.

Requires numpy (via la_capture) and a capture from la_capture.py.
"""

import argparse
import json
import os
import re
import sys

import numpy as np

from la_capture import analyse, parse_rate, read_config, sha256_file

# Dallas/Maxim CRC-8: x^8 + x^5 + x^4 + 1.
CRC8_POLY = 0x8C

# A byte is 8 slots; the trailing release pulse of a pre-encoded command is a
# released line, so it produces no falling edge and does not appear here.
BITS_PER_BYTE = 8

ROM_COMMANDS = {
    0x33: ("READ ROM", 8),
    0xCC: ("SKIP ROM", 0),
    0x55: ("MATCH ROM", 8),
}
# Function commands depend on the family the master selected: 0x55 and 0xF0
# mean different things on a DS2431 and on a DS18B20, and the ROM byte says
# which device the transaction is talking to.
FUNCTION_COMMANDS = {
    0x28: {
        0x44: ("CONVERT", 0),
        0xBE: ("READ SCRATCHPAD", 9),   # 8 data + CRC8
    },
    0x2D: {
        0x0F: ("WRITE SCRATCHPAD", 10),  # TA1 TA2 + 8 data
        0xAA: ("READ SCRATCHPAD", 13),   # TA1 TA2 ES + 8 data + CRC16
        0x55: ("COPY SCRATCHPAD", 3),    # TA1 TA2 ES
        0xF0: ("READ MEMORY", 2),        # TA1 TA2 + the rest of the slots
    },
}
SEARCH_COMMANDS = {
    0xF0: "SEARCH ROM",
    0xEC: "ALARM SEARCH",
}
# DS2431 Copy Scratchpad requires the ending offset of a full row.
COPY_ES = 0x07


class DecodeError(Exception):
    """The capture is not a valid 1-Wire exchange we understand."""


# A master pulse is ONE = 5 us; anything much shorter cannot be one, and the
# analyzer's last or first edge can leave a fraction of a pulse behind.
MIN_WIDTH_US = 2.5
# Slots follow each other at the configured period; a silence longer than this
# is the end of the exchange, not a gap between two slots.
MAX_SLOT_GAP_US = 2000.0


def clean(widths, gaps, min_width=MIN_WIDTH_US):
    """Drop sub-pulse capture artifacts, keeping the real gaps between pulses."""
    starts = np.concatenate(([0.0], np.cumsum(gaps)))
    keep = widths >= min_width
    dropped = int((~keep).sum())
    if dropped:
        widths = widths[keep]
        starts = starts[keep]
        gaps = np.diff(starts) if len(starts) > 1 else np.zeros(0)
    return widths, gaps, dropped


def crc8(data):
    c = 0
    for b in data:
        for _ in range(8):
            mix = (c ^ b) & 1
            c >>= 1
            if mix:
                c ^= CRC8_POLY
            b >>= 1
    return c


def crc16(data):
    """Maxim CRC-16 (reflected 0x8005, init 0, no final XOR)."""
    c = 0
    for b in data:
        for _ in range(8):
            mix = (c ^ b) & 1
            c >>= 1
            if mix:
                c ^= 0xA001
            b >>= 1
    return c


def temperature(scratchpad):
    """DS18B20 scratchpad bytes 0-1, signed 1/16 degree."""
    raw = (scratchpad[1] << 8) | scratchpad[0]
    if raw & 0x8000:
        raw -= 0x10000
    return raw / 16.0


def hexs(data):
    return " ".join("%02X" % b for b in data)


class Slot:
    __slots__ = ("bit", "width", "period")

    def __init__(self, bit, width, period):
        self.bit = bit
        self.width = width
        self.period = period


class Frame:
    __slots__ = ("command", "name", "rom", "payload", "crc_ok", "value",
                 "passes", "empty_terminator", "addr", "data", "problem")

    def __init__(self, command, name):
        self.command = command
        self.name = name
        self.rom = b""
        self.payload = b""
        self.crc_ok = None
        self.value = None
        self.passes = []
        self.empty_terminator = False
        self.addr = b""      # DS2431 TA1 TA2
        self.data = b""      # the memory/scratchpad bytes, without address
        self.problem = None  # a protocol violation that has no CRC of its own


class Transaction:
    __slots__ = ("reset", "presence", "presence_delay", "frames", "recovery")

    def __init__(self, reset, presence, presence_delay):
        self.reset = reset
        self.presence = presence
        self.presence_delay = presence_delay
        self.frames = []
        self.recovery = None


class Pass:
    __slots__ = ("rom", "crc_ok", "collisions", "ended_empty")

    def __init__(self, rom, crc_ok, collisions, ended_empty):
        self.rom = rom
        self.crc_ok = crc_ok
        self.collisions = collisions
        self.ended_empty = ended_empty


# 1-Wire slaves hold the line low for a read-0 for at least 15 us (tLOW0),
# while the master's '1' pulse is ONE. The split sits halfway between those two
# floors, not halfway between ONE and ZERO: a read-0 is ~29 us here, much
# shorter than the master's write-0.
MIN_ZERO_US = 15.0


def read_bit(width, cfg):
    """'1' or '0' from the low width; intervals in between are an error.

    A read-0 (the device holds the line past the master's pulse) is shorter
    than a write-0, but both mean bit 0, so the upper boundary only has to
    separate a bit from a presence pulse.
    """
    one, zero = cfg["one"], cfg["zero"]
    limit = (one + MIN_ZERO_US) / 2.0  # 10 us for ONE = 5
    if width <= limit:
        return 1
    if width <= 2.0 * zero:  # a write-0 or a device-extended read-0
        return 0
    raise DecodeError("ширина %.1f мкс не слот: между '0' и presence" % width)


def slot_period(cfg, gaps):
    """The configured slot period, chosen from the two guard bands.

    Both guard bands are in inc/ow_config.h; the capture says which build it
    was, and a period that matches neither is an error rather than a number to
    be extrapolated.

    The band is +-10 us rather than exact: a plain transaction holds 166.0 us
    to the sample, but the merged search window reloads three different slot
    lengths (166, 168, 170), so demanding one period would reject a correct
    search. Ten microseconds still catches a slot that doubled.
    """
    candidates = [
        cfg["one"] + cfg["zero"] + cfg["guard_default"],
        cfg["one"] + cfg["zero"] + cfg["guard_parasite"],
    ]
    if len(gaps) == 0:
        raise DecodeError("нет интервалов между слотами")
    for candidate in candidates:
        if all(abs(g - candidate) <= 10.0 for g in gaps):
            return candidate
    raise DecodeError(
        "период слота %.1f-%.1f мкс не совпал ни с %s (конфигурация)"
        % (min(gaps), max(gaps),
           " / ".join("%.0f" % c for c in candidates)))


def take_bytes(slots, i, count):
    """Read count whole bytes from the slot cursor, LSB first."""
    need = count * BITS_PER_BYTE
    if len(slots) - i < need:
        raise DecodeError("обрыв: %d слотов на %d байт" % (len(slots) - i, count))
    data = bytes(
        sum(slots[i + b * BITS_PER_BYTE + k].bit << k
            for k in range(BITS_PER_BYTE))
        for b in range(count))
    return data, i + need


def scratchpad_data_len(ta1, es):
    """Bytes the DS2431 actually transmitted: E[2:0] - T[2:0] + 1, 1..8."""
    n = (es & 0x07) - (ta1 & 0x07) + 1
    if n <= 0:
        n += 8
    if n > 8:
        raise DecodeError("READ SCRATCHPAD: окно %d байт (TA1=%02X ES=%02X)"
                          % (n, ta1, es))
    return n


def parse_transaction(slots, cfg):
    """Frames of one transaction, starting with its ROM command."""
    frames = []
    i = 0
    first = True
    family = None
    while i < len(slots):
        command, i = take_bytes(slots, i, 1)
        command = command[0]

        if first and command in SEARCH_COMMANDS:
            passes, used, empty = parse_search(slots[i:])
            i += used
            frame = Frame(command, SEARCH_COMMANDS[command])
            frame.passes = passes
            frame.empty_terminator = empty
            frames.append(frame)
            first = False
            continue

        if first:
            if command not in ROM_COMMANDS:
                raise DecodeError("первый байт 0x%02X не ROM-команда" % command)
            name, nbytes = ROM_COMMANDS[command]
            frame = Frame(command, name)
            if nbytes:
                frame.rom, i = take_bytes(slots, i, nbytes)
                frame.crc_ok = crc8(frame.rom[:7]) == frame.rom[7]
                family = frame.rom[0]
            frames.append(frame)
            first = False
            continue

        table = FUNCTION_COMMANDS.get(family)
        if table is None:
            raise DecodeError(
                "функция 0x%02X без MATCH ROM: семейство неизвестно (0x55 и "
                "0xF0 означают разное у 28 и 2D)" % command)
        if command not in table:
            raise DecodeError("неизвестная функция 0x%02X для семейства %02X"
                              % (command, family))
        name, nbytes = table[command]
        frame = Frame(command, name)

        if family == 0x2D and command == 0xF0:
            # Read Memory: two address bytes, then the window the master
            # clocks. Its length is the master's choice, so it is whatever is
            # left of the transaction - not a guess, just the frame boundary.
            frame.addr, i = take_bytes(slots, i, 2)
            rest = len(slots) - i
            if rest <= 0 or rest % BITS_PER_BYTE:
                raise DecodeError("READ MEMORY: хвост из %d слотов" % rest)
            frame.data, i = take_bytes(slots, i, rest // BITS_PER_BYTE)
            frame.payload = frame.addr + frame.data
        else:
            payload, i = take_bytes(slots, i, nbytes)
            frame.payload = payload
            if family == 0x28 and command == 0xBE:
                frame.crc_ok = crc8(payload[:8]) == payload[8]
                frame.value = temperature(payload)
            elif family == 0x2D and command == 0xAA:
                frame.addr = payload[:2]
                n = scratchpad_data_len(payload[0], payload[2])
                frame.data = payload[3:3 + n]
                got = payload[3 + n] | (payload[4 + n] << 8)
                expected = (~crc16(bytes([0xAA]) + payload[:3 + n])) & 0xFFFF
                frame.crc_ok = got == expected
                tail = payload[3 + n + 2:]
                if tail and any(b != 0xFF for b in tail):
                    frame.problem = "после CRC16 не 0xFF"
            elif family == 0x2D and command in (0x0F, 0x55):
                frame.addr, frame.data = payload[:2], payload[2:]
                if command == 0x55 and payload[2] != COPY_ES:
                    frame.problem = ("ES=0x%02X, ожидалось 0x%02X"
                                     % (payload[2], COPY_ES))
        frames.append(frame)
    if not frames:
        raise DecodeError("транзакция без команд")
    return frames


def parse_search(slots):
    """Search passes after 0xF0/0xEC, in threes: bit, complement, direction.

    The master writes the bit it chose; the written directions *are* the ROM
    of that pass, so no search stack is needed here. The pair must be one of
    (1,0) all ones, (0,1) all zeros, (0,0) a collision - and (1,1) only as the
    terminator of a pass in which no device answered.
    """
    passes = []
    i = 0
    empty = False
    while i < len(slots):
        rom = bytearray()
        bits = []
        collisions = 0
        ended = False
        for k in range(64):
            if len(slots) - i < 2:
                raise DecodeError("поиск обрывается на бите %d" % k)
            b, c = slots[i].bit, slots[i + 1].bit
            i += 2
            if b == 1 and c == 1:
                ended = True
                empty = True
                break
            if b == 0 and c == 0:
                collisions += 1
            if len(slots) - i < 1:
                raise DecodeError("поиск: нет бита направления после пары "
                                  "на бите %d" % k)
            d = slots[i].bit
            i += 1
            if b == 1 and d != 1:
                raise DecodeError("поиск, бит %d: все единицы, а записано %d"
                                  % (k, d))
            if c == 1 and d != 0:
                raise DecodeError("поиск, бит %d: все нули, а записано %d"
                                  % (k, d))
            bits.append(d)
        if ended:
            break
        rom = bytes(sum(bits[b * 8 + k] << k for k in range(8))
                    for b in range(8))
        passes.append(Pass(rom, crc8(rom[:7]) == rom[7], collisions, ended))
    return passes, i, empty


def decode(widths, gaps, cfg):
    """Transactions in time order, plus the slots before the first reset.

    The capture may start in the middle of an exchange; that leading partial
    transaction is reported rather than decoded or silently dropped.
    Returns (transactions, orphan_slots, dropped_artifacts).
    """
    widths, gaps, dropped = clean(widths, gaps)
    transactions = []
    orphan = 0
    i = 0
    n = len(widths)
    # A reset is 480 us; a presence pulse is 60-240 us. The boundary has to
    # sit between them, not at "twice a zero" - a long presence would be
    # mistaken for the next reset.
    reset_min = 300.0
    while i < n:
        w = float(widths[i])
        if w <= reset_min:
            orphan += 1
            i += 1
            continue

        # reset, then the presence pulse the devices answer it with
        reset = w
        if i + 1 >= n:
            raise DecodeError("reset без presence в конце захвата")
        delay = float(gaps[i]) - reset
        presence = float(widths[i + 1])
        if not (60.0 <= presence <= 260.0):
            raise DecodeError("после reset ширина %.1f мкс - не presence "
                              "(1-Wire: 60-240 мкс)" % presence)
        if not (5.0 <= delay <= 80.0):
            raise DecodeError("presence через %.1f мкс после reset "
                              "(ожидалось 15-60)" % delay)
        txn = Transaction(reset, presence, delay)
        first_gap = float(gaps[i + 1]) if i + 1 < len(gaps) else None
        i += 2

        slots = []
        while i < n and float(widths[i]) <= reset_min:
            w = float(widths[i])
            if w > 2.0 * cfg["zero"]:
                raise DecodeError("presence внутри транзакции (%.1f мкс)" % w)
            period = float(gaps[i]) if i < len(gaps) else None
            slots.append(Slot(read_bit(w, cfg), w, period))
            # A long silence ends the exchange even if no reset follows: the
            # line is idle, so whatever comes next is not a slot.
            if period is not None and period > MAX_SLOT_GAP_US:
                i += 1
                break
            i += 1

        if not slots:
            raise DecodeError("reset без обмена: на шине никто не ответил?")
        inner = [s.period for s in slots[:-1]]  # the last gap leads to the reset
        slot_period(cfg, inner)
        txn.frames = parse_transaction(slots, cfg)
        txn.recovery = None if first_gap is None else first_gap - presence
        transactions.append(txn)
    return transactions, orphan, dropped


def report(transactions, orphan, dropped):
    print("\n=== содержимое 1-Wire ===")
    if dropped:
        print("  отброшено %d импульсов короче %.1f мкс (артефакты краёв)"
              % (dropped, MIN_WIDTH_US))
    if orphan:
        print("  до первого reset: %d слотов (начало захвата, пропущено)" % orphan)
    for k, txn in enumerate(transactions, 1):
        print("  транзакция %d: reset %.1f мкс, presence %.1f мкс (через %.1f)"
              % (k, txn.reset, txn.presence, txn.presence_delay))
        for f in txn.frames:
            # 0xF0 is SEARCH ROM in the first position and READ MEMORY later;
            # a search frame carries passes, a memory read carries bytes.
            if f.command in SEARCH_COMMANDS and not f.payload:
                print("    %s (0x%02X): %d проходов%s"
                      % (f.name, f.command, len(f.passes),
                         ", пустой терминатор" if f.empty_terminator else ""))
                for p in f.passes:
                    print("      %s  CRC %s, коллизий %d"
                          % (hexs(p.rom), "ok" if p.crc_ok else "FAIL",
                             p.collisions))
            elif f.rom:
                print("    %s %s  CRC %s"
                      % (f.name, hexs(f.rom), "ok" if f.crc_ok else "FAIL"))
            elif f.addr:
                where = "TA1=%02X TA2=%02X" % (f.addr[0], f.addr[1])
                if f.command == 0x55:  # Copy Scratchpad: address and ES only
                    print("    %s  %s  ES=%02X" % (f.name, where, f.payload[2]))
                else:
                    crc = ""
                    if f.crc_ok is not None:
                        crc = "  CRC16 %s" % ("ok" if f.crc_ok else "FAIL")
                    print("    %s  %s: %s%s"
                          % (f.name, where, hexs(f.data), crc))
            elif f.payload:
                extra = ". %.4f C" % f.value if f.value is not None else ""
                print("    %s: %s  CRC %s%s"
                      % (f.name, hexs(f.payload),
                         "ok" if f.crc_ok else "FAIL", extra))
            else:
                print("    %s (0x%02X)" % (f.name, f.command))


def failures(transactions):
    bad = 0
    for txn in transactions:
        for f in txn.frames:
            if f.crc_ok is False:
                bad += 1
                print("  FAIL CRC: %s %s"
                      % (f.name, hexs(f.rom or f.payload)))
            if f.problem:
                bad += 1
                print("  FAIL %s: %s" % (f.name, f.problem))
            for p in f.passes:
                if not p.crc_ok:
                    bad += 1
                    print("  FAIL CRC поиска: %s" % hexs(p.rom))
    return bad


HEX8 = r"((?:[0-9A-Fa-f]{2}\s+){7}[0-9A-Fa-f]{2})"
HEX11 = r"((?:[0-9A-Fa-f]{2}\s+){10}[0-9A-Fa-f]{2})"


def as_bytes(text):
    return bytes(int(x, 16) for x in text.split())


def parse_log(path):
    """What the application reported, in its order.

    Returns (ROMs, temperature readings, census, DS2431 events). The events
    are the EEPROM traffic the example printed: the baseline read, the written
    pattern, the scratchpad frame, the copy and the read-back - each one is a
    known byte string that the wire has to carry unchanged.

    The example's own read-back verdict is not an event: it is a claim about
    the device, and parse_log_claims() is where it is read.
    """
    roms = set()
    readings = []
    events = []
    found = None
    device = None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = re.search(r"Found (\d+) device", line)
            if m:
                found = int(m.group(1))
            m = re.search(r"Selected ROM:\s+" + HEX8, line)
            if m:
                device = as_bytes(m.group(1))
                roms.add(device)
                continue
            # "EEPROM: ..." contains "ROM:", so the EEPROM line is claimed
            # before the ROM line is looked for.
            m = re.search(r"EEPROM:\s+" + HEX8, line)
            if m:
                if device is not None:
                    events.append((device, "read", as_bytes(m.group(1))))
                continue
            m = re.search(r"(?<!EEP)ROM:\s+" + HEX8, line)
            if m:
                roms.add(as_bytes(m.group(1)))
                continue
            m = re.search(r"Write Scratchpad \(0x0F\):\s+" + HEX8, line)
            if m:
                if device is not None:
                    events.append((device, "write", as_bytes(m.group(1))))
                continue
            m = re.search(r"frame:\s+" + HEX11, line)
            if m:
                if device is not None:
                    events.append((device, "frame", as_bytes(m.group(1))))
                continue
            if "copied to EEPROM" in line and device is not None:
                events.append((device, "copy", b""))
                continue
            m = re.search(HEX8 + r":\s+(-?\d+\.\d+)\s+C", line)
            if m:
                readings.append((as_bytes(m.group(1)), float(m.group(2))))
    return roms, readings, found, events


def tenths(value):
    """The value the application prints: its temperature is in tenths."""
    scaled = value * 10.0
    return int(scaled + 0.5) if scaled >= 0 else -int(-scaled + 0.5)


# The verdict the example prints after its own read-back comparison. It is a
# claim about the device, not evidence about it: the wire is what this decoder
# trusts, and a broken comparison in the application - one that always says
# "matches" - leaves the bytes correct and the verdict wrong.
CLAIM_MATCH = "read-back matches the write"
CLAIM_DIFFER = "read-back DIFFERS from the write"


def parse_log_claims(path):
    """The read-back verdicts the application printed, per selected device.

    A log with the right bytes and a wrong verdict is exactly the failure this
    check exists to catch: comparing the claim against the wire is the only
    way to see that the application's own check did not really run. A log from
    an image that prints no verdict yields an empty dict, and nothing is
    claimed either way.
    """
    claims = {}
    device = None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = re.search(r"Selected ROM:\s+" + HEX8, line)
            if m:
                device = as_bytes(m.group(1))
                continue
            if device is None:
                continue
            if CLAIM_MATCH in line:
                claims[device] = True
            elif CLAIM_DIFFER in line:
                claims[device] = False
    return claims


def wire_events(transactions):
    """DS2431 memory traffic on the wire, in transaction order.

    Only the bytes the device carries matter: the address window and the data
    (or the scratchpad frame, CRC included).
    """
    events = []
    for txn in transactions:
        rom = next((f.rom for f in txn.frames if f.rom), None)
        if rom is None or rom[0] != 0x2D:
            continue
        for f in txn.frames:
            # 0x55 is MATCH ROM as the first byte and COPY SCRATCHPAD later;
            # a frame with a ROM is the former.
            if f.rom:
                continue
            if f.command == 0xF0:
                events.append((rom, "read", f.data))
            elif f.command == 0x0F:
                events.append((rom, "write", f.data))
            elif f.command == 0xAA:
                events.append((rom, "frame", f.payload[:11]))
            elif f.command == 0x55:
                events.append((rom, "copy", b""))
    return events


def compare_events(wire, log):
    """The DS2431 cycle, byte for byte.

    The log prints the pattern it wrote, the frame it read and the EEPROM it
    read back; those are constants, so the comparison is an equality and needs
    no clock alignment. A capture armed before the flash can hold the previous
    image's cycle too, so the log is searched as a contiguous run anywhere in
    the wire's sequence.
    """
    if not log:
        print("  FAIL на проводе трафик DS2431, а в логе его нет")
        return 1
    for start in range(len(wire) - len(log) + 1):
        if all(wire[start + k] == log[k] for k in range(len(log))):
            print("  DS2431: %d команд совпали с логом байт в байт" % len(log))
            return 0

    print("  FAIL последовательность DS2431 не совпала (%d команд на проводе, "
          "%d в логе)" % (len(wire), len(log)))
    for k in range(min(len(wire), len(log))):
        if wire[k] != log[k]:
            print("    команда %d: провод %s, лог %s"
                  % (k + 1, event_str(wire[k]), event_str(log[k])))
            break
    return 1


def event_str(event):
    rom, kind, data = event
    return "%s %s %s" % (hexs(rom), kind, hexs(data) if data else "-")


def ds2431_readback_errors(events, claims=None):
    """The copy is proven by reading the row back, not by saying it worked.

    compare_events() proves the wire and the log agree. It does not prove the
    write took: a device that ignores Copy Scratchpad, or an application that
    prints the pattern it meant to write instead of the one it read, leaves
    both sides identical and the EEPROM holding the old bytes. The device
    contract is narrow - write P, copy, read the same row, get P - so that is
    all this checks, over the wire's own bytes.

    A cycle the capture only partly holds is not guessed at: without the Write
    in view there is nothing to compare the read against, and the copy is not
    tracked. `claims` is the application's own verdict per device (see
    parse_log_claims); it is held against the wire's verdict, and a claim the
    capture cannot cover is a note rather than a failure, because a short
    capture is not the device lying.
    """
    bad = 0
    written = {}
    awaiting = {}
    baseline = {}
    results = {}
    confirmed = 0
    changed = 0
    for rom, kind, data in events:
        if kind == "write":
            written[rom] = data
            awaiting.pop(rom, None)
        elif kind == "frame" and rom in written:
            # frame = TA1 TA2 ES + the data the scratchpad returned.
            if data[3:] != written[rom]:
                print("  FAIL %s: кадр scratchpad %s, записано %s"
                      % (hexs(rom), hexs(data[3:]), hexs(written[rom])))
                bad += 1
        elif kind == "copy" and rom in written:
            awaiting[rom] = written[rom]
        elif kind == "read" and rom in awaiting:
            want = awaiting.pop(rom)
            if len(data) < len(want) or data[:len(want)] != want:
                results[rom] = False
                print("  FAIL %s: после копирования EEPROM %s, записано %s"
                      % (hexs(rom), hexs(data), hexs(want)))
                bad += 1
            else:
                results[rom] = True
                confirmed += 1
                old = baseline.get(rom)
                if old is None:
                    # No baseline read in view: the read-back matches the
                    # write, but whether the row changed cannot be told.
                    pass
                elif old == want:
                    # Reading the same bytes back proves the read path, but
                    # not the write: an EEPROM that already held them looks
                    # identical when the copy is ignored.
                    print("  примечание %s: записанное уже было в EEPROM - "
                          "копирование этим прогоном не доказано" % hexs(rom))
                else:
                    # The row changed and the device returned the new bytes:
                    # the Copy cannot have been ignored.
                    changed += 1
                    print("  DS2431 %s: ряд %s -> %s, чтение после Copy "
                          "подтвердило запись"
                          % (hexs(rom), hexs(old), hexs(want)))
        elif kind == "read":
            baseline[rom] = data
    for rom in sorted(awaiting):
        print("  FAIL %s: запись скопирована, но не прочитана - EEPROM "
              "не подтверждён" % hexs(rom))
        bad += 1

    for rom in sorted(claims or {}):
        if rom not in results:
            print("  примечание %s: вердикт лога вне захвата - проверить "
                  "нечем" % hexs(rom))
        elif claims[rom] != results[rom]:
            print("  FAIL %s: лог утверждает %s, а провод - %s"
                  % (hexs(rom),
                     "совпадение" if claims[rom] else "расхождение",
                     "совпадение" if results[rom] else "расхождение"))
            bad += 1

    if confirmed:
        print("  DS2431: %d записей подтверждены чтением EEPROM" % confirmed)
    if changed:
        print("  DS2431: %d из них изменили ряд - копирование доказано"
              % changed)
    return bad


def compare_log(transactions, roms, readings, found, events=(), claims=None):
    """The analyzer's bytes and the application's report, item by item.

    Temperatures: both start at the boot the log covers - the application
    searches once, so the last search in the capture is that boot - and the
    reads after it must match ROM for ROM and round to the same tenth. The
    wire's value is exact (1/16 C), so this is an equality, not a tolerance.

    DS2431: the EEPROM cycle is a fixed sequence of byte strings the example
    printed, compared as one contiguous run (see compare_events()), and the
    application's own read-back verdict is held against the wire's.
    """
    bad = 0
    wire_roms = set()
    reads = []
    last_search = None
    for k, txn in enumerate(transactions):
        rom = next((f.rom for f in txn.frames if f.rom), None)
        for f in txn.frames:
            # 0xF0 is READ MEMORY when the family is 0x2D: only a frame with
            # passes is the search that starts the log's run.
            if f.command in SEARCH_COMMANDS and not f.payload:
                last_search = k
                for p in f.passes:
                    wire_roms.add(p.rom)
            if f.rom:
                wire_roms.add(f.rom)
            if f.payload and f.value is not None and rom is not None:
                reads.append((k, rom, f.value))

    for rom in sorted(roms):
        if rom not in wire_roms:
            print("  FAIL %s есть в логе, но не на проводе" % hexs(rom))
            bad += 1
    if found is not None and found != len(roms):
        print("  FAIL лог объявил %d устройств, а перечислил %d"
              % (found, len(roms)))
        bad += 1

    # Only the reads after the boot's search belong to the logged run: a
    # capture armed before the flash also holds the previous image's round.
    if last_search is not None:
        reads = [r for r in reads if r[0] > last_search]
    paired = 0
    for (_, rom, value), (log_rom, log_value) in zip(reads, readings):
        if rom != log_rom:
            print("  FAIL на проводе %s, в логе %s: это разные прогоны?"
                  % (hexs(rom), hexs(log_rom)))
            bad += 1
            break
        if tenths(value) != tenths(log_value):
            print("  FAIL %s: провод %.4f C (%.1f), приложение %.1f C"
                  % (hexs(rom), value, tenths(value) / 10.0, log_value))
            bad += 1
        paired += 1

    # zip() stops at the shorter list, and a capture may be shorter than the
    # logged run - but a comparison that paired nothing at all is not a pass.
    if readings and not paired:
        print("  FAIL ни одно из %d измерений лога не сверено" % len(readings))
        bad += 1
    elif paired < len(readings):
        print("  сверено %d из %d измерений лога (захват короче прогона)"
              % (paired, len(readings)))

    wire_ds2431 = wire_events(transactions)
    if events or wire_ds2431 or claims:
        bad += compare_events(wire_ds2431, list(events))
        bad += ds2431_readback_errors(wire_ds2431, claims)

    # The example filters for one family; a device of another family is
    # expected to be on the wire and absent from its report. A device of the
    # family the log is about, missing from the log, is a real miss.
    families = {r[0] for r in roms}
    extra = wire_roms - roms
    other = [r for r in extra if r[0] not in families]
    for rom in sorted(extra):
        if rom[0] in families:
            print("  FAIL %s есть на проводе, но не в логе" % hexs(rom))
            bad += 1
    if other:
        print("  на проводе ещё %d устройств другого семейства (%s) - "
              "пример их не печатает"
              % (len(other), ", ".join("%02X" % r[0] for r in sorted(other))))
    if not bad:
        extra = ", %d команд DS2431" % len(events) if events else ""
        print("  приложение и провод согласны: %d ROM, %d измерений совпали "
              "точно%s" % (len(roms), paired, extra))
    return bad


def parse_log_header(path):
    """The `# hil:` identity a HIL log carries, as key=value pairs.

    Older logs predate the header; an empty dict is the honest answer for
    them, and the report says the identity was not recorded instead of
    assuming the current tree.
    """
    facts = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if not line.startswith("# hil:"):
                continue
            for word in line[6:].split():
                key, sep, value = word.partition("=")
                if sep:
                    facts[key] = value
    return facts


def resolve_capture(path, rate_text, channel_arg, digest):
    """Pick the sample rate and channel; refuse flags that fight the artifact.

    The rate is not in the samples: it lives in the `.meta` sidecar the
    capture wrote, and decoding at another rate silently rescales every width.
    So an explicit flag that disagrees with the sidecar is an error, not an
    override. Older artifacts have no sidecar - there the flags (or the 4M/D0
    defaults) are all there is, and the report says so.

    Returns (rate_hz, channel, meta, error).
    """
    meta = None
    meta_path = path + ".meta"
    if os.path.isfile(meta_path):
        try:
            with open(meta_path, encoding="utf-8") as fh:
                meta = json.load(fh)
        except (OSError, ValueError) as exc:
            return None, None, None, "метаданные не читаются: %s" % exc
        if not isinstance(meta, dict):
            return None, None, None, "метаданные не читаются: ожидался объект"
        recorded = meta.get("sha256")
        if recorded and recorded != digest:
            return None, None, None, (
                "%s изменён после записи метаданных - это уже другой захват"
                % os.path.basename(path))

    try:
        rate_hz = float(meta["rate_hz"]) if meta and meta.get("rate_hz") else None
        channel = (int(meta["channel"])
                   if meta and meta.get("channel") is not None else None)
    except (KeyError, TypeError, ValueError) as exc:
        return None, None, None, "метаданные не читаются: %s" % exc

    if rate_text is not None:
        given = parse_rate(rate_text)
        if rate_hz is not None and abs(given - rate_hz) > 0.5:
            return None, None, None, (
                "частота %s противоречит метаданным захвата (%g МГц)"
                % (rate_text, rate_hz / 1e6))
        rate_hz = given
    if channel_arg is not None:
        if channel is not None and channel != channel_arg:
            return None, None, None, (
                "канал D%d противоречит метаданным захвата (D%d)"
                % (channel_arg, channel))
        channel = channel_arg

    return (4e6 if rate_hz is None else rate_hz,
            0 if channel is None else channel, meta, None)


def identity(path, digest, rate_hz, channel, log_path, meta):
    """Name the exact artifacts and the image the run came from.

    A capture alone is not reproducible: the samples say what the wire did,
    not which firmware drove it, on which part or at which clock. The hashes
    fix the files; the sidecar records the rate and channel they were taken
    at; the header the HIL run writes names the image and the tree, and a
    dirty tree is called out because the commit alone would then be a lie.
    """
    print("=== воспроизводимость ===")
    print("  захват: %s, %d байт" % (path, os.path.getsize(path)))
    print("  sha256: %s" % digest)
    print("  частота: %g МГц, канал D%d" % (rate_hz / 1e6, channel))
    if meta:
        print("  метаданные: %s.meta, снят %s (%g с, %s)"
              % (os.path.basename(path), meta.get("captured", "?"),
                 meta.get("seconds", 0), meta.get("tool", "?")))
    else:
        print("  метаданные: нет (.meta) - частота и канал взяты из аргументов")
    cfg = read_config()
    print("  конфигурация: ONE=%s мкс, ZERO=%s мкс, guard %d/%d мкс"
          % (cfg["one"], cfg["zero"], cfg["guard_default"],
             cfg["guard_parasite"]))
    if not log_path:
        print("  лог: не задан (--log)")
        return
    print("  лог: %s, %d байт" % (log_path, os.path.getsize(log_path)))
    print("  sha256: %s" % sha256_file(log_path))
    facts = parse_log_header(log_path)
    if not facts:
        print("  прошивка: лог не сообщает (нет шапки # hil:)")
        return
    dirty = ", дерево изменено" if facts.get("dirty") == "yes" else ""
    print("  прошивка: %s, коммит %s%s"
          % (facts.get("fw", "?"), facts.get("commit", "?"), dirty))
    print("  стенд: %s (%s), SYSCLK %s"
          % (facts.get("mcu", "?"), facts.get("chip", "?"),
             facts.get("sysclk", "?")))
    if facts.get("ext") or facts.get("uart_tx"):
        print("  сборка: EXT %s, UART TX %s"
              % (facts.get("ext", "?"), facts.get("uart_tx", "?")))
    if facts.get("sha256"):
        print("  образ: %s, sha256 %s"
              % (facts.get("hex", "?"), facts["sha256"]))


def verdict(bad, message):
    """One line that says how the run ended, whatever the detail above."""
    print("")
    if bad:
        print("ИТОГ: FAIL - %s" % message)
        return 1
    print("ИТОГ: PASS - %s" % message)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture", nargs="?", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--rate", default=None,
                    help="sample rate; default: the capture's .meta, else 4M")
    ap.add_argument("--channel", type=int, default=None,
                    help="analyzer channel; default: the capture's .meta, else D0")
    ap.add_argument("--log", default="",
                    help="the application's UART log from the same run")
    args = ap.parse_args()

    path = args.capture or args.out
    if not path:
        raise SystemExit("нужен файл захвата (позиционный аргумент)")
    if not os.path.isfile(path):
        return verdict(1, "захвата нет: %s" % path)
    if args.log and not os.path.isfile(args.log):
        return verdict(1, "лога нет: %s" % args.log)

    digest = sha256_file(path)
    rate_hz, channel, meta, problem = resolve_capture(
        path, args.rate, args.channel, digest)
    if problem:
        return verdict(1, problem)

    identity(path, digest, rate_hz, channel, args.log, meta)

    res = analyse(path, rate_hz, channel)
    if res is None:
        return verdict(1, "шина простаивала весь захват - разбирать нечего")
    widths, gaps = res

    cfg = read_config()
    try:
        transactions, orphan, dropped = decode(widths, gaps, cfg)
    except DecodeError as e:
        print("  FAIL разбор: %s" % e)
        return verdict(1, "разбор прерван")
    if not transactions:
        return verdict(1, "в захвате нет ни одной транзакции")

    report(transactions, orphan, dropped)
    bad = failures(transactions)

    print("\n=== сверка с приложением ===")
    if args.log:
        roms, readings, found, events = parse_log(args.log)
        claims = parse_log_claims(args.log)
        bad += compare_log(transactions, roms, readings, found, events, claims)
    else:
        print("  лог не задан (--log): сверено только внутреннее (CRC)")
        bad += ds2431_readback_errors(wire_events(transactions))

    if bad:
        return verdict(1, "провод не соответствует: %d ошибок" % bad)
    return verdict(0, "провод разобран, CRC и приложение согласны")


if __name__ == "__main__":
    sys.exit(main())
