# STM32F4 backend (STM32F407 / STM32F401) — hardware notes

Bring-up notes for `ow_port_f4.h`: the peripheral topology that works, and the
alternatives that were tested on real hardware and rejected. The operating
invariants live in the code comments and in the "Required Timer Capabilities"
section of `README.md`; this file keeps the experiments. Everything here was
measured on the **STM32F407VGT6** (STM32F4DISCOVERY); the F401 shares the same
TIM1/DMA2/`CHSEL=6` topology by construction (the CHSEL note below cites RM0368
— the F401 reference manual — and matches silicon behavior on the F407), but
the 84MHz F401 configuration has **not yet been run on an F401 board**.

## F401CC specifics (`OW_CHIP=f401xc`)

The F401 needs no port-level changes — only the build/case layer: `STM32F401xC`
CMSIS device + startup (`stm32f401xc.h` / `startup_stm32f401xc.s`), the
`STM32F401CC_FLASH.ld` linker script (256KB flash / 64KB SRAM; F401 has no
CCM), an 84MHz clock-config branch in `app.c` (8MHz HSE → PLL M=8,N=168,P=2;
2 wait states; APB1/APB2 /2 keeps TIM1 at SYSCLK = 84MHz, so the 1µs-tick
invariant holds), and a library clock default of 84MHz for the `STM32F401xC` /
`STM32F401xE` device macros. The console UART paths in `app.c` now derive PCLK1/
PCLK2 from the active prescalers, so 84MHz gets the correct 42MHz baud clocks.
`SYSCLK_MHZ=16` (raw HSI) is the no-crystal fallback.

## DMA direct mode and the 16-bit feed

The F4 DMA streams are used in **direct mode** (FIFO disabled). In direct mode
the memory transfer width is forced to equal the peripheral width (`PSIZE`),
and packing/unpacking between different source/destination widths is available
only in FIFO mode (AN4031 §1.1.9). `CCR3` is a 16-bit (halfword) register, so:

- the feed source must be a **halfword** buffer — the family-typed `ow_pulse_t`
  (`uint16_t` on F4) feeds `CCR3` zero-copy from the shared command buffer
  (`&ow_cmd_buf[1]`: the first slot is latched directly) with `MSIZE_0` set;
- the capture path moves matching 16-bit halfwords (`MSIZE_0` = `PSIZE_0`).

### Rejected: 8-bit feed (`MSIZE=8`) under a halfword PSIZE

Tried feeding the byte command buffer directly with `MSIZE=8`. The stream
silently reads the bytes as halfwords, so consecutive slot durations are
recombined: for a `0xF0` Search ROM the durations `60, 60, 60, 60, 5, 5, 5, 5`
come out as `60|60<<8 = 15420`, `60|5<<8`, `5|5<<8`, … Every recombined value
is far larger than the slot `ARR`, so the line is held LOW across several
slots.

On the logic analyzer (8 MHz) the `0xF0` command's trailing bits merged into a
single ~213 µs LOW (3 × 71 µs slots). Search then returned all-zero ROMs with
CRC failures. With the halfword feed the same command reads back clean
(`60, 60, 60, 60, 5, 5, 5, 5`).

### Rejected: equal-width byte feed (`PSIZE=MSIZE=8`) into CCR3

Storing 8-bit DMA writes straight into the 16-bit `CCR3` was also tested and
rejected: the timer does not latch them, and search again returns all-zero ROMs
with CRC failures. The source must therefore be widened to halfwords, which is
why the feed is zero-copy from the internal `ow_pulse_t` command buffer with no
staging copy.

## Forced-update race — resolved

`ow_port_update_event()` forces a timer update (EGR=UG) to reload the
ARR/RCR/CCR preloads and give a freshly scheduled operation a clean
completion flag. On the F4 the UG-raised UIF appears a few timer cycles
*after* the EGR write (APB2 = 84MHz, TIM1 = 168MHz — the timer is 2×
faster), so clearing SR immediately after EGR=UG races the set and can
drop the clear. The stale UIF then makes the next ow_port_bus_done()
report the operation complete before it has started — seen on hardware
as a conversion wait that returns immediately and every sensor reporting
its 85.0 °C power-on-reset value.

Current fix: a dummy `(void)T1.SR` read flushes posted APB writes into the
timer domain, so UG is fully processed and UIF guaranteed set before the
subsequent `SR=0` clear — the clear cannot lose the race. Verified in both
`ow_port_update_event()` and `ow_port_kick()` on F407 (example
2_device_search, 7 devices, parasite power, 168MHz).

## Multi-frequency fleet validation (all 6 multi-sensor examples)

Full hardware matrix after the URS/`__DSB()` removal: every example that
operates several sensors was built (`-Os -flto`, release default) and flashed
to the F407DISCOVERY's 7 DS18B20s on a parasite-powered bus, across all three
supported clock configurations:

| Example (parasite) | 168 MHz (HSE+PLL) | 16 MHz (raw HSI) | 8 MHz (raw HSE) |
|---|---|---|---|
| 2_device_search | Found 7, ~24 °C | Found 7, ~24 °C | Found 7, ~24 °C |
| 3_round_robin (9→12 bit cycle) | Found 7 | Found 7 | Found 7 |
| 4_scan_mode (broadcast Convert T) | Found 7 | Found 7 | Found 7 |
| 6_statistics (round-robin + stats) | Found 7 | Found 7 | Found 7 |
| 7_low_power (WFE sleep, `OW_PORT_LOW_POWER=1`) | Found 7 | Found 7 | Found 7 |
| 5_commands (parasite detect, scratchpad/EEPROM) | CRC ok, parasite | CRC ok, parasite | CRC ok, parasite |

Every run: **Found 7 device(s)**, temperatures 23.9–24.5 °C, scratchpad CRC ok,
no stale-UIF 85.0 °C symptoms, no CRC/no-sensor failures. 5_commands' Read ROM
shows the expected multi-device CRC fail annotation (single-ROM command on a
7-device bus). Build-time knobs exercised: `SYSCLK_MHZ=168|16|8` (mapping to
`OW_PORT_SYSCLK_MHZ`), `-DOW_PARASITE_POWER=1`, `-DOW_PORT_LOW_POWER=1`,
`-DOW_STATS_ENABLE=1` (auto for `APP=6_statistics`).

### Re-validated after the polled-clock rework

The matrix above was first measured when the examples still paced themselves
with an interrupt-driven tick. The time base is now the ARM SysTick counter
read by polling `COUNTFLAG` at 1 kHz — no handler at all — and the WFE sleep
for `-DOW_PORT_LOW_POWER=1` moved inside `ds18b20_poll()`. Both changes touch
the low-clock rows, because `SysTick->LOAD` is derived from
`OW_PORT_SYSCLK_MHZ` (7999 at 8 MHz, 15999 at 16 MHz) and the timer update that
wakes the sleeping driver also scales with the clock. So the whole 6 × 3 matrix
was re-run on the F407DISCOVERY with the same seven parasite-powered sensors
after the rework, and still passes:

| Example @ 16 MHz | @ 8 MHz |
|---|---|
| `2_device_search` — 7 found, 0 errors, 5.75–5.82 s rounds | 7 found, 0 errors, 5.76–5.84 s rounds |
| `3_round_robin` — 9→12 bit cycle, 0 errors | 9→12 bit cycle, 0 errors |
| `4_scan_mode` — 8 full rounds, 0 CRC errors | 8 full rounds, 0 CRC errors |
| `5_commands` — parasite detected, EEPROM round-trip, CRC ok | same; Read ROM CRC fail as above |
| `6_statistics` — 0.70–0.89 s per sensor | 0 errors |
| `7_low_power` — WFE sleep inside the driver, 5.76–5.81 s rounds | 5.75–5.79 s rounds |

The round periods are the useful cross-check: they are pinned by a 5 s
application pause counted in polled milliseconds, so if the reload were wrong at
a low clock the period would drift by the ratio between the nominal and the
actual HCLK. Measured spread across 168/16/8 MHz is 5.75–5.84 s, i.e. the
counted millisecond tracks real time at every supported frequency.

## CHSEL is a per-stream mux index

DMA2 streams select their request source with their own `CHSEL` field
(RM0368 §9.3.3, Table 29). Both the feed and the capture stream use `CHSEL=6`
(`DMA2_Stream2` → `TIM1_CH2`, `DMA2_Stream4` → `TIM1_CH4`); this is not a
shared physical request line, and neither stream consumes the other's request.

## Capture-chain latency scales with the timer kernel clock

Raw pulse dumps (`-DOW_DEBUG_PULSE_DUMP=1`, one-shot on the first scratchpad
read of `6_statistics`, 7 sensors, parasite power) across `SYSCLK_MHZ` builds.
The values are CCR4 input-capture readings on a 1 µs tick; both slot levels
shift together:

| SYSCLK | IC4F filter | CCR4 `'1'` | CCR4 `'0'` | Δ vs 168 | LA physical `'1'` | LA physical read-`'0'` |
|---|---|---|---|---|---|---|
| 168 MHz (HSE+PLL) | fDTS/8, N=6 | 5 | 28 | 0 | 5.0–6.2 µs | 29.4–30.6 µs |
| 16 MHz (raw HSI) | fCK, N=8 (default) | 7 | 30 | +2 | 5.0–6.2 µs | 29.4–30.6 µs |
| 16 MHz | fCK, N=2 (`IC4F_0`) | 7 | 30 | +2 | — | — |
| 16 MHz | fCK, N=4 (`IC4F_1`) | 7 | 30 | +2 | — | — |
| 8 MHz (raw HSE) | fCK, N=8 | 9 | 32 | +4 | 5.0–6.2 µs | 29.4–30.6 µs |

Logic-analyzer column: fx2lafw @ 16 MS/s on PA10 (D0), 0.26 s windows merged
per clock, low-width histogram (write-`0` lows measure 60.0 µs and resets
≈ 481–485 µs at every clock, as they must).

- The master low is hardware-timed in µs ticks (CCR3 compare), so the physical
  waveform is identical across builds — the offset lives in the capture path
  (edge → latched CCR4), not in the emitted pulse. The LA shows no clock
  dependence while CCR4 moves 5 → 7 → 9.
- The offset is independent of the input filter depth (N=2 and N=8 measure
  identically): the IC4F configuration is not the cause.
- The offset tracks the timer **kernel** clock (before the prescaler): 32 timer
  cycles → 32/168 ≈ 0 µs, 32/16 = 2 µs, 32/8 = 4 µs. Same law as the F030@8
  datapoint in the CHANGELOG ("a 5 µs pulse measures ~9 µs").
- The slow slave release (read-`'0'`) explains why CCR4 `'0'` reads 28 at
  168 MHz while the analyzer (higher logic threshold, later crossing on the RC
  front) shows ≈ 30: the capture latches earlier on the slow edge than the
  analyzer's threshold, then the same +0/+2/+4 latency applies on top
  (29.6 − 1.6 + {0,2,4} = {28, 30, 32}).
- Decode margin: `ONEWIRE_SHORT_PULSE_MAX = 10` still clears `'1' = 9` at
  8 MHz, but with only 1 µs to spare — anything slower needs a re-check of the
  decode window.

## Board connector hazards (STM32F4DISCOVERY MB997C)

The F4DISCOVERY's USB OTG FS mini-AB connector shares pads with the driver's
pins — keep it unplugged while the driver owns the bus (same class of hazard
as the G031's USB-C on PA11/PA12):

| Pad | Driver use | OTG FS use | Failure mode with a cable plugged |
|---|---|---|---|
| PA10 | 1-Wire bus (TIM1_CH3) | OTG_FS_ID | An A-cable grounds ID → holds the bus LOW; every reset/presence fails |
| PA11 | LA marker (GPIO) | OTG_FS_D− | Probe/cable contention on the marker; USB traffic would corrupt it |
| PA12 | — (free) | OTG_FS_D+ | Only matters if USB is initialised or PA12 is repurposed |

Never enable the USB FS peripheral while the driver runs: its pin
initialisation would reconfigure PA10/PA11 away from TIM1_CH3 / the marker.
