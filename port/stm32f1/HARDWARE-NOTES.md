# STM32F1 backend (STM32F103CB) — hardware notes

Bench notes for the F1 backend (the `src/ow_mcu.h` F1 section): what was
measured on real hardware, and how. The operating invariants live in the code
comments and in the "Required Timer Capabilities" section of `README.md`;
this file keeps the experiments.

Everything below was measured on the **STM32F103CB** ("Blue Pill") reference
bench: 8MHz crystal, 72MHz SYSCLK, 7 DS18B20 on one bus **in parasite
power**, console on a CP210x at 115200. The bus is shared with a second
DS2431 (the EEPROM driver uses the same bus).

## The wire geometry, measured with a logic analyzer

`tests/hil/la_capture.py` captures the 1-Wire line (PA10) with a Saleae
Logic (`sigrok-cli`, channel D0) and checks the measured shape against the
configuration in `inc/ow_config.h`:

```bash
python tests/hil/la_capture.py --parasite     # the parasite build: guard 100 us
python tests/hil/la_capture.py               # the default build: guard 5 us
```

Measured (4 MHz sample rate, the edge quantisation is a quarter of a
microsecond, so the readings are 1 us above the nominal value):

| | parasite bus | default bus | configured |
| --- | --- | --- | --- |
| bit slot | 166.0 us | 71.0 us | ONE 5 + ZERO 60 + guard 100 / 5 |
| '1' pulse (master pulls low) | 5.0 us | 5.5 us | `ONEWIRE_ONE_PULSE` = 5 |
| '0' pulse (master pulls low) | 60.0 us | 60.5 us | `ONEWIRE_ZERO_PULSE` = 60 |
| reset pulse | 481.5 us | 482.5 us | `OW_PORT_RESET_PULSE_DURATION` = 480 |
| presence pulse (slave answers) | 128.5 us | 128.5 us | device-side |

The guard band is what moves between the two columns: the parasite build
widens it to 100 us so the line is released cleanly for a device that is
drawing its power from it.

**What this is for.** The port's schedule is what a consumer's bus has to
live with, so a change to it has to be visible on the wire and not only in a
register. The capture above was taken with the same firmware before and after
the 3.0.0 work (the encapsulation of the engine, the `OW_PORT_*` → `OW_*`
rename, the driver stating a wait in microseconds instead of timer
registers): the measured pulse widths and slot periods were **identical**
before and after, and the firmware images differ. The 2026-10-22 capture, taken
for the wait check below, reproduced the same numbers. The wait decomposition
changed - the conversion wait is now a different arrangement of the same
total. That is invisible as *shape* (the line is idle for the whole wait), but
its *duration* is on the wire as the gap between the command and the next
transaction; `--wait-ms` measures it (below).

### The duration of a long wait

`python tests/hil/la_capture.py --parasite --wait-ms 750` checks the one
thing the host suite cannot: the length of a long wait.

The engine and the host model both read the repetition counter as
"periods - 1" (the reference manual's convention). A consistently wrong
reading of that convention - say, one period too many - passes every host
test, because the model agrees with the engine by construction: they share the
convention. Only the wire can tell.

With `examples/2_device_search` (12-bit conversion,
`MEASURE_PERIOD_MS` = 5 s) a capture holds two long gaps per cycle: the 750 ms
conversion wait and the 5 s application pause. The check takes the gap nearest
to the requested wait, allows ±2 ms (the tail of the last pulse plus the poll
latency), and separately requires that **no** gap sits at exactly twice the
target - that is where a plan with one period too many lands, and it is the
failure a shared convention would produce.

Measured on 2026-10-22 with the current tree (`make test-hil HIL_SCRIPT=smoke`
first, so the board runs the image under test; 7 devices, parasite power):

| | measured | expected |
| --- | --- | --- |
| conversion wait (12-bit) | 750.31 ms | 750 ms |
| application pause | 5000.48 ms | 5000 ms |

The wait is the gap between the last falling edge of `Convert T` and the first
falling edge of the read that follows. The 0.31 ms above the nominal 750 ms is
the tail of the last bit slot (at most 166 µs) plus the application's polling
and the driver's start-up for the next transaction - fixed overhead, not part
of the plan.

**What the number rules out.** One period is 62.5 ms and the tolerance is
2 ms, so a plan one period short or long (687.5 ms or 812.5 ms) cannot pass,
and a plan with one period too many would put the gap at exactly 1500 ms,
which the separate doubling check rejects. The wire therefore says the
repetition counter holds "periods - 1" on real silicon, not only in the host
model.

**A capture only means something against the image under test.** The first
capture on this bench was taken against an older image: the wire showed
resets and presence pulses and nothing else, and the firmware reported
"no sensor detected" - the image was stale, not the bus. `make test-hil`
flashes the tree, so run it (or flash the same way) before trusting a capture.

### The content on the wire

Geometry and duration say the line was driven correctly; they say nothing
about *what* was driven. `tests/hil/la_decode.py` reads the bits and rebuilds
the frames. Run against the 2026-10-22 capture of the wait run, it produced:

```
транзакция 1: reset 481.8 мкс, presence 128.8 мкс (через 29.0)
  MATCH ROM 28 20 78 92 07 00 00 67  CRC ok
  READ SCRATCHPAD: 70 01 19 0F 7F FF 10 10 BB  CRC ok. 23.0000 C
транзакция 2: ...
  MATCH ROM 28 78 B8 AC 0B 00 00 2C  CRC ok
  CONVERT (0x44)
транзакция 3: ...
  MATCH ROM 28 78 B8 AC 0B 00 00 2C  CRC ok
  READ SCRATCHPAD: 72 01 4B 46 7F FF 0E 10 57  CRC ok. 23.1250 C
```

The CRC bytes are computed by the device, not by the master: a correctly
decoded frame with a matching CRC means the ROM and the scratchpad are the
bytes the part actually sent. The two temperatures were 23.0 and 23.125 C,
and the application's log of the same image printed 23.0 and 23.1.

A second run, with the analyzer armed before `make test-hil` so the capture
starts at the flash, held the boot search as well: 9 devices enumerated (7
DS18B20 at `28`, 2 DS2431 at `2D`), every ROM CRC valid, and the example
reported the 7 sensors it filters for. Four readings paired exactly with the
application's output - 24.5625 -> 24.6, 24.6875 -> 24.7 twice, 24.75 -> 24.8.

**Why this is not the host model again.** The host model and the engine are
the same code; a shared misreading is invisible there. Here the analyzer sees
what the sensor put on the wire, the application prints what its own parser
made of it, and the two are compared. The capture also contained one round
from the previous image before the flash - the comparison starts at the last
search, which is the boot the log belongs to, so that round is not mixed in.

**The DS2431 memory cycle.** A third capture (`make test-hil HIL_SCRIPT=ds2431`,
also armed before the flash) decoded the EEPROM traffic of both `2D` parts:

```
  MATCH ROM 2D F8 47 60 26 00 00 69  CRC ok
  READ MEMORY  TA1=00 TA2=00: DE AD BE EF 01 02 03 04
  MATCH ROM 2D F8 47 60 26 00 00 69  CRC ok
  WRITE SCRATCHPAD  TA1=00 TA2=00: DE AD BE EF 01 02 03 04
  MATCH ROM 2D F8 47 60 26 00 00 69  CRC ok
  READ SCRATCHPAD  TA1=00 TA2=00: DE AD BE EF 01 02 03 04  CRC16 ok
  MATCH ROM 2D F8 47 60 26 00 00 69  CRC ok
  COPY SCRATCHPAD  TA1=00 TA2=00  ES=07
  MATCH ROM 2D F8 47 60 26 00 00 69  CRC ok
  READ MEMORY  TA1=00 TA2=00: DE AD BE EF 01 02 03 04
```

The same five commands ran on the second part, and all ten matched the
application's log byte for byte: the pattern it printed, the scratchpad frame
it verified, the `ES=07` it copied. The Read Scratchpad reply is 13 bytes -
`TA1 TA2 ES` plus 8 data plus the inverted CRC-16 the device appends - and
the analyzer recomputes that CRC over `0xAA` followed by the eleven frame
bytes, so the frame is the device's, not the master's guess.

**Caveat on the read-back.** The bench EEPROM already held `DE AD BE EF 01 02
03 04` from earlier runs, so the baseline read equals the pattern. The capture
therefore proves the command sequence, the addresses, the bytes and `ES=07`,
and that the read-back still holds the pattern - not that a blank row was
programmed by this run. A capture of a blank part (or a pattern derived from
the ROM) would close that last gap.

**What is not covered yet.** The decoder understands the ROM commands
(`0x33`, `0xCC`, `0x55`, `0xF0`, `0xEC`), the DS18B20 functions (`0x44`,
`0xBE`), and the DS2431 functions `0x0F`, `0xAA`, `0x55` (copy position) and
`0xF0` (memory read). The function table is chosen by the family byte of the
selected ROM, because `0x55` and `0xF0` mean different things on a `28` and a
`2D`; a function sent after SKIP ROM is refused as ambiguous rather than
guessed. Not decoded: the DS2431's `0x33`/`0x55` in *function* position for
other families, overdrive timing, and alarm-search results beyond the ROM.


## The bench as it stands

| | |
| --- | --- |
| sensors | 7 DS18B20 + 2 DS2431 on one bus, parasite power |
| console | CP210x, 115200, on the same board |
| debugger | ST-Link (V2), 3.25 V at the target |

The HIL harness (`make test-hil`) drives the examples on this bench:
`smoke` (7 devices, 13 readings), `lowpower` (the WFE path), `ds2431` (the
EEPROM cycle, read-back compared), `scan` and `stats`. All of them pass
here; see the "Hardware-in-the-Loop" section of `README.md` for the harness.

**Noise.** The capture showed a *run* of bytes replaced by others in the
middle of a line - not single-bit errors, which is what a marginal wire would
give - so the corruption is more likely in the capture path (the CP210x bridge
or the analyzer's USB transfer) than on the bus. The UART capture in the
harness tore a line once as well. The harness now tolerates a mangled line
instead of failing the run, but the cause is not established, and the bench
wiring (a twisted pair and a common ground for the analyzer's reference) is
the first thing to try before chasing it in the code.
