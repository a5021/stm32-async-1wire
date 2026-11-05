# Examples and benches

[Back to the README](../README.md)

## Examples

Ten ready-to-run example applications are provided; select one with `APP`.
`0_dma_pressure` is a diagnostic that needs no sensor at all; the other nine
drive real 1-Wire devices.

> **Warning — three examples write non-volatile sensor memory.** On each run
> where a DS18B20 is found, `5_commands` attempts Copy Scratchpad on the
> selected sensor. A successful copy replaces its EEPROM alarm thresholds
> with TH=0x19 / TL=0x0F; the demo does not restore the previous values.
> Repeated runs consume EEPROM write endurance, even when the values already
> match. `8_ds2431` attempts to replace the first 8 bytes (address 0) on
> **every DS2431 found** with a derived pattern and does not restore the
> original contents. `9_multi` writes its peak/low records and the ROM blocks
> they refer to (addresses 16..47) on the first DS2431 it finds, but only when
> a record actually changes, and it does not restore the previous contents
> either. The same applies when running the DS2431 HIL check. Use test devices
> with no data you need to keep; do not run these demos repeatedly on deployed
> sensors.

| APP | File | What it demonstrates |
| --- | --- | --- |
| `0_dma_pressure` | `examples/0_dma_pressure/main.c` | Diagnostic: 64-slot write pattern under a selectable memory-subsystem load; reports feed starvation. |
| `1_basic` (default) | `examples/1_basic/main.c` | Single DS18B20 via Skip ROM. |
| `2_device_search` | `examples/2_device_search/main.c` | Startup search, then one Match ROM conversion per device. |
| `3_round_robin` | `examples/3_round_robin/main.c` | Startup search, then round-robin measurement of every device. |
| `4_scan_mode` | `examples/4_scan_mode/main.c` | Broadcast convert for the whole fleet, then Match ROM reads. |
| `5_commands` | `examples/5_commands/main.c` | Read ROM, power supply, thresholds, Copy/Recall EEPROM and Alarm Search. |
| `6_statistics` | `examples/6_statistics/main.c` | Sequential measurement plus the optional `ow_stats` report. |
| `7_low_power` | `examples/7_low_power/main.c` | The standard loop with the opt-in `OW_LOW_POWER=1` WFE path. |
| `8_ds2431` | `examples/8_ds2431/main.c` | Full DS2431 EEPROM write/read-back cycle. |
| `9_multi` | `examples/9_multi/main.c` | Peak/low logger: seven DS18B20 plus one DS2431 on one bus. |

### Build

```bash
make                                          # build 1_basic -> build/ds18b20_1_basic.elf
make APP=0_dma_pressure                       # build 0_dma_pressure -> build/ds18b20_0_dma_pressure.elf (diagnostic)
make APP=2_device_search                      # build 2_device_search -> build/ds18b20_2_device_search.elf
make APP=3_round_robin                        # build 3_round_robin -> build/ds18b20_3_round_robin.elf
make APP=4_scan_mode                          # build 4_scan_mode -> build/ds18b20_4_scan_mode.elf
make APP=5_commands                           # build 5_commands -> build/ds18b20_5_commands.elf
make APP=6_statistics                         # build 6_statistics -> build/ds18b20_6_statistics.elf (OW_STATS_ENABLE auto-added)
make APP=7_low_power EXT="-DOW_LOW_POWER=1"   # build 7_low_power -> build/ds18b20_7_low_power.elf (WFE low-power)
make APP=8_ds2431                             # build 8_ds2431 -> build/ds18b20_8_ds2431.elf (DS2431 EEPROM)
make APP=9_multi                              # build 9_multi -> build/ds18b20_9_multi.elf (DS18B20 + DS2431)
make debug APP=3_round_robin                  # debug build of 3_round_robin (for J-Link/ST-Link)

# STM32F030 target (same examples, bus on PA10):
make OW_MCU=STM32F030x6 APP=4_scan_mode

# STM32G031 target (bus on PA12 pad via remap, console on PA11):
make OW_MCU=STM32G031xx APP=4_scan_mode
# ... or on the raw internal HSI16 (no PLL):
make OW_MCU=STM32G031xx SYSCLK_MHZ=16 APP=4_scan_mode

# STM32F303VC target (F3-DISCOVERY / MB1035B, bus on PA10, console on PA9):
# 72MHz is the default and needs the board's 8MHz external clock, which the
# ST-Link drives onto OSC_IN and which is read in bypass mode. This family has
# no HSI16 and PLLMUL tops out at 16, so 64MHz is all the internal RC can reach.
make OW_MCU=STM32F303xC APP=4_scan_mode
make OW_MCU=STM32F303xC SYSCLK_MHZ=64 APP=4_scan_mode   # HSI/2 + PLL, no external clock
make OW_MCU=STM32F303xC SYSCLK_MHZ=8  APP=4_scan_mode   # raw HSI, nothing configured

# STM32F407 target (same examples, bus on PA10):
make OW_MCU=STM32F407xx APP=4_scan_mode

# STM32F401CC target (F401 Black Pill, 84MHz HSE+PLL default):
make OW_MCU=STM32F401xC APP=4_scan_mode
# ... or on the raw internal RC (no HSE crystal needed):
make OW_MCU=STM32F401xC SYSCLK_MHZ=16 APP=4_scan_mode

# STM32F446RE target (WeAct F446, 180MHz HSE+PLL + over-drive default):
make OW_MCU=STM32F446xx APP=4_scan_mode
# ... 16MHz on the raw internal RC, or straight off the 8MHz crystal:
make OW_MCU=STM32F446xx SYSCLK_MHZ=16 APP=4_scan_mode
make OW_MCU=STM32F446xx HSE_MHZ=8 SYSCLK_MHZ=8 APP=4_scan_mode

# STM32G474CB target (WeAct G474 Long, 170MHz HSE+PLL + Range 1 Boost default):
make OW_MCU=STM32G474xx APP=4_scan_mode
# ... 16MHz on the raw internal HSI16 (no HSE crystal needed):
make OW_MCU=STM32G474xx SYSCLK_MHZ=16 APP=4_scan_mode

# STM32L476RG target (NUCLEO-L476RG, 80MHz HSI16+PLL default; no HSE fitted):
make OW_MCU=STM32L476xx APP=4_scan_mode
# ... 16MHz on the raw internal HSI16:
make OW_MCU=STM32L476xx SYSCLK_MHZ=16 APP=4_scan_mode

# STM32H503CB target (WeAct STM32H503Cx Core Board, 64MHz raw-HSI default):
make OW_MCU=STM32H503xx APP=4_scan_mode

# STM32H743VI target (WeAct STM32H743VI Core Board, 240MHz HSE+PLL default):
make OW_MCU=STM32H743xx APP=4_scan_mode
```

### Example details

#### `0_dma_pressure`

Diagnostic only, no sensor needed. The build-time knobs are `STRESS_LEVEL=0..5`,
`STRESS_BURSTS`, `STRESS_WORDS`, `STRESS_CAPTURE`, `STRESS_INJECT_US` and
`STRESS_MARKER`; the measured matrix and the analyzer contract are in
[Measuring DMA feed margin](#measuring-dma-feed-margin-0_dma_pressure) below.

#### `1_basic`

Single sensor via Skip ROM (0xCC). With several sensors on the bus all of them
answer the read command and the data collides, so CRC failures are expected.

#### `2_device_search`

Startup Search ROM, then each discovered sensor is measured **individually**
via Match ROM (one `Convert T` per device, no broadcast conversion) in
round-robin order. A separator line is printed between full rounds. With one
sensor it behaves like `1_basic` but with ROM addressing; with N sensors a round
costs `N × conversion`. Supports `-DOW_PARASITE_POWER=1` (the strong pull-up is
handled per conversion).

#### `3_round_robin`

Measures the devices found at startup one at a time, in round-robin order. With
exactly one sensor it behaves like `1_basic`.

#### `4_scan_mode`

Converts every discovered sensor in parallel: one conversion wait covers all
devices, so N devices take `1 × conversion + N × read` instead of
`N × conversion`. Before converting, it programs the conversion resolution to
every sensor with one broadcast Write Scratchpad
(`ds18b20_set_resolution()`), because scan mode assumes a uniform resolution:
the single conversion wait must match the whole fleet. Skipping this can make a
12-bit sensor that follows a 9-bit one read back its 85.0 °C power-on-reset
value. Each reading is reported through `ds18b20_complete()` in device-table
order; `ds18b20_scan_index()` / `ds18b20_device_rom()` identify the sensor. Scan
mode is mutually exclusive with `ds18b20_select()`.

With several parasite-powered devices, a simultaneous broadcast conversion draws
all their current from the one strong pull-up. A sagging supply (for example
feeding the bus from a USB adapter) can brown out a marginal sensor (a stuck
127.9 °C reading with a valid CRC). Feed the fleet from a dedicated supply with
an adequate pull-up — see the STM32F407VGT6 section in Hardware Verified — and
the broadcast conversion stays reliable at the high PLL clock.

#### `5_commands`

Targets the first sensor found by the search (Match ROM) and runs the
non-blocking command sequence once at startup: power supply, raw scratchpad,
TH/TL write with a Copy/Recall pair to demonstrate EEPROM persistence, and the
single-device Read ROM. Persistence is real and nothing undoes it: the Copy
leaves the demo's TH/TL (0x19 / +25 °C, 0x0F / +15 °C) in the sensor's EEPROM,
not the pair it shipped with, and they load again at the next power-up. Each
command advances by one hardware operation per `*_poll()` call;
`ds18b20_last_command_ok()` verifies the result. The sequence closes with forced
alarm thresholds (TH=-55 °C, TL=+125 °C on the selected device, so it alarms
deterministically whatever the room temperature) plus an Alarm Search (0xEC)
pass that reports only alarmed devices — the only example exercising
`ds18b20_alarm_search_start()` / `ds18b20_alarm_search_poll()` /
`ds18b20_alarm_search_count()`. Other fleet members may join from thresholds
retained in their own RAM/EEPROM.

#### `6_statistics`

Extends the `3_round_robin` sequential loop with signal statistics
(`-DOW_STATS_ENABLE=1`, auto-enabled by `make APP=6_statistics`). After
`STATS_DUMP_SWEEPS` sweeps — one sweep is one pass over every device, so a batch
covers `STATS_DUMP_SWEEPS × devices` measurements and yields that many samples
per sensor — the accumulated per-sensor pulse-width min/max, 13-bucket histogram
(0–60+ µs) and error counters are streamed over UART by `ow_stats_dump_poll()`
(one line per call, non-blocking); the measurement loop is paused during the dump
and resumed afterwards via `ow_stats_reset()`. The period is
`STATS_DUMP_SWEEPS` (default 10), defined only in the example's source and
overridable with `-DSTATS_DUMP_SWEEPS=N` in `EXT`. Supports
`-DOW_PARASITE_POWER=1`.

#### `7_low_power`

The same search + sequential loop as `2_device_search`, built with
`-DOW_LOW_POWER=1`. The driver then idles the core in `__WFE()` while a long
stage runs; the mechanism and the SysTick caveat are in
[Low-power mode](#low-power-mode-7_low_power) below. Without the define the
example uses the standard polling loop.

#### `8_ds2431`

Startup search plus a full DS2431 EEPROM write cycle: Read Memory (baseline),
Write Scratchpad, Read Scratchpad (TA/ES echo and written-data read-back check),
Copy Scratchpad (t_PROG hold-off), then Read Memory again to confirm the data
landed in EEPROM. The pattern written is the row's own next value (every byte
plus one), so the read-back differs from the baseline and proves the copy instead
of re-reading bytes that were already there. Every step runs through the public
`ds2431.h` interface. The write cycle is one-shot: once every device has been
served the example prints `Demo done.` and then keeps re-running the
family-filtered search once a second, so it goes on streaming its
`Found N device(s)` census and ROMs instead of falling idle.

#### `9_multi`

A peak/low temperature logger with seven DS18B20 sensors and one DS2431 EEPROM
on the same bus. One explicit bus-owner state machine uses the two drivers
strictly in sequence, because they share TIM1/DMA and neither may be polled while
the other owns the timer: DS18B20 search, a second full-family search to locate
the DS2431, boot-read of four EEPROM blocks, then measurement rounds. Rounds run
in scan mode (one broadcast `Convert T`) and fall back to the sequential Match
ROM path if a scan fails. The round maximum and minimum are compared with the
stored peak and low; a new record is queued as Write Scratchpad → Read Scratchpad
(echo + data verify) → Copy Scratchpad  → Read Memory, with the sensor's ROM block
written before the record that refers to it, so a power cut cannot leave a record
pointing at a ROM that is not there. Records are self-validating 8-byte blocks
(magic, version, flags, counter, temperature, sensor index, CRC-8);
`PEAK_RESET=1` ignores the stored records at boot. Every stage is a `poll()` loop
paced by `app_millis()` — no delay, no wait, no interrupt.

### Common notes

- All examples use `app_init()` (from `examples/app/app.h`) to set up the system
  clock, USART1 TX and the busy LED in a single call.
- Programming targets (`make jprogram` / `make program`) flash whichever example
  is currently selected by `APP`.
- Two build knobs have no dedicated example by design: `-DOW_DRIVE_ACTIVE=1`
  switches master-only write slots to push-pull (experimental; covered by the
  `test-active` host suite), and `-DDS18B20_MAX_DEVICES=1` trims the scan table
  from 64 to 8 bytes for single-sensor builds (see CHANGELOG).

### Measuring DMA feed margin (0_dma_pressure)

The CCR3 feed asks for one DMA transfer per slot at the CC2 compare
(`ONE + ZERO` microseconds) and the next slot starts at the update event
(`ONE + ZERO + GUARD + 1`). The transfer must land in that window. The feed
channel uses the default DMA priority, so an application that moves data with
DMA1 competes with it for the same bus. All load channels in this rig stay at
that default priority too; a higher-priority application channel (`PL > 0`) is
a different case and is deliberately not covered here.

`0_dma_pressure` transmits a fixed 8-byte pattern (64 slots plus the trailing
`ONEWIRE_RELEASE_PULSE`) back to back while a build-time selectable load runs,
and reports two on-chip symptoms:

- `cndtr_late`: after `onewire_bus_done()` the feed channel still had transfers
  queued. That is complete starvation: at least one slot repeated and the final
  release may be missing.
- `dma_error`: the feed channel latched a transfer error.

A stretched pulse or a doubled edge does not move either counter; that is what
the logic analyzer sees. The HIL script bisects `ONEWIRE_GUARD_BAND` per level:
the smallest guard with a clean per-slot geometry bounds the feed latency from
above. The timer resolution is 1 µs, so guard 1 (a 2 µs window) is the
measurement floor: a clean run there means a 2 µs window was enough.

| `STRESS_LEVEL` | Load (F0/F4/H5) |
|---|---|
| 0 | none - the driver's own margin |
| 1 | CPU hammers the SRAM array in the `onewire_bus_done()` loop |
| 2 | finite mem-to-memory burst on the first load channel about every 1 ms |
| 3 | the same burst restarted as soon as it drains (the channel stays busy) |
| 4 | finite mem-to-memory burst on the second load channel, restarted as soon as it drains (circular is not allowed in mem-to-memory) |
| 5 | level 4 plus the CPU hammer |

On F0 the load channels are DMA1 channel 2 and channel 1; on F4 they are
DMA2 stream 0 and stream 1 with `DIR=0b10` (memory-to-memory); on H5 they are
GPDMA1 channel 0 and channel 1 in direct mode with a software request, where
`CBR1.BNDT` is a source-byte count and at most 32767 halfwords fit. GPDMA does
not break a tie by channel number: arbitration is by `CCR.PRIO` (PRIO 3 is a
fixed class, PRIO 0/1/2 share a weighted round-robin), so `-DSTRESS_GPDMA_PRIO=N`
selects the load class: 0 is the equal-priority case, 3 the priority case. The
other families compile the example and run level 0 with `stress=unsupported` in
the banner, so the all-family build matrix stays green.

On the F446 at its 180 MHz default every level is clean at guard 1 (a 2 µs
window), the same result as the F030. At 16 and 8 MHz levels 0-4 are also clean
at guard 1. Level 5 with the default load on DMA2 stream 0/1 fails at 16/8 MHz,
but that is the **stream-priority experiment**, not a timer limit: stream 0/1
are lower-numbered than the feed on stream 2, so at equal priority the F4
arbiter runs the load first. Moving the same 512-word burst + CPU hammer to
stream 5/6 (`--load-high`, or `STRESS_LOAD_HIGH=1`) makes the whole 0-5 matrix
clean at guard 1 at both 8 and 16 MHz (and 180 MHz is already clean). The burst
sweep on stream 0/1 (128 words with a minimal hammer clean, 160 failing) gives
the threshold when application DMA really does outrank the feed. For real code
the actionable point is to keep heavy continuous DMA off stream 0/1 — streams
5/6/7 let the feed win — or to raise the feed priority. The F446 bench also
wires PA11 to D1, so its pressure and capture matrices use the same
marker-bracketed analysis as the H503.

On the H503, re-measured on 2026-11-05 at `84f32003` from a clean tree, every
level is clean at the 5 µs default at 64, 250 and 8 MHz **at equal priority**,
and the measured floor is guard 1 in every cell; the earlier level 0/4 outliers
disappear once the D1 marker brackets the frames. The GPDMA negative control
suspends the channel (`CCR.SUSP`) and waits for the hardware acknowledgement in
`CSR.SUSPF`; taking a live channel down adds `IDLEF` and `CCR.RESET`, because
clearing `EN` alone does not drop a pending request. That corrected sequence is
what the current data measures.

Raising the load to `CCR.PRIO 3` against the feed's `PRIO 0` is the second
model, and it is a different picture: 250 MHz stays clean at guard 1, 64 MHz
needs up to 13 µs (levels 2-4), and at 8 MHz level 2 needs 100 µs while levels
3-5 never pass. The fixed high class really does outrank the feed; the
equal-priority round-robin above does not.

The Search ROM pass under the same loads matched the baseline in 33 of 36
cells. At 8 MHz levels 3 and 4 lost the bus entirely (count 0, or one device at
level 3/guard 1) on both attempts; the feed matrix is clean in those cells, so
this is capture-path margin at 8 MHz, not a feed deadline miss. The failing
searches are kept under `platforms/h503/captures/search/` and the report names
them.

The H503 bench wires PA11 to the logic analyzer's D1 (the F446 uses PA11 and the
F030 PA2). `STRESS_MARKER=1` makes
each frame (or the search pass) a D1-high window, so the HIL analyses only the
D0 traffic inside the marker: a startup glitch or a capture that ends mid-frame
is outside every frame and cannot be mistaken for one. Every cell also reads
`dma_pressure_telemetry[17..21]` (plus the capture-mode word) before trusting
the waveform: run magic, effective level/clock, injection state, start timeout
and load error. A stale image, an injection that did not actually stop the channel, or
a load channel that did not stop before being re-armed is retried once and then
reported as an identity or start-timeout failure, not as a bus failure. Cells
that only pass on a retry keep `attempts` and `first_attempt_*` in the JSON, so
a green result is not silently a flake. The same applies to a load channel that
latches a transfer error: `dma_pressure_telemetry[21]` raises `load_error`, the
cell fails, and the verdict cannot claim a load that had already stopped.

The F446 was re-measured on 2026-11-05 at `f2ff4fe7` from a clean tree, with
per-cell image hashes in `results.json` / `results-high.json`; the same run
reproduced the split above (level 5 at 16 MHz needs 89 µs, at 8 MHz never
passes; with the load on streams 5/6 every cell is clean at guard 1). The H503
was re-measured the same day at `84f32003`; both platform runs are schema 2
with a per-cell image hash.

```bash
# a build with a load and a narrow guard
make APP=0_dma_pressure OW_MCU=STM32F030x6 \
     EXT="-DOW_PARASITE_POWER=0 -DSTRESS_LEVEL=4 -DONEWIRE_GUARD_BAND=1"

# the full matrix (rebuilds per cell; needs the fx2lafw LA and an ST-Link)
make test-hil HIL_SCRIPT=dma_pressure HIL_MCU=STM32F030x6
python tests/hil/dma_pressure.py --levels 0,4,5 --guards 1,5 --clocks 48,8
```

Re-measured on 2026-11-05 at commit `12921ecd` on a clean source tree with the
PA2 marker and the run-identity checks; `docs/stress/results.json` carries a
SHA-256 of the flashed image for every cell. Measured on the F030 bench (100
frames per cell, D0 on PA10, 4 MS/s). Every cell is clean at guard 1;
`cndtr_late` is 0 everywhere:

| Level | Load evidence (48 MHz / 8 MHz) | guard 5 | guard 1 |
|---|---|---|---|
| 0 | none | clean | clean |
| 1 | 3300 / 700 CPU passes | clean | clean |
| 2 | 500 / 501 DMA bursts | clean | clean |
| 3 | 8300 / 1400 DMA bursts | clean | clean |
| 4 | 8300 / 1400 ch1 bursts | clean | clean |
| 5 | 3500 / 700 ch1 bursts + 3500 / 700 CPU passes | clean | clean |

The load is real: channel 1 restarted its finite burst 8300/1400 times at
level 4 and 3500/700 times at level 5 (the CPU hammer takes part of the
restart time), and the CPU passes are in the table. The feed stayed clean at
a 2 µs window in every cell. On this evidence the 5 µs default needs no change
on F0. The result is F030-specific; other DMA IPs (F4 streams, H5/H7 GPDMA)
need their own run.

Circular mem-to-memory is not allowed on F0 (RM0091), so level 4 is a stream
of restartable finite bursts on channel 1, not a circular channel.

`STRESS_CAPTURE=1` switches the rig to a Search ROM pass: every read bit goes
through the capture DMA (channel 4), so the same levels now stress the capture
drain as well as the feed. The unloaded level-0 search is the baseline; all 24
stressed cells (6 levels x 2 clocks x 2 guards) returned the same 7 DS18B20
ROMs and checksum, and the viewer shows all 8 responders on the bench (seven
DS18B20 plus one DS2431). `STRESS_INJECT_US=N` is the negative control: it
holds the feed (write mode) or capture (search mode) channel disabled for N
microseconds, then restores it. The write harness detects the first delayed
feed at 16 µs at 48 MHz and 20 µs at 8 MHz; the search harness detects the
first delayed capture at 200 µs - a late transfer, not only a wrong count or
route.

Two geometry notes the analyser accounts for. Guard 0 is not an operating
point: `CC2` then coincides with the update event and the release write
produces a spurious end-of-frame pulse, so the smallest meaningful guard is 1.
Slot 1 is armed by a register write, not by the feed DMA, and its pulse
includes the CPU gap between enabling OC3 and starting the timer (about 1 µs at
48 MHz, 7 µs at 8 MHz); the check skips that slot's width and the first period
of each frame.

`tests/hil/test_dma_pressure.py` replays synthetic captures for each failure
shape (stretched pulse, doubled edge, missing slot, wrong bit, parked line,
wrong guard band) and runs in `make check`, so the analyser cannot silently
lose its teeth.

The published report, with the measured tables and a representative frame per
level/clock/guard cell in the viewer, is at
<https://a5021.github.io/stm32-async-1wire/stress/>. It is generated from
`docs/stress/results.json` by `tools/stress_report.py`; `make
test-stress-report` regenerates it and decodes every linked capture.

### Low-power mode (7_low_power)

`7_low_power` runs the same search + sequential loop as `2_device_search`, but a
build with `-DOW_LOW_POWER=1` lets the driver idle the core in `__WFE()` while a
*long* stage (strictly more than 1 ms) is in progress. TIM1+DMA keep generating
the bus timing, so 1-Wire execution and result timing are unchanged — only the
CPU stops spinning.

- Stages treated as long: the temperature conversion (up to 750 ms), the
  scratchpad read (~5 ms) and an EEPROM hold-off (10 ms). Reset, commands and
  search reads stay on the normal polling path.
- The TIM1 update **interrupt source** (UIE) is enabled only to generate a
  pending event that wakes `WFE()` via `SEVONPEND`; no ISR is installed and no
  NVIC interrupt is enabled.
- Power draw is not measured yet: the example establishes the mechanism and the
  CPU-time saving.

One caveat applies to the application's millisecond counter: `app_millis()`
counts *observed* SysTick wraps, so it under-counts while the core sleeps inside
`ds18b20_poll()` for up to 750 ms. In a low-power build the figure is valid for
the pauses between cycles (where the loop spins) and deliberately excludes the
conversion time. `make fuzz-lowpower` drives the sleep loop on the host with
`-DMOCK_WFE_HOOK`; see [Building, testing and integration](building.md#testing).
