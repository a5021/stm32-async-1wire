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

Requires numpy (via la_capture) and a capture from la_capture.py.
"""

import argparse
import re
import sys

import numpy as np

from la_capture import analyse, read_config

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
FUNCTION_COMMANDS = {
    0x44: ("CONVERT", 0),
    0xBE: ("READ SCRATCHPAD", 9),
}
SEARCH_COMMANDS = {
    0xF0: "SEARCH ROM",
    0xEC: "ALARM SEARCH",
}


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
                 "passes", "empty_terminator")

    def __init__(self, command, name):
        self.command = command
        self.name = name
        self.rom = b""
        self.payload = b""
        self.crc_ok = None
        self.value = None
        self.passes = []
        self.empty_terminator = False


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


def parse_transaction(slots, cfg):
    """Frames of one transaction, starting with its ROM command."""
    frames = []
    i = 0
    first = True
    while i < len(slots):
        if len(slots) - i < BITS_PER_BYTE:
            raise DecodeError("транзакция обрывается: %d слотов на неполный байт"
                              % (len(slots) - i))
        command = 0
        for k in range(BITS_PER_BYTE):
            command |= slots[i + k].bit << k
        i += BITS_PER_BYTE

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
                need = nbytes * BITS_PER_BYTE
                if len(slots) - i < need:
                    raise DecodeError("%s: нет %d байт ROM" % (name, nbytes))
                frame.rom = bytes(
                    sum(slots[i + b * BITS_PER_BYTE + k].bit << k
                        for k in range(BITS_PER_BYTE))
                    for b in range(nbytes))
                i += need
                frame.crc_ok = crc8(frame.rom[:7]) == frame.rom[7]
            frames.append(frame)
            first = False
            continue

        if command not in FUNCTION_COMMANDS:
            raise DecodeError("неизвестная функция 0x%02X (ROM-команда "
                              "ожидается только первой)" % command)
        name, nbytes = FUNCTION_COMMANDS[command]
        frame = Frame(command, name)
        if nbytes:
            need = nbytes * BITS_PER_BYTE
            if len(slots) - i < need:
                raise DecodeError("%s: нет %d байт ответа" % (name, nbytes))
            frame.payload = bytes(
                sum(slots[i + b * BITS_PER_BYTE + k].bit << k
                    for k in range(BITS_PER_BYTE))
                for b in range(nbytes))
            i += need
            frame.crc_ok = crc8(frame.payload[:8]) == frame.payload[8]
            frame.value = temperature(frame.payload)
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
            if f.command in SEARCH_COMMANDS:
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
            for p in f.passes:
                if not p.crc_ok:
                    bad += 1
                    print("  FAIL CRC поиска: %s" % hexs(p.rom))
    return bad


def parse_log(path):
    """ROMs found and readings printed by the application, in its order."""
    roms = set()
    readings = []
    found = None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = re.search(r"Found (\d+) device", line)
            if m:
                found = int(m.group(1))
            m = re.search(r"ROM:\s+((?:[0-9A-Fa-f]{2}\s+){7}[0-9A-Fa-f]{2})", line)
            if m:
                roms.add(bytes(int(x, 16) for x in m.group(1).split()))
                continue
            m = re.search(r"((?:[0-9A-Fa-f]{2}\s+){7}[0-9A-Fa-f]{2}):\s+"
                          r"(-?\d+\.\d+)\s+C", line)
            if m:
                rom = bytes(int(x, 16) for x in m.group(1).split())
                readings.append((rom, float(m.group(2))))
    return roms, readings, found


def tenths(value):
    """The value the application prints: its temperature is in tenths."""
    scaled = value * 10.0
    return int(scaled + 0.5) if scaled >= 0 else -int(-scaled + 0.5)


def compare_log(transactions, roms, readings, found):
    """The analyzer's bytes and the application's numbers, conversion by conversion.

    Both start at the boot the log covers: the application searches once, so
    the last search in the capture is that boot, and the reads after it are the
    log's reads in the app's order. They must match ROM for ROM and round to
    the same tenth - the wire's own temperature is exact (1/16 C), so this is
    an equality, not a tolerance. Drift only appears between different
    conversions, and this pairs the same one.
    """
    bad = 0
    wire_roms = set()
    reads = []
    last_search = None
    for k, txn in enumerate(transactions):
        rom = next((f.rom for f in txn.frames if f.rom), None)
        for f in txn.frames:
            if f.command in SEARCH_COMMANDS:
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

    extra = wire_roms - roms
    other = [r for r in extra if r[0] != 0x28]
    for rom in sorted(extra):
        if rom[0] == 0x28:
            print("  FAIL %s есть на проводе, но не в логе" % hexs(rom))
            bad += 1
    if other:
        print("  на проводе ещё %d устройств другого семейства (%s) - "
              "пример их не печатает"
              % (len(other), ", ".join("%02X" % r[0] for r in sorted(other))))
    if not bad:
        print("  приложение и провод согласны: %d ROM, %d измерений совпали "
              "точно" % (len(roms), paired))
    elif paired < len(readings):
        print("  сверено %d из %d измерений лога" % (paired, len(readings)))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture", nargs="?", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--rate", default="4M")
    ap.add_argument("--channel", type=int, default=0)
    ap.add_argument("--log", default="",
                    help="the application's UART log from the same run")
    args = ap.parse_args()

    path = args.capture or args.out
    if not path:
        raise SystemExit("нужен файл захвата (позиционный аргумент)")
    rate_hz = float(args.rate.upper().replace("M", "e6").replace("K", "e3"))
    res = analyse(path, rate_hz, args.channel)
    if res is None:
        print("шина была простаивающей весь захват - разбирать нечего")
        return 1
    widths, gaps = res

    cfg = read_config()
    try:
        transactions, orphan, dropped = decode(widths, gaps, cfg)
    except DecodeError as e:
        print("  FAIL разбор: %s" % e)
        return 1
    if not transactions:
        print("  FAIL в захвате нет ни одной транзакции")
        return 1

    report(transactions, orphan, dropped)
    bad = failures(transactions)

    print("\n=== сверка с приложением ===")
    if args.log:
        roms, readings, found = parse_log(args.log)
        bad += compare_log(transactions, roms, readings, found)
    else:
        print("  лог не задан (--log): сверено только внутреннее (CRC)")

    print("")
    if bad:
        print("провод не соответствует: %d ошибок" % bad)
        return 1
    print("провод разобран, CRC и приложение согласны")
    return 0


if __name__ == "__main__":
    sys.exit(main())
