# STM32F4 backend (STM32F407 / STM32F401 / STM32F446) — hardware notes

Bring-up notes for `ow_port_f4.h`: the peripheral topology that works, and the
alternatives that were tested on real hardware and rejected. The operating
invariants live in the code comments and in the "Required Timer Capabilities"
section of `README.md`; this file keeps the experiments. Most of it was
measured on the **STM32F407VGT6** (STM32F4DISCOVERY); the F401 shares the same
TIM1/DMA2/`CHSEL=6` topology by construction (the CHSEL note below cites RM0368
— the F401 reference manual — and matches silicon behavior on the F407), and
the F446 section records its own separate bench run on a WeAct F446RET6.

## F446 specifics (`OW_CHIP=f446xx`)

**Validated on hardware** (WeAct F446RET6, 8MHz crystal, 7 DS18B20 in parasite
power on one bus, console on the on-board CP210x at 115200). The part file, the
clock branch and the host tests are unchanged from what was written before the
board existed; what follows is the bench result. The build/case layer is the
whole of the delta — like the F401 below, the F446 needs no port-level changes,
because TIM1_CH2 on DMA2_Stream2 and TIM1_CH4 on DMA2_Stream4 at `CHSEL=6`, and
PA10 as AF1, are the same mapping the F407 uses.

What was checked before a board, and how:

- **Memory.** 512KB flash / 128KB SRAM, and no CCM. Confirmed against ST's own
  `STM32F446ZETX_FLASH.ld` and against the upstream CMSIS header and SVD, which
  contain no `CCM` peripheral at all (unlike the F405/F407, whose linker scripts
  name a 64KB CCM bank), and then confirmed on the part: `st-info --probe`
  reports `flash: 524288`, `sram: 131072`, `dev-type: STM32F446`, and the
  program's `_estack` links at `0x20020000` (top of 128KB). So
  `STM32F446RE_FLASH.ld` is a single contiguous RAM block, and the backend's
  DMA writes need nothing beyond it.
- **Clock layout.** 180MHz with 8MHz HSE, M=8 N=360 P=2, APB1 `/4`, APB2 `/2`,
  5 flash wait states, over-drive — taken from ST's `RCC_ClockConfig` example
  for the F446ZE Nucleo rather than inferred from the F407's numbers. The
  APB1 45MHz / APB2 90MHz results are that part's own limits, which is why this
  is a separate branch in `app.c` and not a divisor away from 168MHz.
- **Over-drive registers.** `PWR_CR_ODEN` (bit 16), `PWR_CR_ODSWEN` (bit 17),
  `PWR_CSR_ODRDY` (bit 16), `PWR_CSR_ODSWRDY` (bit 17), all read from
  `stm32f446xx.h`. The F407's header defines none of them, which is why the
  sequence lives in the 180MHz branch. The four values are pinned against that
  header by `tests/check_mock_headers.sh`, and `test_timing.c` asserts the whole
  sequence at register level in the 180MHz host suite.
- **VOS left alone.** The 180MHz path does not write `PWR_CR_VOS`. The F4 CMSIS
  header gives no `_0`/`_1` spellings for that field, so the encoding cannot be
  written from a verified constant, and guessing it is the one way to make this
  worse than leaving it: writing Scale 2 by mistake would cap the part at 144MHz
  and break the very clock it is trying to enable. The reasoning for leaving it
  is that the F407 runs 168MHz on that board with no VOS write either, and
  168MHz already requires Scale 1 — so the family comes out of reset in Scale 1,
  which is also what 180MHz wants. That is an inference from the F407's
  validated behavior, not a read of the F446 datasheet, and it is the one thing
  on this part the bench run did **not** settle: read `PWR_CR.VOS` at boot and
  confirm Scale 1 (`VOS` = `01`). If it reads `10`, the part is in Scale 2 and
  180MHz is out of spec even with over-drive, and the fix is a VOS write with
  the encoding taken from RM0390 rather than guessed. The board in hand runs
  180MHz correctly, so whatever VOS it is in is high enough — but that is an
  observation, not a measurement of the field, and a different module could
  differ.

The bench checklist, in the order that would catch the most, with the outcome
of each recorded after the dash:

0. **Get a console first — check which pin the board actually uses** — *done,
   and PB6 was right.* The F4 console is USART1/TX on **PB6** by default. That
   is where the STM32F4DISCOVERY routes its ST-LINK virtual COM port, and a
   WeAct F446RET6 does the same: its CP210x VCP answers on PB6, so the default
   build talks to it with no extra flags. (Measured — do not "fix" this by
   assuming PA9.) `-DOW_UART_USART1_PA9` still exists (`make OW_TARGET=f4
   OW_CHIP=f446xx EXT=-DOW_UART_USART1_PA9`) for boards that do put the VCP on
   PA9, or attach a USB-TTL adapter to whichever pin you build for. PA9 does not
   conflict with the bus: the backend takes PA10 for TIM1_CH3 and PA11 for the
   LA marker, and the console is TX only.

   Silence is the failure mode worth fearing, because it is ambiguous: the
   clock-failure banner leaves over the same pin, so "no output" cannot be told
   apart from "the clock never started" by looking at the wire. Measure the
   registers instead — see `hardware_init()` in `examples/app/app.c`. Every part
   of the USART1 bring-up is one write, and a single missed one leaves the
   peripheral in reset: no `APB2ENR.USART1EN`, or a pin left in analog mode, or
   `CR1` without `UE`, and `TXE` then never sets at all, no matter what baud
   you try. So silence means "one of those writes did not happen", not "wrong
   baud". This is not hypothetical: the `#else` selecting the USART1 path was
   appended behind a `//` comment on the USART3 branch's last line, so the
   branch never opened and the entire bring-up above sat inside the inactive
   `#if defined(OW_UART_USART3)`. It compiled clean, and cost a long
   baud-rate hunt before the preprocessor nesting was checked.

   If output appears but is garbage at 115200, that *is* a baud problem, and
   worth naming separately: a wrong APB2 assumption is exactly 2× off at 180MHz
   and shows up as garbage rather than as nothing. Silence and garbage are
   different bugs with different causes — do not chase baud first.

1. **`SYSCLK_MHZ=180` boots and the clock is really 180** — *done.* A working
   console at 115200 on USART1 is the cheap first signal — its divisor assumes
   APB2 = 90MHz, so a mis-set APB2 divider shows up as wrong baud rather than
   as a silent mis-timing. Five clocks (180/168/84/16/raw-HSE) each produced a
   clean console with a *different* `BRR` (781/729/365/139/69), which is what
   pins APB2 — and therefore SYSCLK — at each one. Measured on the 1-Wire line
   with a logic analyzer as well: low-pulse distributions are identical at
   180MHz and 16MHz (p25 = 5.562 µs, p75 = 60.0 µs both), which is the intended
   result and not a null one, because `PSC = SYSCLK_MHZ - 1` keeps the tick at
   1 µs on every supported clock. Do not expect 1-Wire timings to *scale* with
   the clock on this backend; there is nothing on the wire that does.
2. **The over-drive actually engages** — *done.* `PWR_CR` reads back with
   `ODEN` (bit 16) and `ODSWEN` (bit 17) both set after the 180MHz boot, and
   none of the bounded waits timed out. If the sequence had been skipped the
   part would still run, just out of spec, and nothing on the bus would say so.
3. **The DMA2 request map behaves as the F407's does** — *done, by consequence.*
   A wrong stream/channel would reset the port and presence-detect nothing; all
   seven sensors enumerate with valid CRC8 at every clock, which it cannot do
   unless the TIM1_CH2/Stream2 and TIM1_CH4/Stream4 requests at `CHSEL=6` are
   live on this part.
4. **PA10 is usable as a bus pin** on the WeAct F446RET6 specifically — *done.*
   The whole fleet below runs on PA10 as AF1: 7 devices found, valid CRC8.
   `-DOW_PORT_BUS_PE13=1` remains the move for a board where PA10 is not
   available.
5. **All three clocks on real hardware:** 180MHz (default), 16MHz raw HSI, and
   raw 8MHz HSE — *done, and two more besides.* 168MHz and 84MHz were added
   because they are the two other distinct APB-divider cases, and all five
   built and ran; see the matrix below. The 8MHz mode is the tight one: the
   F407 measurements below leave roughly 1µs of margin for
   `ONEWIRE_SHORT_PULSE_MAX`, and it is clean here.
6. **The capture-chain numbers.** *Not recorded for 180MHz.* The F407
   baselines at the bottom of this file are 168MHz-specific; at 180MHz the TIM1
   kernel clock is 180 instead of 168, so IC4 offsets and CCR4 `'1'`/`'0'`
   counts move. The LA captures taken here were used only to confirm the
   1-Wire timing is clock-independent (item 1); a full 180MHz capture-chain
   characterisation is still outstanding.

7. **The IC4F filter, if the captures come out wrong** — *not needed; captures
   were correct at 180MHz with the default `fDTS/16, N=6`.* For the record,
   168MHz gets `fDTS/8, N=6` (T_f ≈ 286ns) while 180MHz gets `fDTS/16, N=6`
   (T_f ≈ 533ns) — the F407 tier was picked for 84MHz and undershoots the
   ~500ns target as the clock rises, so the F446 needed its own row rather than
   inheriting one that no longer fits. That is arithmetic from the ICxF
   encoding table in `inc/ow_port.h`, not a measurement, and it is the one
   choice on this part that has still never been swept on a board. It is only
   worth sweeping if the captures are actually wrong: the F407's 286ns is the
   empirically proven setting for this bus, so `SYSCLK_MHZ=180
   -DOW_PORT_IC4F_ARGS=IC4F_3` (fDTS/8, N=6, ≈267ns) is the fallback to try
   before anything else, and `IC4F_2|IC4F_3` (fDTS/16 N=5, ≈444ns) is the next
   step down from the default.

### Open item carried forward

`PWR_CR.VOS` was never read (see the reasoning above), so the Scale-1
assumption behind leaving it alone is still an inference from the F407 rather
than a measurement on this part. 180MHz runs correctly either way on the board
in hand, but if a future F446 module misbehaves at 180 while this one does not,
read `PWR_CR.VOS` first before suspecting the PLL.

### F446 example matrix — all 7 examples × 2 clocks

Every example built (`-Os -flto`, release default) and flashed to the WeAct
F446RET6's 7 parasite-powered DS18B20s, at the two extremes of the range: raw
HSI and HSE+PLL+over-drive.

| Example (parasite) | 16 MHz (raw HSI) | 180 MHz (HSE+PLL+OD) |
|---|---|---|
| `1_basic` | CRC fail ×5 — **expected**, see below | same |
| `2_device_search` | Found 7, 5 valid reads, 23.9–24.3 °C | same |
| `3_round_robin` (9→12 bit cycle) | Found 7, 5 valid reads | same |
| `4_scan_mode` (broadcast Convert T) | Found 7, 35 valid reads | same |
| `5_commands` (parasite detect, scratchpad/EEPROM) | parasite detected, CRC ok, Read ROM CRC fail as below | same |
| `6_statistics` (round-robin + stats) | Found 7, 31 valid reads | same |
| `7_low_power` (WFE sleep, `OW_PORT_LOW_POWER=1`) | Found 7, 5 valid reads | same |

Console output was **byte-identical between the two clocks** — 170 / 369 / 656
/ 1525 / 1111 / 1281 / 396 bytes for `1_basic` … `7_low_power` — with no
clock-failure banner and no bad CRC anywhere. That equality is the useful
result: an 11.25× clock ratio producing identical byte counts and identical
valid reads is the observable form of the 1 µs-tick invariant holding across
the range.

Two lines above are correct behavior on a 7-sensor bus, not defects:

- `1_basic` uses Skip ROM, where all seven devices answer simultaneously, so
  the merged read always fails its CRC. Any single-device example is invalid on
  this bus; use the search-based ones.
- `5_commands`' Read ROM (`0x33`) is likewise a single-ROM command and reports
  `CRC fail (valid only with one device on the bus)`, which the driver
  annotates itself.

`5_commands` is the broadest functional pass of the set: Read Power Supply
returned `parasite` (confirms the bus wiring from the sensor's own answer, not
just from a build flag), and scratchpad read / write / copy-to-EEPROM / recall
all round-tripped with `CRC ok`, showing 12-bit resolution (`0x0C`) and TH/TL
thresholds correctly restored from EEPROM.

## F401CC specifics (`OW_CHIP=f401xc`)

The F401 needs no port-level changes — only the build/case layer: `STM32F401xC`
CMSIS device + startup (`stm32f401xc.h` / `startup_stm32f401xc.s`), the
`STM32F401CC_FLASH.ld` linker script (256KB flash / 64KB SRAM; F401 has no
CCM), an 84MHz clock-config branch in `app.c`, and a library clock default of
84MHz for the `STM32F401xC` / `STM32F401xE` device macros. The console UART paths
in `app.c` derive PCLK1/PCLK2 from the active prescalers, so 84MHz gets the
correct 42MHz baud clocks. `SYSCLK_MHZ=16` (raw HSI) is the no-crystal fallback.

### The crystal is a board property, not a part property

The 84MHz branch used to hardcode `M=8`, which silently assumed an 8MHz crystal —
true of the F4DISCOVERY this file is written from, false of the F401 Black Pill,
which carries **25MHz**. With M=8 a 25MHz crystal puts 25/8 = 3.125MHz into a PLL
input specified for 1–2MHz, the PLL never locks, and the wait on `PLLRDY` never
ends: the board is silent, with nothing to distinguish it from a dead one.

The divider is now derived instead of encoded. Aiming the PLL input at exactly 1MHz
gives `PLLM = OW_HSE_MHZ` and `PLLN = 2 × SYSCLK`, so 8MHz still yields M=8/N=336
at 168MHz and M=8/N=168 at 84MHz — the F407 numbers this file validated, unchanged
— while 25MHz reaches the F401's cap with M=25/N=168. `HSE_MHZ=N` sets it;
`chips/f401xc.mk` declares 25 for the Black Pill and `chips/f401xe.mk` 8, and the
header default is 8.

Two consequences worth keeping in mind:

- A wrong `HSE_MHZ` is not a compile error, so the HSE and PLL waits are bounded and
  the application prints the reason and stops. It does *not* fall back to the HSI —
  `PSC`, `SysTick` and the console divisor are all compiled against the requested
  frequency, so carrying on would scale every 1-Wire timing by an unknown factor.
- `SYSCLK_MHZ=8` used to mean "raw HSE" whatever the crystal was, so on a 25MHz
  board it compiled and ran with every timing scaled by 3.125. It is now selected
  only when `SYSCLK_MHZ == HSE_MHZ`, and mismatches are a build error.

#### Validated on the F401CC (WeAct F401 Black Pill, 25MHz crystal)

`OW_CHIP=f401xc`, default 84MHz over the board's 25MHz crystal (`HSE_MHZ=25`,
`M=25/N=168`), 7 × DS18B20 in parasite power on one bus on **PA10**, console on
USART1 TX / **PB6** at 115200 8N1, flashed over SWD. Flashed with
`EXT="-DOW_PARASITE_POWER=1"`, no other clock overrides - the 84MHz default is
what ships.

| Example | Result |
|---|---|
| `2_device_search` | 7 found, 0 errors, 24.3–24.6 °C |
| `3_round_robin` | 8 measurements, 0 CRC failures |
| `4_scan_mode` | 49 measurements, 0 CRC failures |
| `5_commands` | parasite detected, 4 × scratchpad CRC ok, EEPROM copy/recall round-trip restored TH=0x19/TL=0x0F; the one CRC fail is the expected Read ROM with 7 devices on the bus |
| `6_statistics` | banner `stats dump every 10 sweeps (70 samples with 7 devices)`, exactly 10 sweep markers per batch, `t=70c 0e`, `n10`/`e0` on all seven, histogram 1814+2307+919 = **5040 = 70 × 72** |
| `7_low_power` | `OW_PORT_LOW_POWER enabled - WFE sleep on stages > 1ms`, 8 measurements, 0 errors, 24.3–24.6 °C |

The millisecond counter is the reason to trust the rest: `MEASURE_PERIOD_MS` is
5000, and ~8 full per-device cycles fit in 60 s, i.e. ~6.0 s each against the
F407DISCOVERY's 5.75–5.84 s. A wrong `SysTick->LOAD` would scale that by the clock
ratio - 11.5 s at a 168MHz-derived reload of 84000 ticks - and it does not. So the
1 µs tick, `PSC` and the polled `app_millis()` are all correct at 84MHz, not merely
plausible.



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
