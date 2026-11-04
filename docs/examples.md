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

| APP               | File                                  | Behaviour                                                        |
|-------------------|---------------------------------------|------------------------------------------------------------------|
| `0_dma_pressure`  | `examples/0_dma_pressure/main.c`      | Diagnostic, no sensor needed: transmits a fixed 8-byte pattern (64 slots, then the hardware release) back to back while a build-time selectable memory-subsystem load runs. `STRESS_LEVEL=0..5`, `STRESS_BURSTS=100` and `STRESS_WORDS=512` select the load. The console reports `cndtr_late` (the feed channel still had transfers queued at completion: complete starvation) and `dma_error`; the same counters are mirrored in `dma_pressure_telemetry` for a debugger. Levels 1-5 drive DMA1 channels 1/2 on F0, DMA2 streams 0/1 on F4 and GPDMA1 channels 0/1 on H5; the other families compile and run level 0. `STRESS_CAPTURE=1` switches to a Search ROM capture-drain pass; `STRESS_INJECT_US=N` is the deliberately-delayed-transfer negative control, and `STRESS_MARKER=1` puts a frame marker on PA11 (LA D1). Used by `tests/hil/dma_pressure.py` and `tests/hil/dma_capture.py`. |
| (default)         | `examples/1_basic/main.c`             | Unconditional polling of a single DS18B20 via Skip ROM (0xCC).   |
| `2_device_search` | `examples/2_device_search/main.c`     | Startup device search + per-device polling: each sensor is converted and read back individually via Match ROM (one `Convert T` per device, no broadcast conversion). |
| `3_round_robin`   | `examples/3_round_robin/main.c`       | Startup device search + sequential polling of every sensor found (up to `DS18B20_MAX_DEVICES`). |
| `4_scan_mode`     | `examples/4_scan_mode/main.c`         | Startup device search + simultaneous broadcast conversion: one `Convert T` (Skip ROM) converts all sensors in parallel, then each is read back via Match ROM. |
| `5_commands`      | `examples/5_commands/main.c`          | Startup device search + non-blocking command transactions on the first sensor: Read Power Supply (0xB4), raw Read Scratchpad (0xBE), Write Scratchpad TH/TL (0x4E), Copy Scratchpad (0x48) to the EEPROM, Recall EEPROM (0xB8), single-device Read ROM (0x33), forced alarm thresholds plus Alarm Search (0xEC) showing that only alarmed devices respond, then steady-state measurement of the selected device. |
| `6_statistics`    | `examples/6_statistics/main.c`        | Startup device search + sequential measurement with signal statistics. The `6_statistics` target auto-enables `-DOW_STATS_ENABLE=1`. Accumulates per-sensor pulse-width min/max, a global histogram and error counters over N sweeps - one sweep being one pass over every device - then streams the full report over UART as a non-blocking dump. The period is `STATS_DUMP_SWEEPS`, default 10, defined only in the example's source and overridable with `-DSTATS_DUMP_SWEEPS=N` in `EXT`. |
| `7_low_power`     | `examples/7_low_power/main.c`         | Low-power example (same search + sequential loop as `2_device_search`): with `-DOW_LOW_POWER=1` the **driver** enters `__WFE()` inside `ds18b20_poll()` while a long 1-Wire stage (> 1 ms: temperature conversion, scratchpad read, EEPROM hold-off) is running, so the CPU idles instead of spinning. The application's code and its result timing are unchanged — it never waits for a result — and the interval between cycles is a plain `app_millis()` deadline. Without the define, the example uses the standard polling loop. |
| `8_ds2431`        | `examples/8_ds2431/main.c`            | Startup device search + a full DS2431 EEPROM write cycle: Read Memory (baseline), Write Scratchpad, Read Scratchpad (TA/ES echo and written-data read-back check), Copy Scratchpad (t_PROG hold-off), then Read Memory again to confirm the data landed in EEPROM. The pattern written is the row's own next value (every byte plus one), so the read-back differs from the baseline and proves the copy instead of re-reading bytes that were already there. Every step runs through the public `ds2431.h` interface. The write cycle is one-shot: once every device has been served the example prints `Demo done.` and then keeps re-running the family-filtered search once a second, so it goes on streaming its `Found N device(s)` census and ROMs instead of falling idle. |
| `9_multi`         | `examples/9_multi/main.c`             | Application example: a peak/low temperature logger with seven DS18B20 sensors and one DS2431 EEPROM on the same bus. One explicit bus-owner state machine uses the two drivers strictly in sequence, because they share TIM1/DMA and neither may be polled while the other owns the timer: DS18B20 search, a second full-family search to locate the DS2431, boot-read of four EEPROM blocks, then measurement rounds. Rounds run in scan mode (one broadcast `Convert T`) and fall back to the sequential Match ROM path if a scan fails. The round maximum and minimum are compared with the stored peak and low; a new record is queued as Write Scratchpad → Read Scratchpad (echo + data verify) → Copy Scratchpad → Read Memory, with the sensor's ROM block written before the record that refers to it, so a power cut cannot leave a record pointing at a ROM that is not there. Records are self-validating 8-byte blocks (magic, version, flags, counter, temperature, sensor index, CRC-8); `PEAK_RESET=1` ignores the stored records at boot. Every stage is a `poll()` loop paced by `app_millis()` — no delay, no wait, no interrupt. |

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
# ST-LINK drives onto OSC_IN and which is read in bypass mode. This family has
# no HSI16 and PLLMUL tops out at 16, so 64MHz is all the internal RC can reach.
make OW_MCU=STM32F303xC APP=4_scan_mode
make OW_MCU=STM32F303xC SYSCLK_MHZ=64 APP=4_scan_mode   # HSI/2 + PLL, no external clock
make OW_MCU=STM32F303xC SYSCLK_MHZ=8  APP=4_scan_mode   # raw HSI, nothing configured

# STM32F407 target (same examples, bus on PA10):
make OW_MCU=STM32F407xx APP=4_scan_mode

# STM32F401CC target (F401 Black Pill, 84MHz HSE+PLL default):
make OW_MCU=STM32F401xC APP=4_scan_mode

# STM32F401xE target (512KB flash / 128KB RAM, same 84MHz default):
make OW_MCU=STM32F401xE APP=4_scan_mode
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

Notes:

- All examples use `app_init()` (from `examples/app/app.h`) to set up the system
  clock, USART1 TX and the busy LED in a single call.
- `1_basic` uses Skip ROM, so it is meant for a **single sensor** on the bus.
  With several sensors connected, all of them respond to the read command and
  the bus data collides (CRC failures are expected).
- `2_device_search` performs a startup Search ROM, then measures each discovered sensor
  **individually** via Match ROM (one `Convert T` per device, no broadcast
  conversion) in round-robin order. A separator `--------------------------------`
  is printed between full rounds. With one sensor it behaves like `1_basic` but
  with ROM addressing; with N sensors a round costs `N × conversion`. Supports
  `-DOW_PARASITE_POWER=1` (strong pull-up handled per conversion).
- `3_round_robin` measures the devices found at startup one at a time, in round-robin
  order. With exactly one sensor it behaves like `1_basic`.
- `4_scan_mode` (scan mode) converts every discovered sensor in parallel: a single
  conversion wait covers all devices, so N devices take `1 x conversion + N x
  read` instead of `N x conversion`. Before converting, it programs the
  conversion resolution to every sensor with one broadcast Write Scratchpad
  (`ds18b20_set_resolution()`), because scan mode assumes a uniform resolution:
  the single conversion wait must match the whole fleet. Skipping this can make
  a 12-bit sensor that follows a 9-bit one read back its 85.0 °C power-on-reset
  value. Each reading is reported through `ds18b20_complete()` in device-table
  order; `ds18b20_scan_index()` / `ds18b20_device_rom()` identify the sensor.
  Scan mode is mutually exclusive with `ds18b20_select()`.
  With several parasite-powered devices, a simultaneous broadcast conversion
  draws all their current from the one strong pull-up. A sagging supply (for
  example feeding the bus from a USB adapter) can brown out a marginal sensor
  (a stuck 127.9 °C reading with a valid CRC). Feed the fleet from a dedicated
  supply with an adequate pull-up — see the STM32F407VGT6 section in Hardware
  Verified — and the broadcast conversion stays reliable at the high PLL clock.
- `5_commands` targets the first sensor found by the search (Match ROM) and runs the
  non-blocking command sequence once at startup: power supply, raw scratchpad,
  TH/TL write with a Copy/Recall pair to demonstrate EEPROM persistence, and
  the single-device Read ROM. Persistence is real and nothing undoes it: the
  Copy leaves the demo's TH/TL (0x19 / +25C, 0x0F / +15C) in the sensor's
  EEPROM, not the pair it shipped with, and they load again at the next
  power-up. Each command advances by one hardware operation
  per `*_poll()` call; `ds18b20_last_command_ok()` verifies the result. The
  sequence closes with forced alarm thresholds (TH=-55C, TL=+125C on the
  selected device, so it alarms deterministically whatever the room
  temperature) plus an Alarm Search (0xEC) pass that reports only alarmed
  devices — the only example exercising
  `ds18b20_alarm_search_start()` / `ds18b20_alarm_search_poll()` /
  `ds18b20_alarm_search_count()`. Other fleet members may join from thresholds
  retained in their own RAM/EEPROM.
- `6_statistics` extends the `3_round_robin` sequential loop with signal statistics
  (`-DOW_STATS_ENABLE=1`, auto-enabled by `make APP=6_statistics`). After
  `STATS_DUMP_SWEEPS` sweeps - one sweep is one pass over every device, so a
  batch covers `STATS_DUMP_SWEEPS * devices` measurements and yields that
  many samples per sensor - the accumulated
  per-sensor pulse-width min/max,
  13-bucket histogram (0–60+ µs) and error counters are streamed over UART by
  `ow_stats_dump_poll()` (one line per call, non-blocking); the measurement
  loop is paused during the dump and resumed afterwards via `ow_stats_reset()`.
  Supports `-DOW_PARASITE_POWER=1`.
- Programming targets (`make jprogram` / `make program`) flash whichever
  example is currently selected by `APP`.
- Two build knobs have no dedicated example by design: `-DOW_DRIVE_ACTIVE=1`
  switches master-only write slots to push-pull (experimental; covered by the
  `test-active` host suite, same loop as every example so nothing demo-specific
  would show on the console), and `-DDS18B20_MAX_DEVICES=1` trims the scan
  table from 64 to 8 bytes for single-sensor builds (see CHANGELOG).

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
`CBR1.BNDT` is a source-byte count. The other families compile the example and
run level 0 with `stress=unsupported` in the banner, so the all-family build
matrix stays green.

On the F446 at its 180 MHz default every level is clean at guard 1 (a 2 µs
window), the same result as the F030. At 16 and 8 MHz levels 0-4 are also clean
at guard 1; only level 5 is limited by the fixed 512-word CPU hammer: it needs
guard 89 at 16 MHz and starves the feed even at guard 100 at 8 MHz. That is a
measured property of this worst-case synthetic load, not a generator failure;
run `python tests/hil/dma_pressure.py --clocks 16,8` to reproduce it. The F446
bench also wires PA11 to D1, so its pressure and capture matrices use the same
marker-bracketed analysis as the H503.

On the H503 every level is clean at the 5 µs default at 64, 250 and 8 MHz; the
measured floor is guard 1 except level 1 at 8 MHz (guard 2), and the earlier
level 0/4 outliers disappear once the D1 marker brackets the frames. The GPDMA
negative control suspends the channel (`CCR.SUSP`) instead of clearing `EN`,
because a pending GPDMA request is not dropped by clearing the enable bit.

The H503 bench wires PA11 to the logic analyzer's D1. `STRESS_MARKER=1` makes
each frame (or the search pass) a D1-high window, so the HIL analyses only the
D0 traffic inside the marker: a startup glitch or a capture that ends mid-frame
is outside every frame and cannot be mistaken for one. Every cell also reads
`dma_pressure_telemetry[17..19]` before trusting the waveform: run magic,
effective level/clock, capture mode and the injection state bits. A stale image
or an injection that did not actually stop the channel is retried once and then
reported as an identity failure, not as a bus failure. Cells that only pass on a
retry keep `attempts` and `first_attempt_*` in the JSON, so a green result is not
silently a flake.

```bash
# a build with a load and a narrow guard
make APP=0_dma_pressure OW_MCU=STM32F030x6 \
     EXT="-DOW_PARASITE_POWER=0 -DSTRESS_LEVEL=4 -DONEWIRE_GUARD_BAND=1"

# the full matrix (rebuilds per cell; needs the fx2lafw LA and an ST-Link)
make test-hil HIL_SCRIPT=dma_pressure HIL_MCU=STM32F030x6
python tests/hil/dma_pressure.py --levels 0,4,5 --guards 1,5 --clocks 48,8
```

Measured on the F030 bench (100 frames per cell, D0 on PA10, 4 MS/s). Every
cell is clean at guard 1; `cndtr_late` is 0 everywhere:

| Level | Load evidence (48 MHz / 8 MHz) | guard 5 | guard 1 |
|---|---|---|---|
| 0 | none | clean | clean |
| 1 | 3300 / 700 CPU passes | clean | clean |
| 2 | 500 / 501 DMA bursts | clean | clean |
| 3 | 8400 / 1400 DMA bursts | clean | clean |
| 4 | 8400 / 1400 ch1 bursts | clean | clean |
| 5 | 3300 / 700 ch1 bursts + 3300 / 700 CPU passes | clean | clean |

The load is real: channel 1 restarted its finite burst 8400/1400 times at
level 4 and 3300/700 times at level 5 (the CPU hammer takes part of the
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
feed at 80 µs and the search harness detects the first delayed capture at
200 µs - a late transfer, not only a wrong count or route.

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
