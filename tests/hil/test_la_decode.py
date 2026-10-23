#!/usr/bin/env python3
"""Host-side tests for the wire decoder - no bench needed.

Run: python tests/hil/test_la_decode.py   (make test-hil-parsing)

The decoder is the only check that reads the *bytes* on the wire, so it is
also the one that can quietly lie: a decoder that guesses a bit in the gap
between '1' and '0', or skips a command it does not understand, turns a red
capture green. The cases below feed it exchanges whose bytes are known in
advance, and the negative controls are the point: a wrong bit must fail the
CRC, a shifted frame must be an error, an unknown command must be refused.
"""

import contextlib
import io
import json
import os
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import la_capture  # noqa: E402
import la_decode  # noqa: E402

ONE = 5.0
ZERO = 60.0
READ_ZERO = 29.0
PRESENCE = 128.8
RESET = 481.8
SLOT = 166.0

ROM_A = bytes.fromhex("28 78 B8 AC 0B 00 00 2C")
ROM_B = bytes.fromhex("28 20 78 92 07 00 00 67")
SCRATCHPAD = bytes.fromhex("72 01 4B 46 7F FF 0E 10 57")  # 23.125 C, CRC 0x57

ROM_D = bytes.fromhex("2D F8 47 60 26 00 00 69")  # DS2431, ROM CRC valid
PATTERN = bytes.fromhex("DE AD BE EF 01 02 03 04")  # what older images wrote

failures = []


def check(what, condition):
    if condition:
        print("  ok   %s" % what)
    else:
        print("  FAIL %s" % what)
        failures.append(what)


def slots(data, kind="w"):
    """One 8-slot byte, LSB first. A read-0 is the device's short hold."""
    return [("slot", (b >> k) & 1, kind) for b in data for k in range(8)]


def exchange(*parts):
    return [("reset",), ("presence",)] + [p for part in parts for p in part]


def build(transactions, idle_us=1000.0):
    """Pulse widths and falling-edge gaps for a sequence of transactions."""
    widths, starts = [], []
    t = 0.0
    for tx in transactions:
        for spec in tx:
            if spec[0] == "reset":
                widths.append(RESET)
                starts.append(t)
                t += RESET + 29.0  # the presence delay
            elif spec[0] == "presence":
                widths.append(PRESENCE)
                starts.append(t)
                t += PRESENCE + 355.0  # release before the first slot
            else:
                _, bit, kind = spec
                width = ONE if bit else (ZERO if kind == "w" else READ_ZERO)
                widths.append(width)
                starts.append(t)
                t += SLOT
        t = starts[-1] + SLOT + idle_us
    gaps = np.diff(np.array(starts))
    return np.array(widths, dtype=float), gaps


def cfg():
    return la_capture.read_config()


def decode(transactions, idle_us=1000.0):
    widths, gaps = build(transactions, idle_us)
    return la_decode.decode(widths, gaps, cfg())


def search_pass(rom, collisions=()):
    """bit, complement, direction - the master writes the bit it chose."""
    out = []
    for k in range(64):
        bit = (rom[k // 8] >> (k % 8)) & 1
        if k in collisions:
            out += [("slot", 0, "r"), ("slot", 0, "r"), ("slot", bit, "w")]
        else:
            out += [("slot", bit, "r"), ("slot", 1 - bit, "r"),
                    ("slot", bit, "w")]
    return out


def scratch_frame(pattern=PATTERN, ta1=0x00, es=0x07):
    """TA1 TA2 ES + data, then the inverted CRC-16 the device appends."""
    frame = bytes([ta1, 0x00, es]) + pattern
    crc = (~la_decode.crc16(bytes([0xAA]) + frame)) & 0xFFFF
    return frame + bytes([crc & 0xFF, crc >> 8])


def ds2431_read_memory(data=PATTERN):
    return exchange(slots(b"\x55" + ROM_D),
                    slots(b"\xF0\x00\x00", "w") + slots(data, "r"))


def ds2431_write(pattern=PATTERN):
    return exchange(slots(b"\x55" + ROM_D),
                    slots(b"\x0F\x00\x00", "w") + slots(pattern, "w"))


def ds2431_frame(frame=None):
    return exchange(slots(b"\x55" + ROM_D),
                    slots(b"\xAA", "w") + slots(frame or scratch_frame(), "r"))


def ds2431_copy(es=0x07):
    return exchange(slots(b"\x55" + ROM_D),
                    slots(b"\x55\x00\x00" + bytes([es]), "w"))


def test_ds2431_read_memory():
    print("a DS2431 Read Memory window")
    txns, _, _ = decode([ds2431_read_memory()])
    f = txns[0].frames[1]
    check("the window is rebuilt", f.data == PATTERN)
    check("the address is TA1=00 TA2=00", f.addr == b"\x00\x00")
    check("a memory read has no CRC of its own", f.crc_ok is None)


def test_ds2431_write_and_copy():
    print("Write Scratchpad, then Copy Scratchpad")
    txns, _, _ = decode([ds2431_write(), ds2431_copy()])
    w = txns[0].frames[1]
    check("the write carries the pattern", w.data == PATTERN)
    check("the write has no CRC", w.crc_ok is None)
    c = txns[1].frames[1]
    check("the copy carries TA1 TA2 ES", c.payload == b"\x00\x00\x07")
    check("ES=0x07 is accepted", c.problem is None)


def test_ds2431_scratchpad_crc16():
    print("Read Scratchpad with the device's inverted CRC-16")
    txns, _, _ = decode([ds2431_frame()])
    f = txns[0].frames[1]
    check("13 bytes on the wire", len(f.payload) == 13)
    check("the CRC-16 matches", f.crc_ok is True)
    check("the 8 data bytes are the pattern", f.data == PATTERN)


def test_ds2431_bad_crc16_fails():
    print("a flipped bit in the scratchpad frame")
    broken = bytearray(scratch_frame())
    broken[5] ^= 0x01
    txns, _, _ = decode([ds2431_frame(bytes(broken))])
    f = txns[0].frames[1]
    check("the bit was decoded as sent", f.data != PATTERN)
    check("the CRC-16 is false", f.crc_ok is False)
    check("failures() counts it", la_decode.failures(txns) == 1)


def test_ds2431_wrong_es_fails():
    print("Copy Scratchpad with an ending offset that is not a full row")
    txns, _, _ = decode([ds2431_copy(0x06)])
    check("the violation is noted", txns[0].frames[1].problem is not None)
    check("failures() counts it", la_decode.failures(txns) == 1)


def test_ds2431_read_memory_needs_whole_bytes():
    print("a Read Memory window that is not whole bytes")
    widths, gaps = build([ds2431_read_memory()])
    widths = np.delete(widths, len(widths) - 1)  # one slot short
    try:
        la_decode.decode(widths, gaps, cfg())
        check("the decoder refuses it", False)
    except la_decode.DecodeError as e:
        check("the decoder refuses it (%s)" % e, True)


def test_ds2431_skip_rom_is_ambiguous():
    print("SKIP ROM followed by a function command")
    try:
        decode([exchange(slots(b"\xCC"), slots(b"\xF0\x00\x00", "w"))])
        check("the decoder refuses it", False)
    except la_decode.DecodeError as e:
        check("the decoder refuses it (%s)" % e, True)


def test_ds2431_rejects_a_ds18b20_function():
    print("CONVERT addressed to a DS2431")
    try:
        decode([exchange(slots(b"\x55" + ROM_D), slots(b"\x44"))])
        check("the decoder refuses it", False)
    except la_decode.DecodeError as e:
        check("the decoder refuses it (%s)" % e, True)


def ds2431_log_text():
    # The harness prefixes every line with a timestamp; the decoder must not
    # care (a "^frame:" anchor would have silently dropped the frame).
    lines = [
        "DS2431 8_ds2431 starting...",
        "Searching 1-Wire bus...",
        "Found 1 device(s).",
        "Selected ROM: " + la_decode.hexs(ROM_D),
        "Read Memory (0xF0) - baseline:",
        "  EEPROM: " + la_decode.hexs(PATTERN),
        "Write Scratchpad (0x0F): " + la_decode.hexs(PATTERN),
        "  scratchpad written",
        "Read Scratchpad (0xAA):",
        "  frame: " + la_decode.hexs(scratch_frame()[:11]),
        "Copy Scratchpad (0x55) -> EEPROM (t_PROG hold-off):",
        "  copied to EEPROM",
        "Read Memory (0xF0) - after copy:",
        "  EEPROM: " + la_decode.hexs(PATTERN),
        "  read-back matches the write",
        "",
    ]
    return "\n".join("[477964.34] " + line if line else "" for line in lines)


def with_log(text):
    fd, path = tempfile.mkstemp(suffix=".log")
    os.close(fd)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)
    return path


def run_main(argv):
    """Run la_decode.main() on argv, capturing its exit code and report."""
    old = sys.argv
    sys.argv = ["la_decode.py"] + argv
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            code = la_decode.main()
    finally:
        sys.argv = old
    return code, buf.getvalue()


def test_main_refuses_what_it_cannot_decode():
    print("la_decode's exit paths")
    with tempfile.TemporaryDirectory() as d:
        missing = os.path.join(d, "nope.bin")
        code, out = run_main([missing])
        check("a missing capture exits 1", code == 1)
        check("and says why", "ИТОГ: FAIL" in out and "захвата нет" in out)

        idle = os.path.join(d, "idle.bin")
        with open(idle, "wb") as fh:
            fh.write(bytes(64))  # every sample low: no falling edge at all
        code, out = run_main([idle])
        check("an idle bus exits 1", code == 1)
        check("and says the bus was idle",
              "ИТОГ: FAIL" in out and "простаивала" in out)

        code, out = run_main([idle, "--log", os.path.join(d, "nope.log")])
        check("a missing --log exits 1", code == 1)
        check("and names the log",
              "ИТОГ: FAIL" in out and "лога нет" in out)


def test_ds2431_log_matches_bytes():
    print("the DS2431 cycle against the application's log")
    path = with_log(ds2431_log_text())
    try:
        roms, readings, found, events = la_decode.parse_log(path)
        check("the selected ROM is the log's device", roms == {ROM_D})
        check("the cycle has 5 printed events", len(events) == 5)
        txns, _, _ = decode([
            ds2431_read_memory(), ds2431_write(), ds2431_frame(),
            ds2431_copy(), ds2431_read_memory(),
        ])
        check("the wire matches the log byte for byte",
              la_decode.compare_log(txns, roms, readings, found, events) == 0)

        broken = ds2431_log_text().replace(
            la_decode.hexs(PATTERN), "DE AD BE EF 01 02 03 05")
        path2 = with_log(broken)
        try:
            _, _, _, bad_events = la_decode.parse_log(path2)
            check("a wrong EEPROM byte fails",
                  la_decode.compare_log(txns, roms, [], found, bad_events) > 0)
        finally:
            os.unlink(path2)
    finally:
        os.unlink(path)


def test_a_hil_header_is_read_back():
    print("the # hil: identity a HIL log carries")
    text = (
        "# ds2431\n"
        "# hil: script=ds2431 app=8_ds2431 mcu=STM32F103xB chip=f103xb "
        "sysclk=72MHz\n"
        "# hil: fw=2.4.0 commit=ac512b3 dirty=no\n"
        "[1.00] DS2431 8_ds2431 starting...\n"
    )
    path = with_log(text)
    try:
        facts = la_decode.parse_log_header(path)
        check("the image is named",
              facts.get("fw") == "2.4.0" and facts.get("commit") == "ac512b3")
        check("the bench and the clock are named",
              facts.get("mcu") == "STM32F103xB"
              and facts.get("sysclk") == "72MHz")
        check("the header does not confuse the traffic parser",
              la_decode.parse_log(path)[0] == set())
    finally:
        os.unlink(path)

    old = with_log("# smoke\n[1.00] nothing here\n")
    try:
        check("an older log without the header reads as empty",
              la_decode.parse_log_header(old) == {})
    finally:
        os.unlink(old)


def test_a_capture_with_no_sidecar_falls_back_to_the_flags():
    print("an artifact that predates the .meta sidecar")
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "cap.bin")
        with open(path, "wb") as fh:
            fh.write(b"\xff" * 16)
        digest = la_decode.sha256_file(path)
        rate, channel, meta, problem = la_decode.resolve_capture(
            path, None, None, digest)
        check("the defaults are 4 MHz on D0",
              (rate, channel, meta, problem) == (4e6, 0, None, None))
        rate, channel, _, problem = la_decode.resolve_capture(
            path, "8M", 1, digest)
        check("the flags are used when nothing contradicts them",
              rate == 8e6 and channel == 1 and problem is None)


def test_a_sidecar_is_the_artifacts_own_statement():
    print("a capture that carries its .meta")
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "cap.bin")
        with open(path, "wb") as fh:
            fh.write(b"\xff" * 16)
        digest = la_decode.sha256_file(path)
        with open(path + ".meta", "w", encoding="utf-8") as fh:
            json.dump({"rate_hz": 8e6, "channel": 1, "sha256": digest}, fh)

        rate, channel, meta, problem = la_decode.resolve_capture(
            path, None, None, digest)
        check("the recorded rate and channel win",
              rate == 8e6 and channel == 1 and problem is None)
        check("a matching flag is accepted",
              la_decode.resolve_capture(path, "8M", 1, digest)[3] is None)
        check("a contradicting rate is refused",
              "метаданн" in (la_decode.resolve_capture(
                  path, "4M", 1, digest)[3] or ""))
        check("a contradicting channel is refused",
              "метаданн" in (la_decode.resolve_capture(
                  path, None, 0, digest)[3] or ""))
        check("a replaced artifact is refused",
              "изменён" in (la_decode.resolve_capture(
                  path, None, None, "0" * 64)[3] or ""))


def test_ds2431_readback_confirms_the_write():
    print("the cycle's read-back: write, copy, read the row")
    txns, _, _ = decode([
        ds2431_read_memory(), ds2431_write(), ds2431_frame(),
        ds2431_copy(), ds2431_read_memory(),
    ])
    events = la_decode.wire_events(txns)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        bad = la_decode.ds2431_readback_errors(events)
    check("the written row is read back unchanged", bad == 0)
    check("a row that already held the bytes is a note, not a proof",
          "примечание" in buf.getvalue())


def test_ds2431_a_changed_row_proves_the_copy():
    print("a cycle whose baseline differs from the write")
    old = bytes.fromhex("DE AD BE EF 01 02 03 05")
    txns, _, _ = decode([
        ds2431_read_memory(old), ds2431_write(), ds2431_frame(),
        ds2431_copy(), ds2431_read_memory(),
    ])
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        bad = la_decode.ds2431_readback_errors(la_decode.wire_events(txns))
    report = buf.getvalue()
    check("the read-back matches the write", bad == 0)
    check("the report says the row changed and the copy is proven",
          "копирование доказано" in report)
    check("the equal-baseline note is not repeated",
          "примечание" not in report)


def test_ds2431_no_baseline_is_not_a_proof():
    print("a cycle whose baseline read is not in the capture")
    txns, _, _ = decode([
        ds2431_write(), ds2431_frame(), ds2431_copy(), ds2431_read_memory(),
    ])
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        bad = la_decode.ds2431_readback_errors(la_decode.wire_events(txns))
    report = buf.getvalue()
    check("the read-back matches the write", bad == 0)
    check("but a change cannot be claimed without the baseline",
          "копирование доказано" not in report)


def test_ds2431_readback_is_not_the_log_agreeing_with_itself():
    print("a wire and a log that agree on a write that did not take")
    # The old failure mode: the log prints what it wrote and the EEPROM it
    # read, both captured; the wire carries the same two strings. The byte
    # comparison is green while the device still holds the old byte.
    stale = bytes.fromhex("DE AD BE EF 01 02 03 05")
    txns, _, _ = decode([
        ds2431_read_memory(stale), ds2431_write(), ds2431_frame(),
        ds2431_copy(), ds2431_read_memory(stale),
    ])
    wire = la_decode.wire_events(txns)
    log = [(ROM_D, "read", stale), (ROM_D, "write", PATTERN),
           (ROM_D, "frame", scratch_frame()[:11]),
           (ROM_D, "copy", b""), (ROM_D, "read", stale)]
    check("the wire matches the log byte for byte",
          la_decode.compare_events(wire, log) == 0)
    check("the read-back check still fails it",
          la_decode.ds2431_readback_errors(log) > 0)
    check("compare_log fails it",
          la_decode.compare_log(txns, {ROM_D}, [], 1, log) > 0)


def test_ds2431_the_logs_verdict_is_checked_against_the_wire():
    print("the example's own read-back verdict against the wire")
    stale = bytes.fromhex("DE AD BE EF 01 02 03 05")
    honest = ds2431_log_text()
    path = with_log(honest)
    try:
        claims = la_decode.parse_log_claims(path)
        check("the verdict is read as a claim of a match",
              claims == {ROM_D: True})
        txns, _, _ = decode([
            ds2431_read_memory(), ds2431_write(), ds2431_frame(),
            ds2431_copy(), ds2431_read_memory(),
        ])
        roms, readings, found, events = la_decode.parse_log(path)
        check("an honest verdict agrees with the wire",
              la_decode.compare_log(txns, roms, readings, found, events,
                                    claims) == 0)
    finally:
        os.unlink(path)

    # The application's comparison is broken: the old row came back and it
    # still printed "matches". The wire and the log agree byte for byte, so
    # only holding the claim against the wire catches it.
    lying = honest.replace("  EEPROM: " + la_decode.hexs(PATTERN),
                           "  EEPROM: " + la_decode.hexs(stale))
    path = with_log(lying)
    try:
        txns, _, _ = decode([
            ds2431_read_memory(stale), ds2431_write(), ds2431_frame(),
            ds2431_copy(), ds2431_read_memory(stale),
        ])
        roms, readings, found, events = la_decode.parse_log(path)
        claims = la_decode.parse_log_claims(path)
        wire = la_decode.wire_events(txns)
        check("the bytes still agree",
              la_decode.compare_events(wire, events) == 0)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            bad = la_decode.ds2431_readback_errors(wire, claims)
        check("the contradictory verdict fails both the write and the claim",
              bad == 2)
        check("the report names the contradiction",
              "лог утверждает" in buf.getvalue())
    finally:
        os.unlink(path)

    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        bad = la_decode.ds2431_readback_errors([], {ROM_D: True})
    check("a verdict for a cycle outside the capture is only a note", bad == 0)
    check("and it is printed as one", "примечание" in buf.getvalue())


def test_ds2431_copy_without_readback_fails():
    print("a copy with no read afterwards")
    txns, _, _ = decode([
        ds2431_read_memory(), ds2431_write(), ds2431_frame(), ds2431_copy(),
    ])
    check("an unconfirmed write is an error",
          la_decode.ds2431_readback_errors(la_decode.wire_events(txns)) == 1)


def test_ds2431_frame_must_carry_the_written_bytes():
    print("a scratchpad frame that returns something else")
    other = bytes.fromhex("DE AD BE EF 01 02 03 05")
    txns, _, _ = decode([
        ds2431_write(), ds2431_frame(scratch_frame(other)),
        ds2431_copy(), ds2431_read_memory(),
    ])
    check("the frame is a valid CRC-16 but the wrong content",
          txns[1].frames[1].crc_ok is True)
    check("the mismatch is an error",
          la_decode.ds2431_readback_errors(la_decode.wire_events(txns)) == 1)


def test_a_missing_device_is_an_error():
    print("a reset that no device answers")
    widths, gaps = build([[("reset",), ("slot", 0, "w")]])
    try:
        la_decode.decode(widths, gaps, cfg())
        check("a slot where a presence belongs is refused", False)
    except la_decode.DecodeError as e:
        check("a slot where a presence belongs is refused (%s)" % e, True)

    try:
        la_decode.decode(np.array([RESET]), np.array([]), cfg())
        check("a capture that ends on the reset is refused", False)
    except la_decode.DecodeError as e:
        check("a capture that ends on the reset is refused (%s)" % e, True)


def test_a_silent_device_fails_the_crc():
    print("a device that drives nothing: the master reads all ones")
    txns, _, _ = decode([
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"),
                 slots(b"\xFF" * 9, "r")),
    ])
    read = txns[0].frames[1]
    check("the bytes are 0xFF", read.payload == b"\xFF" * 9)
    check("the CRC-8 rejects them", read.crc_ok is False)
    check("failures() counts it", la_decode.failures(txns) == 1)


def test_a_bad_rom_crc_is_counted():
    print("a MATCH ROM whose ROM byte was damaged")
    rom = bytearray(ROM_A)
    rom[3] ^= 0x01
    txns, _, _ = decode([
        exchange(slots(b"\x55" + bytes(rom)), slots(b"\x44")),
    ])
    check("the ROM CRC is false", txns[0].frames[0].crc_ok is False)
    check("failures() counts it", la_decode.failures(txns) == 1)


def test_a_presence_inside_a_transaction_is_an_error():
    print("a device answering in the middle of a frame")
    widths, gaps = build([
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"), slots(SCRATCHPAD, "r")),
    ])
    widths[10] = 130.0  # between 2 x ZERO and a reset: a presence, not a slot
    try:
        la_decode.decode(widths, gaps, cfg())
        check("the interruption is refused", False)
    except la_decode.DecodeError as e:
        check("the interruption is refused (%s)" % e, True)


def test_a_truncated_capture_is_an_error():
    print("a capture that stops one slot before the end of a frame")
    widths, gaps = build([
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"), slots(SCRATCHPAD, "r")),
    ])
    try:
        la_decode.decode(widths[:-1], gaps[:-1], cfg())
        check("a short frame is refused, not zero-filled", False)
    except la_decode.DecodeError as e:
        check("a short frame is refused, not zero-filled (%s)" % e, True)


def test_ds2431_read_memory_is_not_a_search():
    print("a Read Memory (0xF0) after the boot search")
    # The DS2431 cycle runs after the boot search; if its 0xF0 were taken for
    # a search, the log's reading (before it) would no longer be paired.
    txns, _, _ = decode([
        exchange(slots(b"\xF0"), search_pass(ROM_A)),
        exchange(slots(b"\x55" + ROM_A), slots(b"\x44")),
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"),
                 slots(SCRATCHPAD, "r")),
        ds2431_read_memory(),
    ])
    check("the reading is still paired with the log",
          la_decode.compare_log(txns, {ROM_A}, [(ROM_A, 23.125)], 1,
                                [(ROM_D, "read", PATTERN)]) == 0)


def test_a_full_exchange_decodes():
    print("a MATCH ROM + CONVERT, then a MATCH ROM + READ SCRATCHPAD")
    txns, orphan, dropped = decode([
        exchange(slots(b"\x55" + ROM_A), slots(b"\x44")),
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"), slots(SCRATCHPAD, "r")),
    ])
    check("two transactions", len(txns) == 2)
    check("no orphan slots, no artifacts", orphan == 0 and dropped == 0)
    match, convert = txns[0].frames
    check("the ROM is rebuilt exactly", match.rom == ROM_A)
    check("the ROM CRC is checked", match.crc_ok is True)
    check("CONVERT has no payload", convert.command == 0x44)
    read = txns[1].frames[1]
    check("the scratchpad is rebuilt", read.payload == SCRATCHPAD)
    check("the scratchpad CRC is checked", read.crc_ok is True)
    check("the temperature is 23.125 C", abs(read.value - 23.125) < 1e-9)


def test_a_wrong_bit_fails_the_crc():
    print("a flipped bit in the scratchpad")
    broken = bytearray(SCRATCHPAD)
    broken[0] ^= 0x01
    txns, _, _ = decode([
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"), slots(broken, "r")),
    ])
    read = txns[0].frames[1]
    check("the bit was decoded as sent", read.payload[0] != SCRATCHPAD[0])
    check("the CRC says the frame is broken", read.crc_ok is False)
    check("failures() counts it", la_decode.failures(txns) == 1)


def test_a_shifted_frame_is_an_error():
    print("a frame shifted by one slot")
    widths, gaps = build([exchange(slots(b"\x55" + ROM_A), slots(b"\x44"))])
    # A leading slot before the reset does not shift the frame; shift the
    # frame itself by inserting a slot right after the presence pulse.
    widths = np.insert(widths, 2, ONE)
    gaps = np.insert(gaps, 2, SLOT)
    try:
        la_decode.decode(widths, gaps, cfg())
        check("the decoder refuses it", False)
    except la_decode.DecodeError as e:
        check("the decoder refuses it (%s)" % e, True)


def test_a_short_presence_is_an_error():
    print("a presence pulse too short")
    widths, gaps = build([exchange(slots(b"\xCC"), slots(b"\x44"))])
    widths[1] = 40.0
    try:
        la_decode.decode(widths, gaps, cfg())
        check("the decoder refuses it", False)
    except la_decode.DecodeError as e:
        check("the decoder refuses it (%s)" % e, True)


def test_an_unknown_command_is_an_error():
    print("a function command the decoder does not know")
    try:
        decode([exchange(slots(b"\x55" + ROM_A), slots(b"\x99"))])
        check("the decoder refuses it", False)
    except la_decode.DecodeError as e:
        check("the decoder refuses it (%s)" % e, True)


def test_a_search_pass_rebuilds_the_rom():
    print("a Search ROM pass")
    txns, _, _ = decode([exchange(slots(b"\xF0"), search_pass(ROM_A))])
    search = txns[0].frames[0]
    check("one pass", len(search.passes) == 1)
    check("the ROM is the written directions", search.passes[0].rom == ROM_A)
    check("its CRC is checked", search.passes[0].crc_ok is True)
    check("no collisions in a one-device pass", search.passes[0].collisions == 0)


def test_two_passes_with_a_collision():
    print("two passes, one collision")
    txns, _, _ = decode([
        exchange(slots(b"\xF0"), search_pass(ROM_A), search_pass(ROM_B, {0})),
    ])
    search = txns[0].frames[0]
    check("two passes", len(search.passes) == 2)
    check("first ROM", search.passes[0].rom == ROM_A)
    check("second ROM", search.passes[1].rom == ROM_B)
    check("the collision is reported", search.passes[1].collisions == 1)


def test_an_empty_pass_terminates_the_search():
    print("a pass in which no device answers")
    empty = [("slot", 1, "r"), ("slot", 1, "r")]
    txns, _, _ = decode([
        exchange(slots(b"\xF0"), search_pass(ROM_A), empty),
    ])
    search = txns[0].frames[0]
    check("only the real pass is recorded", len(search.passes) == 1)
    check("the empty terminator is marked", search.empty_terminator is True)


def test_a_direction_contradicting_the_pair_is_an_error():
    print("all devices say 0, the master writes 1")
    bad = search_pass(ROM_A)
    bad[2] = ("slot", 1, "w")  # ROM_A bit 0 is 0, so the pair is (0, 1)
    try:
        decode([exchange(slots(b"\xF0"), bad)])
        check("the decoder refuses it", False)
    except la_decode.DecodeError as e:
        check("the decoder refuses it (%s)" % e, True)


def test_a_capture_that_starts_mid_transaction():
    print("a capture that starts in the middle of an exchange")
    widths, gaps = build([
        exchange(slots(b"\x55" + ROM_A), slots(b"\x44")),
    ])
    # Drop the reset and presence: the leading slots are a partial exchange.
    widths, gaps = widths[2:], gaps[2:]
    txns, orphan, _ = la_decode.decode(widths, gaps, cfg())
    check("nothing is decoded from the partial exchange", len(txns) == 0)
    check("the partial slots are counted, not dropped", orphan == len(widths))


def test_an_idle_capture_has_nothing():
    print("an idle capture")
    txns, orphan, dropped = la_decode.decode(
        np.array([5.0, 60.0]), np.array([166.0]), cfg())
    check("no transactions", txns == [])
    check("the pulses are reported as orphans", orphan == 2)


def test_the_log_must_agree():
    print("the application's log")
    txns, _, _ = decode([
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"), slots(SCRATCHPAD, "r")),
    ])
    roms = {ROM_A}
    good = [(ROM_A, 23.1)]
    check("a matching log passes",
          la_decode.compare_log(txns, roms, good, 1) == 0)
    check("a wrong temperature fails",
          la_decode.compare_log(txns, roms, [(ROM_A, 22.0)], 1) > 0)
    check("a ROM missing from the wire's log fails",
          la_decode.compare_log(txns, set(), good, 1) > 0)
    check("a log that enumerates a different device fails",
          la_decode.compare_log(txns, roms, [(ROM_B, 23.1)], 1) > 0)
    check("a wrong census fails",
          la_decode.compare_log(txns, roms, good, 2) > 0)
    # The wire's 23.125 C rounds to the 23.1 C the application prints; the
    # comparison is an equality of the printed value, not a tolerance.
    check("23.125 C matches the printed 23.1",
          la_decode.tenths(23.125) == la_decode.tenths(23.1))


def test_the_reads_after_the_boot_are_paired():
    print("a capture that caught the flash")
    txns, _, _ = decode([
        # the previous image's round, then the boot of the logged one
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"),
                 slots(bytes.fromhex("88 01 19 0F 7F FF 10 10 BB"), "r")),
        exchange(slots(b"\xF0"), search_pass(ROM_A)),
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"),
                 slots(SCRATCHPAD, "r")),
    ])
    check("only the post-boot read is compared",
          la_decode.compare_log(txns, {ROM_A}, [(ROM_A, 23.1)], 1) == 0)


def test_the_search_set_is_the_log_set():
    print("a full search against the log")
    txns, _, _ = decode([
        exchange(slots(b"\xF0"), search_pass(ROM_A), search_pass(ROM_B)),
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"),
                 slots(SCRATCHPAD, "r")),
        exchange(slots(b"\x55" + ROM_B), slots(b"\xBE"),
                 slots(SCRATCHPAD, "r")),
    ])
    roms = {ROM_A, ROM_B}
    readings = [(ROM_A, 23.1), (ROM_B, 23.1)]
    check("both ROMs agree with the log",
          la_decode.compare_log(txns, roms, readings, 2) == 0)
    check("a missing device in the log fails",
          la_decode.compare_log(txns, {ROM_A}, readings, 1) > 0)
    check("a wrong count fails",
          la_decode.compare_log(txns, roms, readings, 3) > 0)


def test_another_family_on_the_wire_is_fine():
    print("a DS2431 next to the sensors")
    ds2431 = bytes.fromhex("2D F8 47 60 26 00 00 69")
    txns, _, _ = decode([
        exchange(slots(b"\xF0"), search_pass(ROM_A), search_pass(ds2431)),
        exchange(slots(b"\x55" + ROM_A), slots(b"\xBE"),
                 slots(SCRATCHPAD, "r")),
    ])
    # The example reports DS18B20s only; the 0x2D device is on the wire but
    # absent from the log by design, and that is not a failure.
    check("the other family does not fail the comparison",
          la_decode.compare_log(txns, {ROM_A}, [(ROM_A, 23.125)], 1) == 0)
    # And the other way round: a DS2431 log ignores the DS18B20s.
    txns, _, _ = decode([
        exchange(slots(b"\xF0"), search_pass(ROM_D), search_pass(ROM_A)),
    ])
    check("a 0x28 device is not a DS2431 log's business",
          la_decode.compare_log(txns, {ROM_D}, [], 1, []) == 0)


def main():
    test_a_full_exchange_decodes()
    test_a_wrong_bit_fails_the_crc()
    test_a_shifted_frame_is_an_error()
    test_a_short_presence_is_an_error()
    test_an_unknown_command_is_an_error()
    test_a_search_pass_rebuilds_the_rom()
    test_two_passes_with_a_collision()
    test_an_empty_pass_terminates_the_search()
    test_a_direction_contradicting_the_pair_is_an_error()
    test_a_capture_that_starts_mid_transaction()
    test_an_idle_capture_has_nothing()
    test_the_log_must_agree()
    test_the_reads_after_the_boot_are_paired()
    test_the_search_set_is_the_log_set()
    test_another_family_on_the_wire_is_fine()
    test_ds2431_read_memory()
    test_ds2431_write_and_copy()
    test_ds2431_scratchpad_crc16()
    test_ds2431_bad_crc16_fails()
    test_ds2431_wrong_es_fails()
    test_ds2431_read_memory_needs_whole_bytes()
    test_ds2431_skip_rom_is_ambiguous()
    test_ds2431_rejects_a_ds18b20_function()
    test_ds2431_log_matches_bytes()
    test_a_hil_header_is_read_back()
    test_a_capture_with_no_sidecar_falls_back_to_the_flags()
    test_a_sidecar_is_the_artifacts_own_statement()
    test_main_refuses_what_it_cannot_decode()
    test_ds2431_readback_confirms_the_write()
    test_ds2431_a_changed_row_proves_the_copy()
    test_ds2431_no_baseline_is_not_a_proof()
    test_ds2431_readback_is_not_the_log_agreeing_with_itself()
    test_ds2431_the_logs_verdict_is_checked_against_the_wire()
    test_ds2431_copy_without_readback_fails()
    test_ds2431_frame_must_carry_the_written_bytes()
    test_a_missing_device_is_an_error()
    test_a_silent_device_fails_the_crc()
    test_a_bad_rom_crc_is_counted()
    test_a_presence_inside_a_transaction_is_an_error()
    test_a_truncated_capture_is_an_error()
    test_ds2431_read_memory_is_not_a_search()
    print("")
    if failures:
        print("test_la_decode: %d FAIL" % len(failures))
        return 1
    print("test_la_decode: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
