# Examples and benches

[Back to the README](../README.md)

## Examples

The repository has ten example applications. Pick one with `APP`.
`0_dma_pressure` is a diagnostic and needs no sensor; the other nine drive real
1-Wire devices.

> **Warning: three examples write non-volatile sensor memory.**
> `5_commands` tries Copy Scratchpad on the selected DS18B20 and replaces its
> EEPROM alarm thresholds with TH=0x19 / TL=0x0F. It does not restore the old
> values, and repeated runs consume EEPROM endurance.
> `8_ds2431` replaces the first 8 bytes (address 0) of every DS2431 it finds
> with a derived pattern and does not restore the original contents.
> `9_multi` writes its peak/low records and the ROM blocks they refer to
> (addresses 16..47) on the first DS2431 it finds, but only when a record
> changes, and does not restore the old contents.
> The DS2431 HIL check does the same. Use test devices with no data you need to
> keep, and do not run these demos repeatedly on deployed sensors.

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

### Captures

Each example below has a logic-analyzer capture committed under
`docs/examples/captures/`. The links open one window in the browser viewer
(STM32F030x6 bench, parasite-powered bus on PA10/D0, 4 MS/s). `1_basic` has no
capture because it needs a single sensor; the reference bench has several, so
Skip ROM would collide.

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

Diagnostic only; no sensor is needed. Build-time knobs are `STRESS_LEVEL=0..5`,
`STRESS_BURSTS`, `STRESS_WORDS`, `STRESS_CAPTURE`, `STRESS_INJECT_US` and
`STRESS_MARKER`. The measured matrix and analyzer contract are in
[Measuring DMA feed margin](#measuring-dma-feed-margin-0_dma_pressure) below.
Capture: [open in the viewer](https://a5021.github.io/stm32-async-1wire/viewer/?src=./../examples/captures/0_dma_pressure.sr&chan=0&speed=standard).

#### `1_basic`

Reads one sensor with Skip ROM (0xCC). With several sensors on the bus, all of
them answer and the data collides, so CRC failures are expected.
No capture: this example needs a single DS18B20, so Skip ROM would collide on
the multi-drop bench. See [2_device_search](#2_device_search).

#### `2_device_search`

Runs Search ROM at startup, then measures each sensor individually with Match
ROM: one `Convert T` per device, no broadcast conversion, in round-robin order.
A separator line marks each full round. With one sensor it behaves like
`1_basic`, but with ROM addressing. With N sensors a round costs
`N × conversion`. Supports `-DOW_PARASITE_POWER=1`; the strong pull-up is
handled per conversion.
Capture: [open in the viewer](https://a5021.github.io/stm32-async-1wire/viewer/?src=./../examples/captures/2_device_search.sr&chan=0&speed=standard).
CPU cost: [measured driver work](cpu-profile.md).

#### `3_round_robin`

Measures the devices found at startup one at a time in round-robin order. With
one sensor it behaves like `1_basic`.
Capture: [open in the viewer](https://a5021.github.io/stm32-async-1wire/viewer/?src=./../examples/captures/3_round_robin.sr&chan=0&speed=standard).

#### `4_scan_mode`

Converts every discovered sensor in parallel: one conversion wait covers all
devices, so N devices take `1 × conversion + N × read` instead of
`N × conversion`. Before converting, it writes the same resolution to every
sensor with one broadcast Write Scratchpad (`ds18b20_set_resolution()`). Scan
mode assumes a uniform resolution, so the single wait matches the whole fleet.
If a 12-bit sensor follows a 9-bit one without this step, it can read back its
85.0 °C power-on-reset value. Each reading arrives through `ds18b20_complete()`
in device-table order; `ds18b20_scan_index()` and `ds18b20_device_rom()`
identify the sensor. Scan mode cannot be combined with `ds18b20_select()`.

With several parasite-powered devices, one broadcast conversion pulls all their
current through the single strong pull-up. A weak supply, such as a USB adapter,
can brown out a marginal sensor and produce a stuck 127.9 °C reading with a
valid CRC. Use a dedicated supply with an adequate pull-up (see the
STM32F407VGT6 section in Hardware Verified). The broadcast conversion then
stays reliable at the high PLL clock.
Capture: [open in the viewer](https://a5021.github.io/stm32-async-1wire/viewer/?src=./../examples/captures/4_scan_mode.sr&chan=0&speed=standard).

#### `5_commands`

Targets the first sensor found by the search (Match ROM) and runs the
non-blocking command sequence once at startup: power supply, raw scratchpad,
TH/TL write with Copy/Recall to show EEPROM persistence, and single-device Read
ROM. Copy leaves the demo's TH/TL (0x19 / +25 °C, 0x0F / +15 °C) in EEPROM,
and they load again at the next power-up. Each command advances by one hardware
operation per `*_poll()` call; `ds18b20_last_command_ok()` checks the result. The sequence ends with forced
alarm thresholds (TH=-55 °C, TL=+125 °C on the selected device, so it alarms
regardless of room temperature) and an Alarm Search (0xEC) pass. This is the
only example that uses `ds18b20_alarm_search_start()`,
`ds18b20_alarm_search_poll()` and `ds18b20_alarm_search_count()`. Other sensors
may join from thresholds kept in their own RAM or EEPROM.
Capture: [open in the viewer](https://a5021.github.io/stm32-async-1wire/viewer/?src=./../examples/captures/5_commands.sr&chan=0&speed=standard).

#### `6_statistics`

Adds signal statistics to the `3_round_robin` loop (`-DOW_STATS_ENABLE=1`,
added automatically by `make APP=6_statistics`). After `STATS_DUMP_SWEEPS`
sweeps (one sweep is one pass over every device, so a batch covers
`STATS_DUMP_SWEEPS × devices` measurements), it streams the per-sensor
pulse-width min/max, a 13-bucket histogram (0–60+ µs) and error counters over
UART with `ow_stats_dump_poll()` (one line per call, non-blocking). The
measurement loop pauses during the dump and resumes with `ow_stats_reset()`.
The period is `STATS_DUMP_SWEEPS` (default 10), defined in the example source
and overridable with `-DSTATS_DUMP_SWEEPS=N` in `EXT`. Supports
`-DOW_PARASITE_POWER=1`.
Capture: [open in the viewer](https://a5021.github.io/stm32-async-1wire/viewer/?src=./../examples/captures/6_statistics.sr&chan=0&speed=standard).

#### `7_low_power`

Same search and sequential loop as `2_device_search`, built with
`-DOW_LOW_POWER=1`. The driver idles the core in `__WFE()` during a long stage.
The mechanism and the SysTick caveat are in
[Low-power mode](#low-power-mode-7_low_power) below. Without the define, the
example uses the normal polling loop.
Capture: [open in the viewer](https://a5021.github.io/stm32-async-1wire/viewer/?src=./../examples/captures/7_low_power.sr&chan=0&speed=standard).

#### `8_ds2431`

Runs a startup search and a full DS2431 EEPROM write cycle: Read Memory
(baseline), Write Scratchpad, Read Scratchpad (TA/ES echo and written-data
check), Copy Scratchpad (t_PROG hold-off), then Read Memory again to confirm the
data reached EEPROM. The pattern is the row's own next value (every byte plus
one), so the read-back differs from the baseline and proves the copy. Every
step uses the public `ds2431.h` interface. The write cycle runs once. After
every device is served, the example prints `Demo done.` and then repeats the
family-filtered search once a second, so it keeps printing `Found N device(s)`
and the ROMs instead of going idle.
Capture: [open in the viewer](https://a5021.github.io/stm32-async-1wire/viewer/?src=./../examples/captures/8_ds2431.sr&chan=0&speed=standard).

#### `9_multi`

A peak/low temperature logger with seven DS18B20 sensors and one DS2431 EEPROM
on the same bus. One bus-owner state machine uses the two drivers in sequence,
because they share TIM1/DMA and neither may be polled while the other owns the
timer: DS18B20 search, a second full-family search to find the DS2431, boot-read
of four EEPROM blocks, then measurement rounds. Rounds use scan mode (one
broadcast `Convert T`) and fall back to the sequential Match ROM path if a scan
fails. The round maximum and minimum are compared with the stored peak and low.
A new record is written as Write Scratchpad → Read Scratchpad (echo + data
verify) → Copy Scratchpad → Read Memory. The sensor's ROM block is written
before the record that refers to it, so a power cut cannot leave a record
pointing at a missing ROM. Records are self-validating 8-byte blocks (magic,
version, flags, counter, temperature, sensor index, CRC-8); `PEAK_RESET=1`
ignores stored records at boot. Every stage is a `poll()` loop paced by
`app_millis()`; there are no delays, waits or interrupts.
Capture: [open in the viewer](https://a5021.github.io/stm32-async-1wire/viewer/?src=./../examples/captures/9_multi.sr&chan=0&speed=standard).

### Common notes

- All examples call `app_init()` from `examples/app/app.h` to set the system
  clock, USART1 TX and the busy LED.
- `make jprogram` / `make program` flash the example selected by `APP`.
- Two build knobs have no dedicated example. `-DOW_DRIVE_ACTIVE=1` switches
  master-only write slots to push-pull (experimental; covered by the
  `test-active` host suite). `-DDS18B20_MAX_DEVICES=1` trims the scan table
  from 64 to 8 bytes for single-sensor builds (see CHANGELOG).

### Measuring DMA feed margin (0_dma_pressure)

The CCR3 feed asks DMA for one transfer per slot at the CC2 compare
(`ONE + ZERO` microseconds). The next slot starts at the update event
(`ONE + ZERO + GUARD + 1`). The transfer must land in that window. The feed
channel uses the default DMA priority, so an application that moves data with
DMA1 competes for the same bus. All load channels in this rig also stay at the
default priority. A higher-priority application channel (`PL > 0`) is a
different case and is not covered here.

`0_dma_pressure` sends a fixed 8-byte pattern (64 slots plus the trailing
`ONEWIRE_RELEASE_PULSE`) back to back while a build-time load runs. It reports
two on-chip symptoms:

- `cndtr_late`: after `onewire_bus_done()` the feed channel still had transfers
  queued. This is complete starvation: at least one slot repeated, and the
  final release may be missing.
- `dma_error`: the feed channel latched a transfer error.

A stretched pulse or a doubled edge does not change either counter; the logic
analyzer shows those. The HIL script bisects `ONEWIRE_GUARD_BAND` per level. The
smallest guard with clean per-slot geometry bounds the feed latency from above.
The timer resolution is 1 µs, so guard 1 (a 2 µs window) is the measurement
floor: a clean run there means a 2 µs window was enough.

| `STRESS_LEVEL` | Load (F0/F4/H5) |
|---|---|
| 0 | none - the driver's own margin |
| 1 | CPU hammers the SRAM array in the `onewire_bus_done()` loop |
| 2 | one mem-to-memory burst on the first load channel about every 1 ms |
| 3 | the same burst restarted as soon as it drains |
| 4 | a burst on the second load channel, restarted as soon as it drains (circular is not allowed in mem-to-memory) |
| 5 | level 4 plus the CPU hammer |

On F0 the load channels are DMA1 channel 2 and channel 1. On F4 they are DMA2
stream 0 and stream 1 with `DIR=0b10` (memory-to-memory). On H5 they are GPDMA1
channel 0 and channel 1 in direct mode with a software request; `CBR1.BNDT` is a
source-byte count and fits at most 32767 halfwords. GPDMA does not break ties by
channel number: arbitration uses `CCR.PRIO` (PRIO 3 is a fixed class; PRIO 0/1/2
share a weighted round-robin). `-DSTRESS_GPDMA_PRIO=N` selects the load class:
0 is equal priority, 3 is the priority case. Other families compile the example
and run level 0 with `stress=unsupported` in the banner, so the all-family
build matrix stays green.

On the F446 at its 180 MHz default every level is clean at guard 1 (a 2 µs
window), the same as the F030. At 16 and 8 MHz levels 0-4 are also clean at
guard 1. Level 5 with the default load on DMA2 stream 0/1 fails at 16/8 MHz.
This is the stream-priority experiment, not a timer limit: stream 0/1 are
lower-numbered than the feed on stream 2, so at equal priority the F4 arbiter
runs the load first. Moving the same 512-word burst and CPU hammer to stream
5/6 (`--load-high`, or `STRESS_LOAD_HIGH=1`) makes the whole 0-5 matrix clean at
guard 1 at 8 and 16 MHz; 180 MHz was already clean. Level 5 is a fixed 512-word
burst plus the CPU hammer, not a load normalized to the slot. A manual
isolation sweep on 2026-11-04 (test-only `STRESS_WORDS` / `STRESS_CPU_WORDS`
knobs, not in the published JSON, guard not recorded) showed the result depends
on burst length and hammer size: 128 words with a minimal hammer stayed clean at
8 MHz, 160 words failed, and 512 words with a one-word hammer was clean again
at guard 100. There is no single word-count threshold; level 5 is a
stress-to-failure point, not a driver limit. For real code, keep heavy
continuous DMA off stream 0/1 (streams 5/6/7 let the feed win) or raise the feed
priority. The F446 bench also wires PA11 to D1, so its pressure and capture
matrices use the same marker-bracketed analysis as the H503.

On the H503, the equal-priority write matrix was measured on 2026-11-07 at
`a19462ae`. Every level is clean at the 5 µs default at 64, 250 and 8 MHz, and
the floor is guard 1 in every cell. The earlier level 0/4 outliers disappear
once the D1 marker brackets the frames.

The GPDMA negative control suspends the channel (`CCR.SUSP`) and waits for
`CSR.SUSPF`. Taking a live channel down also accepts `IDLEF`, because the
transfer may have finished before the suspend landed, then uses `CCR.RESET`;
clearing `EN` alone does not drop a pending request.

Raising the load to `CCR.PRIO 3` gives a different result. At 250 MHz levels
0-4 are clean at guard 1 and level 5 needs 2 µs, so the 5 µs default still
passes. At 64 MHz levels 0-1 pass at guard 1, level 2 needs 2 µs, levels 3-4
need 5 µs, and level 5 needs 13 µs and fails at the default. At 8 MHz levels
0-1 pass at guard 1, level 2 fails at the default, and levels 3-5 never pass.
The fixed high class outranks the feed; the equal-priority round-robin does
not.

The Search ROM matrix was re-run on 2026-11-06 at `ac57ff8`: all 36 cells match
the baseline with 7 devices and `88DB0C9D` at 64, 250 and 8 MHz, guards 5 and
1. An earlier run reported 33 of 36 and blamed the capture engine at 8 MHz
(levels 3/4). That was wrong. Every failing cell had `start_timeout = 1`: the
load burst finished between the `CCR.EN` read and the `CCR.SUSP` write in
`stress_gpdma_idle()`, so hardware cleared `EN` and the suspend was never
acknowledged. The harness left the search loop, and the analyzer window ended
mid-pass. The 8 MHz cells hit it because that gap is longest in real time, and
levels 3/4 re-arm bursts back-to-back. Accepting `IDLEF` or `SUSPF` fixes the
restart, since either means no transfer is active. The republished
`platforms/h503/capture.json` replaces the three aborted runs. An aborted trace
still shows the reset, presence and `0xF0` correctly, then stops after a read
pair; that is why the old waveform looked like a search failure.

The H503 bench wires PA11 to the logic analyzer's D1 (the F446 uses PA11, the
F030 PA2). With `STRESS_MARKER=1`, each frame or search pass is a D1-high
window, so the HIL only analyses D0 traffic inside the marker. A startup glitch
or a capture that ends mid-frame is outside every window. Every cell also reads
`dma_pressure_telemetry[17..21]` and the capture-mode word before trusting the
waveform: run magic, effective level/clock, injection state, start timeout and
load error. A stale image, an injection that did not stop the channel, or a
load channel that did not stop before re-arming is retried once and then
reported as an identity or start-timeout failure, not a bus failure. Cells that
pass only on a retry keep `attempts` and `first_attempt_*` in the JSON. A load
channel that latches a transfer error raises `load_error`; the cell fails, so
the verdict cannot claim a load that had already stopped.

The F446 was re-measured on 2026-11-05 at `f2ff4fe7` from a clean tree, with
per-cell image hashes in `results.json` / `results-high.json`. The same run
reproduced the split above: level 5 at 16 MHz needs 89 µs, at 8 MHz never
passes; with the load on streams 5/6 every cell is clean at guard 1. The H503
was re-measured the same day at `84f32003`. Both platform runs are schema 2
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
PA2 marker and run-identity checks. `docs/stress/results.json` carries a SHA-256
of the flashed image for every cell. The F030 bench ran 100 frames per cell,
with D0 on PA10 at 4 MS/s. Every cell is clean at guard 1; `cndtr_late` is 0
everywhere:

| Level | Load evidence (48 MHz / 8 MHz) | guard 5 | guard 1 |
|---|---|---|---|
| 0 | none | clean | clean |
| 1 | 3300 / 700 CPU passes | clean | clean |
| 2 | 500 / 501 DMA bursts | clean | clean |
| 3 | 8300 / 1400 DMA bursts | clean | clean |
| 4 | 8300 / 1400 ch1 bursts | clean | clean |
| 5 | 3500 / 700 ch1 bursts + 3500 / 700 CPU passes | clean | clean |

Load activity: channel 1 restarted its finite burst 8300/1400 times at level 4
and 3500/700 times at level 5 (the CPU hammer takes part of the restart time).
The CPU pass counts are in the table. The feed stayed clean at a 2 µs window in
every cell, so the 5 µs default needs no change on F0. This result is specific
to the F030; other DMA IPs (F4 streams, H5/H7 GPDMA) need their own run.

Circular mem-to-memory is not allowed on F0 (RM0091), so level 4 is a stream of
restartable finite bursts on channel 1.

`STRESS_CAPTURE=1` switches the rig to a Search ROM pass. Every read bit goes
through the capture DMA (channel 4), so the same levels stress the capture drain
as well as the feed. The unloaded level-0 search is the baseline. All 24
stressed cells (6 levels × 2 clocks × 2 guards) returned the same 7 DS18B20 ROMs
and checksum, and the viewer shows all 8 responders on the bench (seven DS18B20
plus one DS2431). `STRESS_INJECT_US=N` is the negative control: it disables the
feed (write mode) or capture (search mode) channel for N microseconds, then
restores it. The write harness detects the first delayed feed at 16 µs at
48 MHz and 20 µs at 8 MHz. The search harness detects the first delayed capture
at 200 µs.

Two geometry details matter here. Guard 0 is not an operating point:
CC2 then coincides with the update event, and the release write produces a
spurious end-of-frame pulse, so the smallest meaningful guard is 1. Slot 1 is
armed by a register write, not the feed DMA, and its pulse includes the CPU gap
between enabling OC3 and starting the timer (about 1 µs at 48 MHz, 7 µs at
8 MHz). The check skips that slot's width and the first period of each frame.

`tests/hil/test_dma_pressure.py` replays synthetic captures for each failure
shape (stretched pulse, doubled edge, missing slot, wrong bit, parked line,
wrong guard band) and runs in `make check`, so the analyser cannot silently
miss a failure.

The published report, with the measured tables and a representative frame per
level/clock/guard cell in the viewer, is at
<https://a5021.github.io/stm32-async-1wire/stress/>. It is generated from
`docs/stress/results.json` by `tools/stress_report.py`; `make
test-stress-report` regenerates it and decodes every linked capture.

### Low-power mode (7_low_power)

`7_low_power` runs the same search and sequential loop as `2_device_search`, but
with `-DOW_LOW_POWER=1` the driver idles the core in `__WFE()` during a long
stage (strictly more than 1 ms). TIM1 and DMA keep generating the bus timing, so
1-Wire execution and result timing are unchanged; only the CPU stops spinning.

- Long stages: the temperature conversion (up to 750 ms), the scratchpad read
  (~5 ms) and an EEPROM hold-off (10 ms). Reset, commands and search reads stay
  on the normal polling path.
- The TIM1 update interrupt source (UIE) is enabled only to create a pending
  event that wakes `WFE()` through `SEVONPEND`. No ISR is installed and no NVIC
  interrupt is enabled.
- Power draw is not measured yet. The example shows the mechanism and the
  CPU-time saving.

One caveat applies to `app_millis()`: it counts observed SysTick wraps, so it
under-counts while the core sleeps inside `ds18b20_poll()` for up to 750 ms. In
a low-power build the value is valid for the pauses between cycles (where the
loop spins) and deliberately excludes the conversion time. `make fuzz-lowpower`
drives the sleep loop on the host with `-DMOCK_WFE_HOOK`; see
[Building, testing and integration](building.md#testing).
