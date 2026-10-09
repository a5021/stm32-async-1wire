# Changelog

All notable changes to stm32-async-1wire — a non-blocking 1-Wire layer for
STM32 and the DS18B20 driver built on top of it — are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **G4 busy LED moved from PC13 to PA8.** The port drove PC13, which on the
  WeAct G474 Long is the user button — the blue LED is on PA8 (WeAct BSP
  `board.h`: CxT6 = PA8; CxU6 = PC6), so the LED never lit. Polarity is
  active-high, verified on the bench (active-low drove it inverted).

### Added

- **Alarm Search demo in `5_commands`.** The command sequence closes with
  forced alarm thresholds (TH=-55C, TL=+125C, so every bench sensor alarms
  deterministically) plus an Alarm Search (0xEC) pass reporting only alarmed
  devices — the first example exercising `ds18b20_alarm_search_start()` /
  `ds18b20_alarm_search_poll()` / `ds18b20_alarm_search_count()`. No new
  example directory, no build-file changes, no HW-matrix growth.

### Changed

- **Port layer second unification (experimental).** After the F4 collapse onto
  the shared TIM1/DMA core, the remaining per-backend duplication moved into
  it too: DMA channels default to D13/D14 with empty request-routing no-ops
  (G0 programs its DMAMUX through the same macros), the core `write_then_read`
  is spelled through the PROG/DISABLE vocabulary, and even the merged
  write+read pass ended up fully shared — two arm-order knobs were tried and
  both removed after full F446 hardware matrices proved each order don't-care.
  The F4 backend lost its `ow_port_dma_rearm()` and its private
  `ow_port_write_then_read()`; what stays per-backend is what genuinely
  differs: clocks, pin mux, and DMA register spelling.
  F0/F1/F3/G0 firmware is byte-identical to before; F4 differs by ~32 bytes
  of LTO codegen (same register sequences, proven by disassembly).

- **F4 DMA runs at reset-default priority.** The `PL_1` (high) bits are gone
  from the F4 capture/feed control words: with a single active channel pair,
  ties break by stream number either way (feed wins), so the bits changed
  nothing observable. No family sets DMA priority now.

- **STM32G4 port added (WeAct STM32G474CBT6 Long).** New `OW_TARGET=g4` backend
  on the shared TIM1/DMA core: 8-bit feed tables (classic DMA + DMAMUX), AF6
  on PA10 for TIM1_CH3, USART1 TX on PA9 (AF7), HSE+PLL to 170MHz (M=1, N=85,
  R=2, Range 1 Boost) and raw HSI16 at 16MHz as fallback. `.bss` 432 bytes
  on `1_basic` (same as F0 class). 14-cell HW matrix (7 apps × 170/16MHz,
  parasite power) validated. CI matrix and RAM budget gate extended.

- **F4 DMA runs at reset-default priority.** The `PL_1` (high) bits are gone
  from the F4 capture/feed control words: with a single active channel pair,
  ties break by stream number either way (feed wins), so the bits changed
  nothing observable. No family sets DMA priority now.

- **Phase buffers are a union; state enums pack to `uint8_t`.** The
  convert/read scratch and result scratch of a transaction never coexist, so
  `ds18b20_txn_ctx_t` now carries one `phase_pulses` overlay instead of
  separate `conv_cmd`/`read_cmd` buffers and per-struct `pulses` members, and
  the resident state/phase enums are `uint8_t` with `_Static_assert` guards.
  The addressed (Match ROM) table joined the union too: its prefix is now
  rebuilt from the stored selection on every operation instead of once per
  selection, so a transaction reusing the storage between measurements is
  harmless.   The constant Search ROM / Alarm Search commands moved to flash
  tables; the search context shrank from 44 to 32 bytes on STM32F0 (52 to 32
  on STM32F4, where pulse entries are 16-bit). One-bit states packed into flag
  bytes with explicit masks (no ISR exists, so read-modify-write is safe):
  transaction 32 to 24, driver context 86 to 84, resolution 4 bytes, plus
  uint8_t search counters. `.bss` on `1_basic` shrinks by 244 bytes on
  STM32F0 (676 → 432) and by 468 bytes on STM32F4 (1004 → 536). Device ROMs and the search buffer
  stay separate: both span phases where the union would be overwritten (the
  search state additionally lives in another translation unit).
  Single-sensor builds can also set `-DDS18B20_MAX_DEVICES=1` to trim the scan
  table from 64 to 8 bytes.

### Removed

- **`OW_PORT_BUS_PE13` alternate bus pin (F4).** No board used it, no bench
  ever validated it, no test or CI job covered it. The F4 bus is PA10-only;
  passing `-DOW_PORT_BUS_PE13=1` is now a hard `#error` instead of a silent
  PA10 build.

## [2.1.1] - 2026-10-04

### Changed

- **F0/G0 Release firmware uses `-flto` again.** The Cortex-M0/M0+ exclusion
  dated from a GCC 14 thin-LTO link failure (`invalid constant after fixup`);
  a 14.2 spot-check links the current F0/G0 firmware cleanly, and Arm GNU
  Toolchain 15.2 — now pinned for the CI firmware builds, matching the build
  and release workflows — does too, so `OPT_NO_LTO` is empty. On `1_basic`
  (F0 @48MHz) LTO shrinks `text` from 8316 to 3740 bytes. Bench-validated on
  STM32F030x6: all seven examples at 48MHz and 8MHz, parasite-powered,
  behave exactly as the no-LTO 2.1.0 reference logs.

## [2.1.0] - 2026-10-04

### Added

- **Capture-underrun modeling and tests for the no-presence reset path.**
  `hw_set_capture_edges()` lets the host model limit the captures taken per
  operation (0 = unlimited, the historical behaviour); honouring it in both
  the slot-loop and the temporal stepper leaves the capture DMA channel armed
  mid-transfer (CNDTR/NDTR > 0, EN set) exactly as silicon does when a reset
  gets no presence. Two tests in `test_bus_release.c` pin the contract:
  `test_reset_absent_then_present_no_stale_capture` (absent reset → channel
  armed → next reset recovers with 2 fresh captures, no stale accumulation,
  `reset_pulses[1] == 0`) and `test_reset_absent_twice_in_a_row` (CNDTR never
  accumulates). The host model has no register-access accessors and no disable
  latency, so these document and pin the end-to-end contract but cannot
  distinguish a fixed vs historical disarm — that enforcement is the driver
  wait (see Fixed) plus the real-hardware bench.

- **STM32F3 support, as the `f303xc` part** (`OW_TARGET=f3`, `OW_CHIP=f303xc`):
  the STM32F3-DISCOVERY (MB1035B, an STM32F303VC) is a build target, with the
  real upstream CMSIS header, startup file and SVD, a 256KB flash / 32KB SRAM
  linker script, and J-Flash/Ozone projects. The F3 backend is a new
  `port/stm32f3/ow_port_f3.h` of the same shape as the G0 one — five statement
  macros over the shared core in `port/common/ow_port_tim_dma.h`, which is
  **unchanged**, so F3 joins the F0/F1/G0 branch rather than extending it.

  Three things about this family are worth recording, because each was wrong in
  an intermediate draft of this work and each fails *silently* if got wrong:

    - **The DMA channels match F0/F1.** Bench-verified on MB1035B: TIM1's CC2
      feed rides channel **3** and the CH4 capture channel **4** (D13/D14),
      the same arrangement as F0/F1. An intermediate draft read RM0316
      Table 78 as ch2=CH2/ch3=CH4 (one lower); that mapping is silent on the
      bench — neither request reaches its channel, the feed DMA never fires,
      and every capture reads back empty — so the backend carries the F0/F1
      pair, quoted and measured.
  - **Three GPIO macro names differ from every other backend here.** The drive
    strength field is `GPIO_OSPEEDER_` — with an E; this header defines no
    `GPIO_OSPEEDR_*` at all. The alternate-function fields are named by their
    *position* in `AFR` (`AFRH_AFRH0..7`), so PA10 is `GPIO_AFRH_AFRH2` and
    there is no `GPIO_AFRH_AFSEL10` to write. And the family has no `RCC_AHB2ENR`
    at all: the GPIO ports and DMA1 share `AHBENR`.
  - **It has no HSI16, and its PLL is not the F4 shape.** `PLLSRC` offers only
    `HSI/2` (4MHz) or `HSE/PREDIV`, `PLLMUL` runs 2..16 and there is no `PLLN`
    at all, so **64MHz is the internal-RC ceiling** and the part's 72MHz ceiling
    is reachable only from the external oscillator. PREDIV=1 is its reset value,
    so the 72MHz path writes no `CFGR2`. `PWR_CR.VOS` does not exist on this
    family, so the error that cost a day on the F446 cannot be made here.

  Three supported clocks, all three separate `app.c` branches: **8MHz** (raw
  HSI — `RCC_CFGR.SW=00` selects HSI undivided, the /2 only feeds the PLL),
  **64MHz** (HSI/2 × PLLMUL16) and **72MHz** (HSE × PLLMUL9, read in bypass
  mode because the F3-DISCOVERY's 8MHz is the ST-LINK's MCO square wave, not a
  crystal). Anything else is a build error rather than a mis-timed firmware. As
  on F4, the two waits that can never end are bounded and report through a
  clock-failure banner that names the F3-specific fix — keep the ST-LINK attached,
  or ask for 64/8MHz. APB1 is /2 for the 36MHz limit and APB2 stays /1, so the
  1µs-tick invariant holds on all three.

  Also: the LED is **PE8 and active high** — all eight indicators on MB1035B are
  on GPIOE, per ST's own BSP header, and `BSP_LED_On` writes `GPIO_PIN_SET`,
  the opposite of F1's PC13 and F0's PA4. The console is USART1_TX on **PA9
  (AF7)**, which the F3-DISCOVERY leaves to an external USB-UART.

    Regression cover: the F3 mock is compared against the real
    `stm32f303xc.h` by `tests/check_mock_headers.sh`; a new row in
    `test_port_init_contract` pins the setup registers; and
    `test_port_f3_ic4psc_is_no_prescaler` asserts the F3-only `IC4PSC` capture
    prescaler in `CCMR2[11:10]` — absent on F4 — stays 0, since a non-zero value
     would divide the filter clock without changing any slot timing the other tests
     check, leaving it silently wrong.

- **Consumer integration fixtures for both CMake integration paths,** built
  in CI for every supported family. `tests/integration/cmake/fetchcontent`
  builds the library as a subproject, the way the README's
  `FetchContent_Declare` example does; `tests/integration/cmake/installed`
  builds a downstream consumer against an install prefix with
  `find_package`. The cmake job previously checked that the install tree
  contained certain files, which says nothing about whether those headers
  still compile on their own, or whether the exported target's public
  definitions reach a consumer at all.

- **STM32F446 support, as the `f446xx` part** (`OW_CHIP=f446xx`,
  `OW_TARGET=f4`): a WeAct F446RET6 is now a build target with the real upstream
  CMSIS header, startup file and SVD, a 512KB flash / 128KB SRAM linker script
  (the F446 has no CCM, so its SRAM is one contiguous DMA-accessible block), and
  J-Flash/Ozone projects. The F4 backend is reused unchanged: TIM1_CH2 on
  DMA2_Stream2 and TIM1_CH4 on DMA2_Stream4, both at `CHSEL=6`, and PA10 as
  AF1, all map identically on this part. **Now validated on hardware**: 180MHz
  with over-drive, and 168/84/16/8MHz, all enumerate the seven sensors with
  valid CRC8 and drive the 115200 console — see the F446 section of
  `port/stm32f4/HARDWARE-NOTES.md` for the full 7-example × 2-clock matrix and
  for how to tell a dead console from a wrong baud rate.

- **VSCode workspace now covers the F3 target.** `tasks.json` gains a
  `Build F3 (debug)` task (`make OW_TARGET=f3 debug`) and `launch.json` the
  matching **Debug F3 (J-Link)** / **Debug F3 (ST-Link)** configurations
  (device `STM32F303VC`, `CMSIS/device/STM32F303.svd`, `target/stm32f3x.cfg`,
  `preLaunchTask Build F3 (debug)`), so the README's VSCode section and the
  actual workspace both list F1/F0/F3/G0/F4.

### Fixed

- **F4 feed tail unified with the shared core order.** `ow_port_feed()` on F4
  programmed `DIER=CC2DE` and `CCR3=cmd[0]` last (after DMA arming), while the
  shared core programs them first; the late order guarded against a stale-CC2
  request clobbering slot 1 that was never reproduced. The common order is used
  on F4 silicon too. One less ordering divergence between the backends.

- **F4 port behaviorally aligned with the shared core (status).** The F4
  backend now matches the shared TIM1/DMA core in observable behavior: the
  `T1.CR1` capture fix, the PA11/test-gap instrumentation removal above, the
  common feed order, and the shared `(void)T1.SR` APB flush. Verified by the
  host suite on all five backends with `-DOW_PARASITE_POWER=1`. This is
  behavioral alignment only: the code-level collapse (shared DMA accessors,
  `feed`/`write_then_read` carve-outs) is completed below, and
  the `2_device_search` stall under sequential per-device converts is a
  parasite-power limitation, not a driver defect.

- **F4 port collapsed onto the shared core (code-level).** `ow_port_f4.h` is
  now a thin shim — family facts (clocks, pin mux, DMA stream assignment),
  the `OW_PORT_DMA_*`/`OW_PORT_ROUTE_*`/`OW_PORT_ENABLE_BUS_CLOCKS`/
  `OW_PORT_CONFIG_BUS_PIN` overrides, the stream `ow_port_dma_rearm()`, and
  the `write_then_read` carve-out (UG placement + marker hook,
  `OW_PORT_OWN_WRITE_THEN_READ`) — over `port/common/ow_port_tim_dma.h`,
  which carries all 17 bodies with DMA1 defaults.  The unification vocabulary
  (`OW_PORT_DMA_CR_RX16/RX8/TX`, `DISABLE_*`, `PROG_*`, `OW_PORT_DMA_EN_BIT`)
  is pinned per target by `test_dma_cr_value_macros`; the per-operation
  register table still asserts exact CR/CPAR/CMAR/CNDTR words. Verified by
  the host suite on all five backends, all-target firmware builds, and the
  DMA1 codegen triage.
- **Removed test instrumentation from production paths.** The F4 backend
  unconditionally toggled a PA11 logic-analyzer marker on every merged search
  operation and seized PA11 as GPIO output in `ow_port_init()` — test scaffolding
  shipped in the library with no flag. The merged pass now goes through the
  existing opt-in `OW_PORT_MARKER_TOGGLE()` hook (no-op by default), PA11 is
  left untouched for the application, and the `onewire_test_set_gap_us()` RTOS-gap experiment
  (hook, `ONEWIRE_SEARCH_GAP` phase, setters, its test and the `3_round_robin`
  gap-sweep alternate main loop) is removed end to end.
- **Mid-exchange bus disturbance verified harmless (rig removed).** Pressing
  the 1-Wire data line to GND for a whole scratchpad READ corrupts exactly
  that one read: the driver reports a single `CRC check failed` and resumes
  normal temperature readings on the very next cycle. No hang, no permanent
  `no sensor`. The bench rig (`OW_PORT_BENCH_MIDEX`)
  is removed; no production code change was needed.
- **Disturbed command write yields one stale-but-valid reading (rig removed).**
  Pressing the data line to GND for a whole command WRITE corrupts the command,
  so no conversion starts; the following READ then returns the untouched
  power-on-default scratchpad (85.0 C) with a valid CRC. No error is reported,
  no hang, normal readings resume on the next cycle. This is a protocol-level
  limitation, not a driver defect — a consistent-but-stale scratchpad is
  indistinguishable from a fresh one — so applications that must never act on
  a stale value should treat a lone 85.0 C as suspect. Rig
  (`OW_PORT_BENCH_WEX`) removed; no production code change was needed.
- **F4 capture path now starts the timer via `T1.CR1`, not `T1.CCR1`.**
  `ow_port_capture()` in `port/stm32f4/ow_port_f4.h` wrote the `OPM|CEN` start
  value into `CCR1` (a compare register) instead of `CR1` (the control
  register), so the capture timer never started: the DMA stayed armed with
  `NDTR` pending, no update event ever fired, and `ow_port_bus_done()` never
  returned. Every other capture/write site already used `T1.CR1`; only the
  capture path had the typo. Found by reading the timer registers on the wire
  (`CR1=0` while `CCR1=0x9`) and confirmed by the search hanging in
  `onewire_bus_done()` with the bus idle.
- **DMA disarm now waits for the EN bit to actually retire before
  reprogramming PAR/M0AR/NDTR.** `ow_port_dma_rearm()` (F4) and the new
  `ow_port_dma_disable()` (shared F0/F1/F3/G0 core, all five disarm sites)
  write `CR/CCR = 0` and, if the EN readback is still set, spin (bounded)
  until it clears. Reason: a no-presence reset arms `OW_PORT_CAPTURE_BUF_SIZE
  = 2` capture transfers but only the master-release edge arrives, and both
  DMA controllers clear EN only at memory-counter drain — so an absent reset
  leaves the capture channel armed mid-transfer (`NDTR/CNDTR = 1`, EN set)
  when the timer operation completes. The historical bare `CR/CCR = 0`
  then reprogrammed PAR/M0AR/NDTR while EN was latched, outside the
  RM0008/RM0091/RM0090 "Programming the DMA" flow. On every healthy path the
  previous transfer drained, so EN is already 0 and the wait is zero
  iterations.

  This is **not** a re-introduction of the blind drain-wait removed below: that
  one ran inside every `ow_port_update_event()` and spun unconditionally, and
  was removed once the stale-UIF race was diagnosed. This is a
  disable-acknowledge — it runs only when the disable write itself did not
  already retire EN (the genuinely-stuck path). It is also deliberately *not*
  covered by the byte-identical `.bin` gate used for the family-core
  deduplication: an acknowledgement loop on the unhealthy path is a real code
  change, so the `.bin` of every family differs from the previous baseline by
  design — the gate here is the host tests plus the real-hardware bench,
  not binary diffing.

- **README documented the F3 DMA pair one channel too low.** The
  Supported-families table said CC2→DMA1 **ch2** / CH4→DMA1 **ch3** — the
  intermediate-draft reading of RM0316 Table 78 that the silicon disproved.
  The backend itself was always correct (`ow_port_f3.h` uses **ch3/ch4**, the
  F0/F1 pair, bench-verified on MB1035B, D13/D14); the prose has been
  corrected to match, and the F3 bullet in the backends list and the F3
  Hardware-Verified row now carry the measured pair.

- **`EXT` (the `-D...` user flags) is now folded into the object-name stamp.**
  The stamp tracked the target, chip and clock knobs but not `EXT`, so
  rebuilding one target with a different `-D` flag silently reused the previous
  build's objects — it compiled clean, linked, and was wrong on hardware only,
  the exact failure the stamp comment warns about. Measured while sweeping the
  OC3PE knob on the F446RET6 bench: a "OC3PE=1" rebuild after an OC3PE=0 flash
  still shipped the OC3PE=0 machine code. The Makefile now appends a sanitised
  `EXT` to `OBJ_STAMP` so each flag set gets its own objects.

- **Parasite strong pull-up now drives HIGH (was LOW).** `ow_port_strong_pullup(1)`
  switched OTYPER to push-pull but left CCR3 holding the last slot's pulse
  value; with the counter stopped at CNT=0, PWM mode 2 evaluates CNT < CCR3 as
  INACTIVE, so the bus was driven LOW through the whole conversion window
  instead of HIGH, starving parasite sensors (scratchpads latched 0x07FE).
  The fix zeroes CCR3 and loads it via UG (OC3PE buffers the write) on engage,
  so CNT=0 holds the output ACTIVE=HIGH; release restores open-drain and the
  line floats HIGH. Applies to the shared F0/F1/G0/F3 core and to F4, which
  had the same shape. Bench: parasite conversions now complete with valid
  CRCs and room temperatures.

- **No DMA wait-loop: the never-needed bounded drain wait was removed after
  the stale-UIF race was diagnosed.** A first attempt at the 72MHz
  back-to-back multi-device failures (stale scratchpads reading 127.9°C
  while spaced single-device reads worked — `ow_port_bus_done()` reported
  done on the timer update flag while the DMA was still moving the trailing
  sample, and the next operation reprogrammed DMA within ~1us, aborting it)
  was a bounded DMA completion wait: spin, in `ow_port_update_event()`
  before any DMA reprogram, until each channel's EN clears or its (C)NDTR
  drains (~1000 iterations; mock flows with UIF and no armed DMA fall
  through immediately). The real root cause turned out to be a stale-UIF
  race — re-arming the timer with `EGR=UG` raises UIF a few timer cycles
  later on a fast core, so `SR=0` right after `EGR=UG` can lose the clear,
  and the next `ow_port_bus_done()` reports the operation complete before it
  started — and the true fix is flushing the posted APB write into the timer
  domain before `SR=0`: a dummy `T1.SR` readback (both backends now — the
  shared core's `__DSB()` survives only in `ow_port_kick()`). The
  *update_event* drain wait therefore serves no purpose and is gone from
  both the shared core and F4 ("UIF-only `ow_port_bus_done()` is correct");
  the full seven-example matrix (7 × 8/64/72MHz, all PASS) above validates
  its removal on real hardware. What remains, deliberately, is a bounded EN
  handshake in the DMA disable path (`ow_port_dma_disable()` / F4 rearm):
  a stream's EN clears only at the end of a normal-mode transfer, so `CR=0`
  merely requests the disable and a `guard = 1000u` spin acknowledges it —
  zero iterations on every healthy path (previous transfer drained, EN
  already 0), so the steady state still never waits.

- **A 180MHz clock for the F446, with the over-drive sequence it requires.**
  This is a separate `app.c` branch, not a divisor away from the 168MHz one: the
  PWR over-drive is enabled and waited on (`ODEN`→`ODRDY`, then
  `ODSWEN`→`ODSWRDY`), the flash latency is set to 5 wait states and the APB
  dividers to `/4` and `/2` before SYSCLK is switched. The APB2 `/2` still
  doubles the timer clock to 2 × 90 = 180MHz, so the 1µs-tick invariant
  (`PSC = SYSCLK_MHZ - 1`) and the bit-slot constants are unchanged, and the
  existing over-drive wait bounds report a stuck `ODRDY` through the same
  clock-failure path as an unlocked PLL instead of hanging. The layout follows
  ST's own `RCC_ClockConfig` example for this part (8MHz HSE, M=8 N=360 P=2).
  The F446 at 16MHz (raw HSI) and at the raw crystal frequency build from the
  same part file.

- **`OW_PORT_F4_MAX_SYSCLK_MHZ`, the per-part F4 ceiling, and a build-time
  rejection above it.** 180MHz is a real frequency on an F446 and out of spec on
  an F407, and the two differ by over-drive, flash latency and APB limits rather
  than by a scale factor, so a build asking for 180MHz on an F407 cannot be
  rescued by turning a divider down. `SYSCLK_MHZ=180 OW_CHIP=f407xx` is now a
  compile error naming the ceiling instead of firmware that runs out of spec.

- **A documented IC4F tier above 168MHz** (`fDTS/16, N=6`, ≈533ns at 180MHz).
  The existing >72MHz tier is `fDTS/8, N=6`, which lands at 286ns at 168MHz —
  it was chosen for 84MHz (571ns) and undershoots its own ~500ns target as the
  clock rises, because the filter's fDTS divisor is part of the encoding. 180MHz
  is the first clock that needs a row of its own. The full ICxF encoding table
  is now written out in `ow_port.h` with the source it was read from (ST's own
  `stm32f0xx_ll_tim.h`), because a wrong value here still configures a
  plausible-looking filter and reports no error: 0b1111, the intuitive "turn on
  every IC4F bit", is `fDTS/32 N=8` — 1422ns, not the 533ns of the intended
  `fDTS/16 N=6`.

- **`-DOW_UART_USART1_PA9` moves the F4 console off PB6.** The F4 console pin
  was hardcoded to USART1/TX on PB6 because that is where the STM32F4DISCOVERY
  routes its ST-LINK virtual COM port — a board-specific choice presented as a
  family default. On a WeAct F446 the VCP is on PA9, so PB6 reaches nothing and
  the console is silent: no output at all, and notably no clock-failure banner
  either, since that is written to the same missing pin. The default is
  unchanged; PA9 is safe alongside the bus because the backend uses PA10 for
  TIM1_CH3 and leaves PA11 untouched, and the console is TX only.

- **Host coverage of the 180MHz path.** The F4 host suite now also builds at
  180MHz (`ds18b20_test_f4_180mhz.exe`) as part of `make test`, because the
  over-drive branch exists in no other configuration and could not otherwise be
  tested at all. `test_timing.c` asserts the PWR clock enable, both halves of the
  over-drive sequence, the full `FLASH->ACR` and PLLM/PLLN at that clock, and
  the mutation of skipping `ODSWEN` was confirmed to fail it. The F4 mock gained
  the four over-drive bits, and `check_mock_headers.sh` gained a second pass
  against `stm32f446xx.h` (with a `<name>_Msk` lookup, since CMSIS carries the
  value on the `_Msk` row) so those constants are checked against a real ST
  definition instead of sitting in the skipped bucket — 57 of the 65 F4 mock
  macros are now compared, up from 19.

- **Changing only `SYSCLK_MHZ` or `HSE_MHZ` reused the previous build's
  objects.** The per-object stamp named the app, the family and the part, but
  not the clock, so building the 180MHz, raw-HSI 16MHz and raw-HSE 8MHz
  variants of one part in sequence compiled the first one and linked it into
  the other two — each silently carrying the first build's prescaler, bit-slot
  timings and console divisor. It compiled clean and was wrong on hardware only.
  Both knobs now reach the stamp, which is what makes the three F446 clocks
  three real builds.

- **`check_mock_headers.sh` could not see a constant whose CMSIS spelling
  carries the value on the `<name>_Msk` row**, which is the usual spelling for a
  bit field. A mock using the short name the drivers themselves use was skipped
  against every real header. Opt-in via the existing per-call argument, so the
  four other families are unaffected; the F446 pass uses it. The skip list is
  also now printable with `CHECK_MOCK_VERBOSE=1`, because a count alone does not
  say which constants are going unchecked.

- **`check_required_components()` was missing from the installed package
  config,** so `find_package(stm32_async_1wire COMPONENTS bogus REQUIRED)
  reported success instead of rejecting a component the package does not
  have. The package is a single whole-library component.

- **Editing a header did not rebuild the host test executables.** The firmware
  build tracks header dependencies through `-MMD` depfiles, but the host build
  is a single `gcc` call producing one `.exe`, and the headers were not listed
  as prerequisites at all. This is not a convenience issue: it produced a false
  pass. Changing the 180MHz IC4F tier in `inc/ow_port.h` and re-running
  `make test` reported success, because the binary was stale; only after
  deleting it did the change fail. The headers the suites compile against are
  now prerequisites of every host test target.

- **A 180MHz F4 build programmed the console UART for twice its real bus
  clock.** The console divisor was a per-clock ternary chain
  (`168 ? 42 : 84 ? 42 : SYSCLK`) with no 180MHz case, so the F446 fell through
  to the raw-HSI default and sized `BRR` for PCLK2 = 180MHz when the part runs
  45/90MHz. The result was a UART that worked perfectly at ~57600 baud on a
  clean build — a symptom that reads as a bad USB cable, not as a wrong clock.
  The APB prescalers are now defined once in `app.h` and both the clock
  configuration and the console divisor derive from them, so a clock cannot be
  added to one without the other. Two structural notes come with it: the
  register write itself is inside `hardware_init()`, which is
  `#if !defined(DS18B20_TEST_HARNESS)`, so **no host build on any family has
  ever compiled it** — which is why the arithmetic moved into a header the
  harness can reach, and why the test pins the expected divisor value rather
  than the register; and the optional `OW_UART_USART3` path is still built by
  no job on any family, which is now at least compile-checked for the F446.

### Changed

- **The OC3 output-compare preload bit (`OC3PE`) is now a sweepable knob.**
  The write/capture paths that rely on it — `ow_port_capture`, `ow_port_read_pair`
  and the single-slot branch of `ow_port_write_slots` — OR `OW_PORT_OC3PE_ARGS`
  into their `TIM_CCMR2` mask. The bit defaults on, so the machine code is
  unchanged and the hardware bus release at the terminal update event still
  works exactly as before; the DMA-fed paths (`ow_port_feed`, merged
  `ow_port_write_then_read`) keep OC3PE off. A bench can now flip it with
  `-DOW_PORT_OC3PE=0/-DOW_PORT_OC3PE=1` to measure how much the preload matters.
  Defined in `ow_port_tim_dma.h` (F0/F1/F3/G0) and `ow_port_f4.h` (F4), which
  `ow_port.h` documents.

- **F4 busy LED is now board-selectable: `-DOW_F4_LED_PB2`.** The default stays
  LD4 green on PD12 (STM32F4DISCOVERY, active high). The bench WeAct F446RET6
  puts its LED on PB2 instead — the "B2" silk is right under the part — so a
  build for that board passes `-DOW_F4_LED_PB2` and the app configures PB2 as
  push-pull output (no extra port clock: GPIOB is already on for the PB6
  console). Polarity is active high on both boards (PB2 per the WeAct
  schematic; PC13 on that board is the user button, not an LED). Mirrors the
  `-DOW_UART_USART1_PA9` board-knob pattern.

- **`port/common/ow_port_tim_dma.h` no longer claims its two DMA channels are
  `D13`/`D14` on every family that uses it.** That was true of F0, F1 and G0 and
  is now false of F3, where the CC2 feed is channel 2 and the CH4 capture channel
  3. The header now says the channel pair is per-family and must be read from
  that backend's assignment against its reference manual — which is the one thing
  in the interface that is genuinely not interchangeable, and the one whose
  mistake is silent. The shared core's **code** is unchanged; F3 plugs into the
  same sixteen functions without a line being added to them.

- **The F4 clock-failure flag and its bounded-wait helper are no longer
  F4-prefixed**, since F3 needs the same two: `ow_f4_clock_ok` and
  `ow_f4_wait_flag` are `ow_clock_ok` and `ow_wait_flag`, guarded by
  `OW_PORT_FAMILY_F4 || OW_PORT_FAMILY_F3`. The helper is still excluded for the
  clock that configures nothing — 16MHz on F4, 8MHz on F3 — because
  `-Werror=unused-function` is the only thing that notices. The failure banner
  gained an F3 variant, because the fix is not the same one: on F3 the
  unreachability is structural (no HSI16, PLLMUL ≤ 16), so the message names the
  64MHz internal ceiling and the ST-LINK's MCO signal rather than `HSE_MHZ`.

- **`src/ow_stats.c` gained an F3 branch in its device-header dispatch.** It had
  its own copy of the per-family `#include` chain, separate from the one in
  `app.c`, and fell through to `stm32f1xx.h` for anything unrecognised. That is
  why only `6_statistics` failed to build: it is the only example that compiles
  `ow_stats.c` with `OW_STATS_ENABLE`. Worth recording because the failure mode
  is a confusing `#error` out of an unrelated family header, and there are three
  more copies of the same chain in the tree.

- **The format job's pinned `clang-format` moves from 18.1.8 to 23.1.1.** The
  pin itself stays — LLVM changes formatting between releases, so the check has
  to name an exact version to be reproducible at all. What changed is which
  version: 18.1.8 was current when the pin was added and is now several
  releases behind, and it is not the version contributors actually have. A
  distribution LLVM is 23.x, so a contributor following `CONTRIBUTING.md` and
  running the local `clang-format` would hit disagreements the pinned job never
  sees. That is not hypothetical: on the F446 work, 18.1.8 reported a
  formatting violation in `examples/app/app.c` that 23.1.1 accepted, on the same
  file in the same tree.

  No reformatting comes with it. Both versions were run over all 87 C and header
  files in the tree and produce byte-identical output, and consider the same 83
  clean — so this is a version bump, not a style change, and the history shows
  that rather than asserting it. `CONTRIBUTING.md` still names no version, which
  is now the remaining gap: it tells a contributor to run `clang-format` without
  saying which one CI will hold them to.

- **`find_package` documented as the second supported integration path,**
  with the two consequences of consuming a fixed install tree made
  explicit: the consumer supplies the CMSIS include paths, and the package
  carries the part selection it was built with.

## [2.0.0] - 2026-09-26

416 commits since v1.8.1, and four breaking changes on the public surface:
the write API is byte-oriented, the parasite knob is unified, the statistics
period is counted in sweeps rather than measurements, and the F401 part is
selected as `f401xc`. The F4 backend and the F401CC are new and validated on
hardware; the build system gained a per-part matrix and a separate knob for
the board's crystal frequency.

### Added

- **The CMake build can now select the same firmware variants as the
  Makefile.** `-DOW_EXTRA_DEFINES` is the counterpart of the Makefile's
  `EXT="..."` and applies to every library and example, so
  `-DOW_PARASITE_POWER=1` and `-DOW_PORT_LOW_POWER=1` no longer require a
  dedicated option each. The `cmake` CI job builds all seven examples with
  both defines set, which is the only coverage the CMake build had of the
  WFE path at all. Each example also emits a `.hex` and a `.bin` next to
  the executable, so the `make all` artefact set no longer needs a manual
  objcopy. `CMAKE_BUILD_TYPE` defaults to `Release` on a single-config
  generator, where CMake would otherwise add no optimisation flags at all.

- **CMake uses the Makefile's optimisation profiles instead of its own.**
  `-Os` was hardcoded into two targets, so `CMAKE_BUILD_TYPE=Debug` silently
  produced a release binary and `-flto` was missing from every CMake build.
  The profiles are now `Release = -Os -flto -g0` and
  `Debug = -Og -g3 -gdwarf`, matching `make` and `make debug`; CMake's
  default `-O3 -DNDEBUG` is not used, because `-O3` is not what the hardware
  numbers were taken with and `NDEBUG` is a separate mode here. `-flto` is
  dropped on Cortex-M0/M0+ as in the Makefile, and when it is on the build
  switches the archiver to `arm-none-eabi-gcc-ar`: with a plain `ar` the
  LTO symbols are missing from the archive index and the link fails with
  `undefined reference to ds18b20_init`. An F4 `1_basic` built this way is
  2896 bytes of text against 2904 from the Makefile - the same firmware to
  within 0.3%.

- **A per-part build matrix in `chips/<part>.mk`.** `OW_TARGET` selects the
  family (unchanged); the new `OW_CHIP` selects the part *within* it by naming
  a file: `f103xb`, `f030x6`, `g031xx`, `f407xx`, `f401xc`, `f401xe`. Each file
  holds only the part identity — CMSIS device macro, device header, startup
  file, linker script, J-Flash and Ozone projects, SVD and default clock — while
  everything a family shares (MCU flags, the port header, the timer/DMA/pin
  mapping) stays in the Makefile. The split is drawn where the real difference
  is: `ow_port_f4.h` has no per-part conditional at all, because TIM1/DMA2 and
  `CHSEL=6` map identically across the F4 parts, so a subdirectory per part
  would have held no code. Both build systems read the same file, so the
  Makefile and CMake cannot drift on the device macro, startup, linker script
  or clock; CMake gains `-DOW_CHIP=` and `-DOW_SYSCLK_MHZ=` to match.
  `make test-chips` (hooked into `make test-clocks`) verifies that every part's
  repository-side files exist and that an unknown family or part is rejected.

- **STM32F401xE support** (`OW_CHIP=f401xe`). The `xE` parts (F401CD/RD/VD/CE/
  RE/VE) are the same core as the xC parts with 512KB flash / 128KB RAM instead
  of 256KB / 64KB, so they share the port backend, the 84MHz clock default and
  the app.c clock branch and differ only in the memory map: a new
  `STM32F401RE_FLASH.ld` linker script, the `stm32f401xe.h` /
  `startup_stm32f401xe.s` CMSIS layer, and J-Flash / Ozone projects. The linker
  map confirms 0x80000 flash for xE against 0x40000 for xC. **Not validated on
  hardware** — no F401xE part was available.

  This also closes a silent mismatch: `inc/onewire.h` has always honoured the
  `STM32F401xE` device macro, but the build could only produce `STM32F401xC`,
  so a hand-rolled `-DSTM32F401xE` linked against the xC script's 256K/64K
  memory map without a diagnostic. The header now promises a part that exists.

- **Interrupt-free polled time base for the examples (`examples/app`).**
  `app_time_init()` (called from `app_init()`) programs SysTick at 1 kHz
  *without* `TICKINT`, and `app_millis()` reads the `COUNTFLAG` bit and folds
  it into a counter. There is no `SysTick_Handler`, no NVIC bit and no
  `NVIC_EnableIRQ()` call anywhere in the firmware: timekeeping costs one
  register read and never waits, and the only rule is that the main loop runs
  at least once per millisecond — which the *pauses between* measurement
  cycles satisfy by construction, since each example counts its pause from
  the result delivered in `ds18b20_complete()`.

- **`ds18b20_start_measure()`: explicit, one-shot measurement cycles.** The
  driver now measures only when asked: one call requests exactly one
  `Convert T` + scratchpad-read cycle, the result arrives through
  `ds18b20_complete()`, and the driver then parks at `DS18B20_ST_IDLE` until
  the next request. The measurement cadence, the retry policy and any idle
  interval are therefore entirely the application's decision. The call is
  idempotent and ignored while a measurement cycle, a device search, a
  resolution change or a command transaction owns the bus. The 1-Wire layer
  exposes the underlying `onewire_kick()`.

- **STM32F4 backend (`port/stm32f4/ow_port_f4.h`): chip-generic across the
  STM32F407VGT6 (STM32F4DISCOVERY, MB997C) and the STM32F401CC family.** The
  1-Wire bus runs on PA10 (TIM1_CH3 PWM / CH4 indirect capture, AF1) with the
  feed and capture on DMA2 streams 2/4 (16-bit direct-mode feed, zero-copy
  from the family-sized `ow_pulse_t`); the console rides USART1 TX remapped
  to PB6 (AF7). The same header drives both parts — TIM1/DMA2/CHSEL=6 map
  identically (RM0090 / RM0368) — with the chip selecting the CMSIS device
  layer, linker script and clock default: `make OW_TARGET=f4` (F407, 168MHz
  HSE+PLL default) vs `make OW_TARGET=f4 OW_CHIP=f401xc` (F401CC, 84MHz
  HSE+PLL default; 256KB flash / 64KB RAM, `STM32F401CC_FLASH.ld`; 84MHz
  clock-config branch in `app.c` and an 84MHz F4 library clock default). The
  host suite runs the whole test matrix (default, low-power, NDEBUG,
  active-drive) against an F4 register mock, plus a dedicated
  F4/F401-family fallback compile check.

- **Configurable 1-Wire bus-pin drive strength (`OW_BUS_DRIVE`).** The bus pin
  (PA10) pad speed / drive strength is now selectable at build time
  (`-DOW_BUS_DRIVE=0..3`: WEAK/MEDIUM/STRONG/MAX, default MAX) on every backend
  — `OSPEEDR` on F0/F4/G0, CRH `MODE` bits on F1. A stronger pad sources more
  current into the bus capacitance, which matters for the parasite strong
  pull-up; `inc/ow_config.h` documents the trade-off (EMI / power).

- **`inc/ow_config.h` — central compile-time configuration header.**
  All genuinely tunable build constants are now collected in a single
  file: bit-slot timing (`ONEWIRE_ONE_PULSE`, `ONEWIRE_ZERO_PULSE`,
  `ONEWIRE_GUARD_BAND`, `ONEWIRE_SHORT_PULSE_MAX`), parasite bus
  timing (`OW_PARASITE_POWER`), feature flags (`OW_PORT_LOW_POWER`,
  `OW_DRIVE_ACTIVE`, `OW_STATS_ENABLE`) and DS18B20 driver knobs
  (`DS18B20_MAX_DEVICES`, `DS18B20_CYCLE_PAUSE_US`).  Every macro
  carries a `#ifndef` guard so existing `-D` overrides keep working;
  the header is the new single source of truth for defaults and
  hardware-validated documentation.  Protocol-inherent values
  (`ONEWIRE_MAX_SLOTS`, `DS18B20_RES_MIN/MAX/DEFAULT`) and the
  per-family system-clock default remain in their respective headers.
  The header is added to `library.json` headers and
  `library.properties` includes.

- **The scheduling API now reports rejected parameter ranges instead of
  silently dropping them.** `onewire_write_slots()` and
  `onewire_read_data()` return `uint8_t`: 1 if the operation was
  scheduled, 0 if the size argument is out of range and nothing was
  started. `onewire_write_bit()` gains the same return type (it
  always returns 1 because the single-slot input is always valid, but
  propagates the status for API consistency). Debug builds still
  trap on the reject path via `assert`; with `NDEBUG` the caller
  receives 0 instead of a silent no-op — so an invalid size can never
  turn into an undiscovered `onewire_bus_done()` hang. The three
  underlying port-layer functions (`ow_port_feed`,
  `ow_port_write_slots`, `ow_port_read_data`) follow the same
  contract on all four backends (F0/F1/G0/F4). Tested by a new
  `make test-ndebug*` build that compiles the suite with `-DNDEBUG`
  (see `tests/test/test_param_guard.c`).

### Changed

- **The bus pin's state is asked through three macros in `mock_target.h`**
  (`MOCK_PIN_IS_AF` / `_OD` / `_PP`) instead of each host test naming the CMSIS
  fields itself. The pin is PA10 on every family, but the fields that say
  "alternate function" and "open-drain" are spelled three ways - F1 uses the
  legacy `CRH` CNF10 field, G0's CMSIS drops the R, F0 and F4 use `MODER10` with
  `OTYPER` - and three test files each carried their own copy of that spelling.
  A fourth copy is part of why the mocks' wrong MODER10 field went unnoticed:
  the same expression spelled out in a test reads as deliberate, where the same
  wrong value inside a mock does not. The roles now sit beside the 18 existing
  `MOCK_*` macros, which is where the rest of the family knowledge already was.

- **`test_state_machine.c`'s four-family `ds18b20_init()` register check is gone,
  and with it 60 lines that repeated the setup contract.** It asserted clock
  gates, prescaler, BDTR, pin mode, output type, AF number and drive strength
  for each family separately, using bit tests where
  `test_port_init_contract.c` now asserts the same fields as exact register
  contents with the failing field named. What it did check that nothing else
  does is the *link* - that `ds18b20_init()` reaches `ow_port_init()` and changes
  nothing beyond it - so that is all it does now, by comparing the two snapshots
  directly instead of restating the expected values.

  One assertion genuinely moved rather than disappeared: F1's copy of the
  strong-pull-up helper also checked the CRH MODE10 drive-strength field, which
  the F0/G0/F4 copies never did. That asymmetry was unintentional, so the
  contract test now captures F1's MODE10 field as `pin_speed` and pins it like
  the other three families - the check is kept, and the file no longer implies
  the families assert the same thing when they did not.

- **The F0, F1 and G0 backends now share one TIM1/DMA1 core**
  (`port/common/ow_port_tim_dma.h`), leaving each backend with only what is
  genuinely its own. All three had the same sixteen `ow_port_*` functions in
  the same order - the same bus machine, written three times because each
  backend was done against its own reference manual and never merged.
  Measuring the three showed the code diverging in only nine places, and all
  of them are now five statement macros a backend defines: which clocks to
  gate, how the bus pin is put into alternate-function open-drain (F1 still
  uses the legacy `GPIO_CRH` field rather than `MODER`/`AFR`), how the pin
  toggles to push-pull, and the DMAMUX routing G0 needs. F4 is deliberately
  not included: same sixteen functions but a different DMA controller (DMA2
  streams with a CHSEL mux), two functions of its own, and an LA marker on
  PA11, so folding it in would mean conditionals on everything.

  There is no run-time cost - it all stays `__STATIC_FORCEINLINE` and every
  difference is resolved by the preprocessor. Checked rather than assumed:
  the `.bin` of all seven examples for F0, G0 and F1 is byte-identical before
  and after, which is what "moved, not rewritten" has to mean for a layer
  whose boards are not available to re-verify on.

- **`ow_port_f1.h` used `const uint8_t*` where the other three backends use
  `const ow_pulse_t*`.** Harmless as it stood - `ow_pulse_t` *is* `uint8_t`
  for F1 - but it hardcoded a family-specific width into four port
  signatures, so giving F1 16-bit slots the way F4 has them would have
  turned into a conflicting-type error far from the cause. The shared core
  has one signature, so the question no longer arises. Confirmed no effect on
  the generated code.

- **CMake artifacts no longer land in the repository root.** The `cmake` CI job
  kept its FetchContent clones in `_deps/` and installed into
  `prefix-<backend>/`; the install tree was only partly hidden, because the
  `*.a` rule caught the archive while the headers and the generated
  `find_package` config showed up as untracked. Both now live under the
  already-ignored `build/` (the cache path moved with them, so it still hits),
  `_deps/` and `/prefix*/` are in `.gitignore` for local runs that pass an
  explicit `--prefix`, `make clean` also removes any install prefix outside
  `build/`, and `make clean-deps` additionally drops the FetchContent clones.
  The FetchContent cache is deliberately not in `clean`: a rebuild should not
  pay for a re-clone.

- **Two build-system structures replaced by simpler ones.** The part-matrix
  check moved out of the Makefile into `tests/check_chips.sh` — reading three
  values out of a flat file and testing them against the filesystem needed a
  parse-time include, an `eval`'d conditional and a sub-make per part in make;
  the entry point is still `make test-chips`. The 37 CMSIS download rules are
  now generated from one table of `target | directory | URL`, so a target and
  its URL can no longer come apart — which is the failure that broke every
  dependency-fetching CI job when the per-family URL variables were added one at
  a time. Makefile: 994 → 888 lines.

- **An unknown `OW_TARGET` or `OW_CHIP` is a build error.** Both used to fall
  through to the F1 branch, so a typo produced a valid-looking Blue Pill
  binary. `OW_TARGET` never even had an explicit default — the empty value
  silently meant F1, which is how `OW_TARGET=f401-84` came to compile a
  Cortex-M3 image in a matrix that was supposed to be exercising F401 parts.
  `OW_TARGET` now defaults to `f1` explicitly, and both tokens are validated
  with an error that lists the valid values.

- **`OW_CHIP=f401` is now `OW_CHIP=f401xc`.** The old name is rejected with the
  list of known parts rather than silently aliased, following the CMSIS
  flash-density naming (`STM32F401xC`) the part files use. Scripts that passed
  `OW_CHIP=f401` must be updated.

- **`make download-deps` fetches only the selected part's files.** The
  `EXTERNAL_DEPS` list is now built from the part file, so an F4 build no longer
  downloads the xC and xE device headers, startup files and SVDs when building

- **The WFE sleep moved from the application into `ds18b20_poll()`.** With
  `-DOW_PORT_LOW_POWER=1` the driver now blocks in `__WFE()` itself while a
  long stage (conversion, scratchpad read, EEPROM hold-off) is in flight, so
  the sleep is invisible to application code: `7_low_power` no longer has a
  `low_power_poll()` helper, a `measure_in_flight` flag or any app-side
  blocking sleep, and its main loop is now identical to `2_device_search`.
  `ow_port_long_wait_pending()` and `ow_port_sleep_until_done()` remain
  available for callers that want to drive the sleep themselves. UIE is still
  used purely as a `WFE` wake-up event through `SEVONPEND` — no ISR, no NVIC
  interrupt.

- **No hidden measurement cycles: the driver starts nothing on its own.**
  Previously the driver re-armed itself through a timer update event after
  *every* finished operation — the 5 s inter-measurement pause, but also the
  completion of a device search, a resolution change or a command transaction,
  so a plain `Read ROM` or `Write Scratchpad` silently started a 750 ms
  conversion. All of these implicit starts are gone: after init and after every
  finished operation the timer stays idle until the application calls
  `ds18b20_start_measure()`. `ds18b20_poll()` at IDLE is a no-op, exactly like
  a poll with a cleared UIF.

- **The inter-measurement pause left the library.** `DS18B20_CYCLE_PAUSE_US`
  (default 5 s) and the internal `start_cycle_pause()` timer are removed from
  `inc/ow_config.h` and the driver; a finished cycle now simply parks. The
  examples keep their observable 5 s cadence as an application-side
  `MEASURE_PERIOD_MS` (see `examples/*/main.c`), and `6_statistics` — whose
  10 ms pause never mattered next to the conversion wait — now requests the
  next cycle as soon as its dump is done. Datasheet waits (conversion time,
  EEPROM hold-off) and the 1 ms scan-mode scheduling bridge are unchanged: they
  are part of a measurement, not of a service cadence.

- **STM32F4 console UART relocated to USART1 TX on PB6.** The `examples/app`
  F4 branch now routes the console through USART1 on the remapped **PB6** pin
  (AF7, push-pull) instead of the default PA9 pad, which has no
  USART1-to-ST-LINK route on the STM32F4DISCOVERY; the optional
  `-DOW_UART_USART3` (PB10) path remains available. The bus stays on the
  default PA10 pin (`OW_PORT_BUS_PE13` remains an option).

- **`src/ds18b20.c` was reorganized into four include-only functional
  modules.** The driver grew to 1459 lines with the search, transaction,
  resolution and measurement state machines sharing a single file, so the
  per-module private state (`dev_roms`/`dev_count`, `txn_ctx`/`detect_buf`,
  `res_ctx`, `conv_cmd`/`read_cmd`) was hard to follow. The file is now an
  amalgamated translation unit: it keeps the shared `ctx`/`txn_ctx` statics
  and `#include`s `src/ds18b20_search.c`, `src/ds18b20_txn.c`,
  `src/ds18b20_resolution.c` and `src/ds18b20_measure.c` in dependency order
  (each guarded by `DS18B20_DRIVER_BUILD`). The split is purely internal: the
  preprocessed source and the generated machine code are unchanged, public API
  and ABI are untouched. The Makefile test rules list the parts as
  prerequisites and CMake marks them `HEADER_FILE_ONLY` so they stay out of
  the compiled sources.

- **The split driver parts are now covered by CI on both axes.** The Code
  Quality `format` job lints `src/ds18b20_{search,txn,resolution,measure}.c`
  alongside `src/ds18b20.c`, and a new `cmake` job smoke-builds the library
  package (with `OW_BUILD_EXAMPLES=ON`) for F1, F0, G0 and F4 via the ARM
  toolchain and verifies the `find_package()` install tree — the CMake path
  previously had no in-CI coverage despite the root `CMakeLists.txt`.

- **Feature flags now use value-style (`#if X`) instead of presence
  (`#ifdef X`).** `OW_PORT_LOW_POWER`, `OW_DRIVE_ACTIVE` and
  `OW_STATS_ENABLE` must be passed as `=1` on the command line
  (e.g. `-DOW_PORT_LOW_POWER=1`); bare `-DOW_PORT_LOW_POWER` no longer
  compiles correctly.  All Makefile targets, fuzz rules and the CMake
  example block are updated accordingly.  The change is transparent for
  `make`/`make test`/`make fuzz-all` invocations — the shipped defaults
  and Makefile knobs already pass the right flags.

- **Unified compile-time parasite knob.** The dual naming between the
  driver guard-band default (`OW_TIMING_PARASITE`) and the example
  application flag (`PARASITE_POWER`) is replaced by a single
  `OW_PARASITE_POWER` value (0/1, default 0).  Passing
  `-DOW_PARASITE_POWER=1` now raises the default guard band to 100 µs
  *and* causes every example to call `ds18b20_set_parasite(1)` at
   startup.  The old flag names are removed; `-DPARASITE_POWER=1` no
   longer has any effect.

- **Public 1-Wire write API is now byte-oriented.** `onewire_write_slots()`
  and `onewire_encode_byte()` are replaced on the public surface by
  `onewire_write_command(const uint8_t *bytes, uint8_t nbytes)` and
  `onewire_write_command_byte(uint8_t byte)` (`inc/onewire.h`): the command
  bytes are encoded synchronously (MSB-first wire order, LSB-first bit
  order) into an internal pulse buffer, the trailing bus-release zero is
  appended there, and the transfer is scheduled in a single call. An empty
  command or one longer than `ONEWIRE_CMD_MAX_BYTES` (13) is rejected
  without starting a transfer. The slot-level encoder and the `ow_pulse_t`
  type move to the new internal header `inc/onewire_internal.h`, used only
  by the driver and the test harness. The DS18B20 driver
  (`addr_bytes`/`txn_ctx.bytes`/`res_ctx.bytes`), the test accessors and the
  DMA-contract/param-guard tests are updated accordingly; externally the
  encoded pulse stream and timing are unchanged.

- **STM32F4 backend is now covered by the host-test harness.** New
  `tests/mock/stm32f4xx.h` (TIM1/DMA2/GPIO/RCC/USART register model) plus an
  F4 dispatch in `tests/mock/mock_target.h` / `tests/mock/hw_model.c` let the
  entire suite run against the F4 backend, including the low-power WFE path.
  The F1/F4 register differences are hidden behind `MOCK_DMA_*` macros and a
  `DMA_SxCR`/`DMA_CCR` union alias. Pulse buffers are typed `ow_pulse_t`
  throughout, so the F4 16-bit feed width (direct mode, `MSIZE=16`) and the
  8-bit direct-mode capture width are exercised by the same tests. New
  Makefile targets `test-f4`, `test-lowpower-f4`, `test-ndebug-f4`,
  `test-active-f4` and `test-clocks-f4` mirror the per-family targets.

- **CMake gained the STM32F4 target.** `-DOW_TARGET=f4` selects the
  `cmsis_device_f4` headers (`STM32F407xx`, Cortex-M4), the
  `port/stm32f4/STM32F407VGT6_FLASH.ld` linker script, and ships
  `port/stm32f4/ow_port_f4.h` in the install set.

### Fixed

- **The F4 clock treated the crystal as if the part decided it.** The 84MHz branch
  hardcoded `PLLM = 8`, which is right for the F4DISCOVERY's 8MHz crystal and wrong
  for every F401 board with a different one - the WeAct F401 Black Pill carries 25MHz.
  With M=8 that puts 3.125MHz into a PLL input specified for 1-2MHz, the PLL never
  locks, and the unbounded wait on `PLLRDY` never ends. The board comes up completely
  silent, which is indistinguishable from a dead board and from a bus fault. Nothing
  caught it: the crystal appears nowhere in the build, no host test looks at the M
  field, and the F4 mock lacked the 84MHz registers (`RCC_CFGR_PPRE1_DIV2`,
  `FLASH_ACR_LATENCY_2WS`) so the 84MHz path did not even compile against it.

  The crystal is now a separate knob, `HSE_MHZ=N`, because it is the *board's*
  property while `SYSCLK_MHZ` is the application's. Aiming the PLL input at exactly
  1MHz makes both dividers fall out - `PLLM = HSE_MHZ`, `PLLN = 2 x SYSCLK` - so an
  8MHz crystal still produces the M=8/N=336 and M=8/N=168 the F407 was validated at,
  unchanged, and a 25MHz board reaches the F401's 84MHz cap with M=25/N=168.
  `chips/f401xc.mk` declares `CHIP_HSE_MHZ = 25` for the Black Pill, `chips/f401xe.mk`
  declares 8, the header default is 8, and the command line overrides both. A
  `_Static_assert` rejects an unreachable combination at build time.

  Two silent-failure paths came with it. `SYSCLK_MHZ=8` used to select "raw HSE"
  whatever the crystal was, so on a 25MHz board it compiled and ran with every 1-Wire
  timing scaled by 3.125 - the worst of the three F4 clock modes to reach for when
  lowering the frequency to be safe. It is now selected only when
  `SYSCLK_MHZ == HSE_MHZ`, and a mismatch is a build error. And the HSE/PLL waits are
  bounded, so a wrong `HSE_MHZ` now reports itself over the console and stops rather
  than hanging with no output. It deliberately does *not* fall back to the HSI: `PSC`,
  `SysTick` and the console divisor are compiled against the requested frequency, so
  continuing would scale every timing by an unknown factor.

  Regression cover, since this failed while every test was green: `test_timing` now
  asserts the programmed `PLLM` and `PLLN` for the active clock (it previously checked
  only the prescalers and `PSC`, which is why the M field could rot unnoticed),
  `test_sysclk_fallback` checks the F4 PLL arithmetic and the raw-HSE guard,
  `test_port_init_contract` derives its `psc` expectation from the configured clock
  instead of hardcoding the 168MHz value, the mock gained the 84MHz registers, and CI
  runs the whole F4 suite a second time at `-DOW_PORT_SYSCLK_MHZ=84 -DOW_HSE_MHZ=25`,
  the real board's numbers, since the matrix otherwise only ever built f4 at
  168MHz/8MHz. The 21 non-F4 firmware images are byte-for-byte unchanged.

- **The f0, g0 and f4 host mocks had pin 10's MODER bit field holding pin
  11's bits.** A test asserting that the bus pin is in alternate-function
  mode was therefore asserting about PA11, on three of the four backends.
  The suite stayed green throughout, because the mocks are what the tests
  check against: the driver is correct (right macro names, real values when
  compiled for hardware), it just had no power to catch a wrong-pin or
  wrong-mode regression. In f4 the two triples were swapped, which was worse
  - that backend drives PA10 (bus) and PA11 (LA marker), so the two uses
  masked each other. g0 also had pin 4's field holding pin 5's bits, under the
  spelling `GPIO_MODER_MODE4` that the G0 CMSIS genuinely uses (F0 and F4
  spell it `MODER10`), which is why a check keyed on the F0/F4 spelling alone
  would have missed it. f1 was correct throughout: it configures the pin
  through the legacy CRH field.

  `tests/check_mock_headers.sh` (`make test-mocks`, and part of
  `make test-clocks`) now compares every literal-valued macro in each mock
  against the same macro in the real CMSIS header, using the value CMSIS
  carries in its trailing `/*!< 0x... */` comment. It fails if a family ends
  up comparing nothing, so it cannot quietly become vacuous, and it reports
  the macros it could not compare rather than ignoring them. Confirmed to
  have teeth by reverting a corrected value and watching it fail.

- **`6_statistics` counted its dump period in measurements while calling the
  knob `STATS_DUMP_INTERVAL`, and shipped a value that put the first dump about
  an hour out.** Both build systems forced `-DSTATS_DUMP_INTERVAL=5000`, and a
  measured device takes ~0.79 s per measurement, so the example spent an hour
  printing measurements and nothing else - it looks broken, and the plausible
  reading of the name (5000 ms, as with every other interval constant in the
  project, which carries its unit) is not what the code did. `override EXT +=`
  appended the define after any value passed as `EXT=`, and the last define on
  the command line wins, so the source default of 100 was unreachable and the
  period could not be overridden at all.

  The period is now `STATS_DUMP_SWEEPS` and is counted in sweeps - one pass
  over every device - defaulting to 10, which on a 7-sensor parasite bus is 70
  measurements and about a minute. The count sits outside the `found_count > 1`
  branch, because the round-robin block never runs with a single device:
  counting the sweep where the `---` marker is printed would have meant no
  batch ever completed on a one-sensor bus, which the old measurement-based
  period did deliver. Both build systems now inject `OW_STATS_ENABLE` and
  nothing else, so the period is defined once, in the example's source, and is
  reachable and overridable. `ow_stats_tick()` still runs per measurement, so
  the `t=` total in the dump keeps its meaning, and the example prints the
  period and the expected sample count at startup so the wait is checkable
  against that total.

- **Object files were not named after the selected part, so building one
  family or part after another silently reused the previous one's objects.**
  Object names carried only the app name (`build/1_basic_onewire.o`) to keep
  `APP=` invocations from sharing `app.o`, but nothing distinguished
  `OW_TARGET=` or `OW_CHIP=`. Only the startup and system files escaped this,
  because their basenames differ per part by nature; everything actually
  shared - `onewire.c`, `ds18b20.c`, `main.c`, `app.c`, `ow_stats.c`,
  `syscall.c` - collided across all six parts.

  Switching targets in one build tree is ordinary usage, and the result was
  wrong rather than refused. Building `OW_TARGET=f4 OW_CHIP=f401xc` after
  `f407xx` produced an image byte-identical to the f407xx one: the F401CC
  build was the F407 firmware, with the F407 168 MHz clock default and the
  F407 flash assumptions, and the user had no way to see it. Building `f0` or
  `g0` after `f1` failed outright with `ld: error: lto-wrapper failed` or an
  assembler `invalid constant after fixup`, which reads like a toolchain bug
  and points at the one freshly-named file rather than at the stale link it
  actually was.

  Object names now carry `$(OW_TARGET)_$(OW_CHIP)` as well. Verified by
  hashing: all six parts built in one tree without `clean` in between are
  byte-identical to the same six built from scratch, and repeated builds of
  one part are byte-identical to each other. No documentation required a
  `clean` between targets, and none is needed now.

- **`chips/<part>.mk` and `ow_port.h` each carried their own system clock
  default, and nothing checked that they agreed.** `onewire.h` picks a
  default per family under `#if !defined(OW_PORT_SYSCLK_MHZ)` - 72 for F1,
  48 for F0, 64 for G0, 84 for the F401 parts, 168 for F407 - and every part
  file separately declares `CHIP_SYSCLK_MHZ`, which the build passes as
  `-DOW_PORT_SYSCLK_MHZ=$(CHIP_SYSCLK_MHZ)`. That define is precisely the
  header's `!defined()` guard, so on a Make or CMake firmware the header
  cascade is bypassed entirely and the part file alone decides, while
  `test_sysclk_fallback.c` deliberately omits the define to check the
  cascade that only a PlatformIO or CubeMX build ever sees. Nothing connected
  the two. Setting `CHIP_SYSCLK_MHZ = 168` in `chips/f401xe.mk` compiled the
  firmware against a 168 MHz timer prescaler on a part that runs at 84 MHz -
  every 1-Wire slot half as long as intended, so the sensors would have read
  nonsense - and `make test` for all four families, `clock-ref-check` and
  `test-chips` all still passed. The build now passes the part file's value to
  the check as `OW_CHIP_SYSCLK_MHZ` and the check compares it against the
  header default, so the mutation fails the build for every family.

- **The `STM32F401xE` branch of the clock-default check was never compiled.**
  `test_sysclk_fallback.c` has handled `STM32F401xE` since the check was
  written, but only `f401xc` was ever built through it, so that branch was
  dead. Both F401 parts are now enumerated in one list (`F401_CLOCK_CHECKS`),
  which also means a future F401 variant cannot ship without a guard.

- **`ow_bits.h` was missing from the PlatformIO `headers` list.** Every
  `ow_port_<family>.h` includes it for the register-address macros, and CMake
  installs it, so it was public by every measure except `library.json` -
  which is what the IDE indexes for autocompletion. Consumers of the PlatformIO
  package got no completions for `D11`..`D17`, `A1`, `A2` and the rest. Its
  include guard was also still `OW_MACRO_H`, left over from the
  `macro.h` rename.

- **A cold-cache F4 build could not compile.** `core_cm4.h` includes
  `mpu_armv7.h` unconditionally - every Cortex-M4 has an MPU - but the F4
  dependency list never fetched it, so `make clean-deps && make OW_TARGET=f4`
  died with `core_cm4.h: fatal error: mpu_armv7.h: No such file or directory`
  on any machine that had not previously built G0 or F4. It went unnoticed
  because the CMSIS cache in CI is keyed on the Makefile and had been warm.
  `core_cm3.h` and `core_cm0.h` reach that header only behind `__MPU_PRESENT`,
  which the F1 and F0 parts do not define, so only the F4 list needed the entry.
  Verified by rebuilding f1, f0, g0, f4, f4/f401xc and f4/f401xe from an empty
  `CMSIS/`.

- **`make jprogram` had no J-Flash project for the STM32F401CC.** `JFLASH` was
  assigned for every port except the F401 branch, so the variable expanded to
  nothing and J-Link was invoked with an empty `-openprj`. Added
  `port/stm32f4/stm32f401cc.jflash` (device `ST STM32F401CC`, 256 KB flash
  range) and the matching `JFLASH` assignment. The ST-Link path
  (`make program`) was never affected. Also corrected `RAMSize` in
  `stm32f407vgt6.jflash`, which claimed 64 KB although the F407VGT6 has
  128 KB of SRAM.

- **The F4 backend had no SEGGER Ozone project.** `port/<mcu>/project.jdebug`
  existed for F1, F0 and G0 and the README pointed at that path generically,
  so F4 users had nothing to open. Added `project.jdebug` (F407VGT6) and
  `project-f401cc.jdebug`, plus the `STM32F401.svd` download the second one
  needs — the F4 `EXTERNAL_DEPS` list only fetched the F407 SVD.

- **The README documented a header that does not exist.** The file tree and
  the slot-level-primitives paragraph both referred to
  `inc/onewire_internal.h`; the private interface ended up in `inc/onewire.h`
  instead, which is where `ow_pulse_t` and `onewire_encode_byte()` are
  declared. This was the same stale name that broke the `format` CI job. The
  tree no longer lists the phantom file, and the paragraph now says the
  primitives live in `onewire.h` and are simply not needed by applications.
  The F407VGT6 entry also described 256 KB flash / 64 KB RAM instead of the
  1 MB / 128 KB the linker script actually sets.

- **The CMake build could not select the STM32F4 backend, although CI asked
  for it.** The `cmake` job matrix lists `f4`, but `CMakeLists.txt` only knew
  `f1`, `f0` and `g0`, so configuring with `-DOW_TARGET=f4` stopped at
  `FATAL_ERROR`. The F4 target is now wired up like the other three (CMSIS
  device repo and `STM32F407xx` define, `-mcpu=cortex-m4 -mthumb` mirroring the
  Makefile, `system_stm32f4xx.c`, `startup_stm32f407xx.s`, the
  `STM32F407VGT6_FLASH.ld` script), `port/stm32f4` is on the include path of
  both library targets, and `ow_port_f4.h` ships in the install tree so
  `ow_port.h`'s sibling quote-include resolves for installed consumers.
  Selecting the F401CC variant remains a Makefile-only knob
  (`OW_TARGET=f4 OW_CHIP=f401xc`).

- **The CMake install-tree check in CI asserted a file that does not exist.**
  The job verified `include/stm32-async-1wire/onewire_internal.h`, a leftover
  from before the edge→pulse rename; no such file exists anywhere in the tree,
  so the job could never pass. It now checks `ow_port_f4.h`, which is the file
  this backend actually adds to the installed headers.

- **`4_scan_mode` could report 85.0 °C for a sensor left at a different
  resolution.** Scan mode converts every sensor in parallel and waits once,
  assuming a uniform resolution, but the example never established one: a
  sensor left at 12-bit by a previous run would be read with the conversion
  wait derived from another device's (lower) resolution and return its 85.0 °C
  power-on-reset value before the conversion completed. The example now
  programs the resolution to every sensor with one broadcast Write Scratchpad
  (`ds18b20_set_resolution()`) before scanning. Diagnosed on hardware with a
  logic analyzer on the bus pin.

- **CMake install package was incomplete and the library did not compile in
  plain CMake builds.** The `stm32_async_1wire` target now gets the CMSIS and
  device include dirs (`BUILD_INTERFACE`-scoped so install exports stay
  clean), and install ships the backend headers and `ow_bits.h` alongside
  `ow_port.h`.

- **STM32F4 low-power build did not compile (`-DOW_PORT_LOW_POWER=1`).**
  The TIM1 update-IRQ mapping in `inc/onewire.h` tested the raw target macros
  (`OW_PORT_TARGET_F0`/`OW_PORT_TARGET_G0`) instead of `OW_PORT_FAMILY_*`, so
  an F4 build took the F1 branch and referenced the nonexistent `TIM1_UP_IRQn`
  (the F4 vector is `TIM1_UP_TIM10_IRQn`). The mapping now keys off
  `OW_PORT_FAMILY_*` and covers F0/G0/F1/F4, with a hard `#error` for any
  unhandled family.

- **`src/ow_stats.c` failed to build on STM32F4 with `OW_STATS_ENABLE=1`.**
  Its device-header chain handled G0/F0/F1 but fell through to `stm32f1xx.h`
  for F4; it now includes `stm32f4xx.h` under `OW_PORT_FAMILY_F4`.

- **Public write/read API limited to the 8-bit `TIM1.RCR` capacity.**
  `onewire_write_slots()` now accepts at most `ONEWIRE_MAX_SLOTS` (256) slots
  and `onewire_read_data()` at most `ONEWIRE_MAX_READ_BYTES` (32) bytes.
  Out-of-range values (including zero) are ignored, with an `assert()` raised
  in debug builds; internal buffers are guarded by `_Static_assert`, and the
  new `test_rcr_limits` host tests cover the hardware boundary.

## [1.8.1] - 2026-09-13

Restored from the commits in `v1.8.0..v1.8.1`: this release was tagged
without a section here, and its changes had drifted into the section below.

### Added

- **Doxygen configuration and CI-deployed API documentation.** `Doxyfile` is
  committed and the `api-docs` workflow publishes the generated docs to
  `gh-pages` on every push.
- **libFuzzer harnesses for the Search ROM and resolution state machines,**
  plus a device-table overflow case for a search that finds more ROMs than
  the scan table holds.


- **Formalised per-operation DMA register contract table in the host
  tests.** New `test_dma_contract` drives every scheduleable hardware operation
  (write, reset, read pair, read data, merged search write+read, Match-ROM
  config write, single-bit write) against one table of exact `RCR`, `CPAR`,
  `CMAR`, `CNDTR`, `MSIZE`/`DIR`/`MINC`, required DMA-enable bits and post-op
  transfer accounting — including the 8-bit `RCR` boundaries (write 256 slots /
  read 32 bytes → `RCR` 255). The feed log gained an uncapped total-transfer
  counter so exact transfer counts hold even beyond the 128-entry value log.

- **New temporal TIM/DMA event model in the host test harness.**
  `hw_run_until_uif()` fires the CC2 feed DMA once per slot *at the slot
  start* ("modeled at slot start for simplicity") — fine for the memory-side
  DMA contract, but it cannot prove the *temporal* contract. The new
  `hw_tim_step()` stepper in `tests/mock/hw_model.c` places every event at its
  physical counter position and the new `test_tim_model` tests prove that
  CCR3(slot N) stays in effect for the whole of slot N, the reload happens
  only after the CC2 compare (never at the slot start), the trailing
  bus-release zero is applied only after the last slot, and CC4 captures fire
  at the pulse-edge counter position.

- **New `TIMING=CUSTOM` compile-time preset** (Makefile; expands into
  `-DONEWIRE_ONE_PULSE=1 -DONEWIRE_ZERO_PULSE=60 -DONEWIRE_GUARD_BAND=1
  -DONEWIRE_SHORT_PULSE_MAX=15`). It uses the minimum slot timing allowed by
  the 1-Wire standard — `one` 1µs, `zero` 60µs, `guard` 1µs,
  `short≤` 15µs → 62µs slot. It is experimental: a 1µs read/write pulse is
  below the values validated on hardware (a 2µs pulse already broke slot
  decoding on an F030 at 8MHz) and is intended for electrically ideal setups
  only.

### Changed

- **The `edge` terminology was renamed to `pulse` throughout the 1-Wire layer.**
  `onewire_reset()`, `onewire_present()`, `onewire_read_pair()` and
  `onewire_pair_bits()` keep the same signatures - only the parameter and
  internal symbol names changed, so callers are unaffected.
- **Backend selection is unified behind a single `OW_PORT_FAMILY_*` token** for
  all four families, so the F0/F1/G0 headers no longer each guess the family
  independently.


- **Example applications restructured into numbered directories.**
  `src/demo*.c` became `examples/1_basic` … `examples/7_low_power`, with the
  shared platform layer moved to `examples/app/app.{c,h}` (`app_init()`,
  non-blocking UART TX ring buffer, busy-LED callback).

- **Timing preset selection moved to compile time.** The four timing values
  (one/zero/guard/short pulse) are never changed at runtime, so the `TIMING=`
  Makefile presets now expand directly into
  `-DONEWIRE_ONE_PULSE=… -DONEWIRE_ZERO_PULSE=… -DONEWIRE_GUARD_BAND=…
  -DONEWIRE_SHORT_PULSE_MAX=…`; `OW_TIMING_PARASITE` selects the wider 100µs
  guard-band default on parasite-powered buses.

- **`inc/macro.h` renamed to `inc/ow_bits.h`**; the newlib-nano syscall stubs
  moved to `src/syscall.c`; public version macros
  `STM32_ASYNC_1WIRE_VERSION_*` (`1.8.1`) and C++ guards added to the headers.

### Removed

- **Breaking:** the runtime timing-profile API is removed —
  `onewire_set_timing_profile()`, `onewire_get_timing_profile()`,
  `ow_set_parasite_guard()` and the `ONEWIRE_TIMING_PROFILE_DEFAULT` /
  `ONEWIRE_TIMING_*` runtime enums. Timings are compile-time defines only (see
  Changed), which is how they were always used on the target.
- `tests/fuzz/fuzz_timing` harness removed with the runtime profile API;
  Search ROM and resolution state-machine harnesses (`fuzz_search`,
  `fuzz_resolution`) added.

### Fixed

- **STM32G031 flash latency matched its own comment.** The configured wait
  states and the documented 2-wait-state requirement were out of step.
- **`src/ow_stats.c` no longer depends on `app.h`;** it includes `onewire.h`
  for the `OW_PORT_FAMILY_*` macros it actually needs, and the examples are
  clang-format clean.


- **CMake `OW_BUILD_EXAMPLES` referenced the old example set.** The list is
  renamed to the real directory names (`3_round_robin`, `4_scan_mode`,
  `5_commands`, `6_statistics` instead of `3_manual_read`, `4_nonblocking`,
  `5_interrupt_driven`, `6_crc_performance`) and each example now links the
  shared `examples/app/app.c` platform layer with the same per-example define
  set as `make APP=<ex>` (`UART_TX_BUF_SIZE`, and for `6_statistics`
  `OW_STATS_ENABLE`/`DS18B20_CYCLE_PAUSE_US`/`STATS_DUMP_INTERVAL`).

- **Search ROM could livelock on a hostile/broken bus under the family
  filter.** If every id/cmp pair keeps answering `00` (all devices disagree
  and pull low), the engine re-assembles a CRC-valid ROM whose family byte
  the filter rejects, so `found` never advances and `last_discrepancy` stays
  pinned — the walk would loop forever. `onewire_search_poll()` now detects a
  repeated leaf (the previous walk produced the identical ROM) and terminates
  the search instead. Found both by the `fuzz_search` harness in CI
  (`crash-99a30a39…`, seed `2971683640`) and locally; regression covered by
  `test_search_hostile_all_zero_bus_terminates`.

## [1.8.0] - 2026-09-01

### Added

- **Opt-in low-power path (`-DOW_PORT_LOW_POWER`, default off).** Enables the
  TIM1 update interrupt (UIE) and the `SEVONPEND` system-control bit so an
  application can block in `__WFE()` while a *long* 1-Wire stage (> 1 ms) is
  running and be woken by the timer's update event while the hardware
  completes the transaction.
  Long stages include the temperature conversion (up to 750 ms), the
  scratchpad read (~5 ms), an EEPROM hold-off (10 ms) and the inter-measurement
  pause. The driver itself stays fully non-blocking; no ISR is ever installed
  and `NVIC_EnableIRQ` is never called (the `SEVONPEND` mechanism wakes the
  core from a pending interrupt event without one). The NVIC pending bit is
  cleared in `ow_port_bus_done()` on every completion; the scan gap (1 ms) and
  all short stages stay outside the sleeping path. Implemented behind a single
  header macro in all three backends (F0/F1/G0) plus the two header helpers
  `ow_port_long_wait_pending()` / `ow_port_sleep_until_done()`.
- **New low-power example** (`examples/7_low_power/main.c`): the
  `2_device_search` search + sequential loop with WFE sleep on long stages,
  exposing the two helpers in its main loop. Power is not yet measured — the
  demo only establishes the mechanism. Build with `make OW_TARGET=g0
  APP=7_low_power EXT="-DOW_PORT_LOW_POWER"`.

### Changed

- Host-test mock headers now define `TIM_DIER_UIE` so the low-power code path
  also compiles on the host (harmless; not used by the default build).
- **New `make test-lowpower` host tests** (`tests/test/test_lowpower.c`, plus
  `test-lowpower`, `test-lowpower-f0` and `test-lowpower-g0` Makefile targets).
  They compile and run the opt-in `-DOW_PORT_LOW_POWER` path against the mock
  target for F1/F0/G0, asserting the observable low-power state: `SEVONPEND`
  armed by `onewire_init()`, `ow_long_pending` set on long stages (conversion
  wait, cycle pause) and cleared on completion, and `UIE` enabled in `T1.DIER`.
  Short slots stay outside the sleeping path (`ow_long_pending` keeps clear).
- **Hardware-validated on STM32G031** (parasite-powered bus, 6 × DS18B20): the
  low-power WFE path was tested at 16 MHz SYSCLK across four configurations
  (STANDARD / ROBUST / FAST × open-drain / active-drive) and completed full
  measurement cycles with correct temperatures in every case.
- **Hardware-validated on STM32F030** (parasite-powered bus, 7 × DS18B20): the
  low-power WFE path was tested at 48 MHz SYSCLK across four configurations
  (STANDARD / ROBUST / FAST × open-drain / active-drive) and completed full
  measurement cycles with correct temperatures in every case.
- **Hardware-validated on STM32F103** (parasite-powered bus, 7 × DS18B20): the
  low-power WFE path was tested at 72 MHz SYSCLK across four configurations
  (STANDARD / ROBUST / FAST × open-drain / active-drive) and completed full
  measurement cycles with correct temperatures in every case.

### Fixed

- **Low-power path (`OW_PORT_LOW_POWER`).** `ow_long_pending` was `static` in
  each port header, producing one independent copy per translation unit, so
  the 7_low_power example's `low_power_poll()` always read its own all-zero
  instance and never
  slept. It is now a single `extern` symbol defined in `src/onewire.c`.
  `ow_port_start_timer()` did not enable TIM1 `UIE` for long stages, so no NVIC
  pending bit was generated and `__WFE()` could never be woken; it now sets
  `T1.DIER |= TIM_DIER(UIE)` behind the macro. Finally, `ow_port_sleep_until_done()`
  used a single `__WFE()`; a leftover/stale event made it either spin (busy-loop
  degradation) or sleep forever. It now clears the NVIC pending bit first, re-arms
  the event structure with `__SEV()` + a draining `__WFE()`, polls `T1.SR`/`UIF`
  and clears the pending bit again after waking.
- **Wrong build flag documented.** The README, the low-power example header
  comment and the changelog referenced `-DOWN_PORT_LOW_POWER` (an extra `N`), which defines the
  macro `OWN_PORT_LOW_POWER` while the code checks `OW_PORT_LOW_POWER` — so the
  documented command line silently disabled the very feature it described. All
  occurrences are corrected to `-DOW_PORT_LOW_POWER`.

## [1.7.1] - 2026-08-29

### Documentation

- Documented the 1-Wire **bus electrical model** (open-drain signaling; the
  master never drives the line HIGH during a normal slot — `write-1` is a
  release, not a push-pull level; the parasite strong-pull-up is the only
  push-pull usage, confined to the slave-silent conversion window). Clarifies
  the hardware contract referenced by the published repository page.
- Stated the optional **active-drive write mode** and its open-drain invariant in
  the Bus Electrical Model: push-pull is confined to master-only write slots, with
  reset/presence/read/write-read phases staying open-drain.
- Documented the lifetime invariant for shared `conv_cmd`/`read_cmd` Skip-ROM
  buffers in `ds18b20.c`: safe to rewrite between DMA bursts because
  `issue_command()` is called exclusively from CONVERT/REQUEST after
  `onewire_bus_done()` confirms idle.
- Added PlatformIO, CMake (FetchContent) and STM32CubeIDE (Makefile import)
  integration sections to the README.

### Added

- **Active-drive write path (opt-in, `-DOW_DRIVE_ACTIVE`, default off).** During
  pure-write transactions PA10 switches to push-pull so the master actively
  drives *both* bus levels (`write-0`/`write-1`), giving a stronger/faster
  write-1 than the external pull-up RC rise. Every read/reset/slave-response
  phase and the merged write+read op stay open-drain, so the slave's wired-AND
  is preserved and there is no window where the master drives HIGH while a
  slave could pull LOW. Hardware-validated on STM32F1; build/host-tested on
  F0/G0. The published default remains open-drain.
- **Selectable compile-time timing presets** via `TIMING=SLOW` (Makefile sugar
  for `-DONEWIRE_ONE_PULSE=8 -DONEWIRE_ZERO_PULSE=90 -DONEWIRE_GUARD_BAND=20`
  plus `-DONEWIRE_SHORT_PULSE_MAX`, with `FAST`, `ROBUST` and the default
  `STANDARD` also available) for hardware margin testing, without changing the
  standard default.
- `test-active`, `test-active-f0`, `test-active-g0` Makefile targets exercising
  the active-drive pin-mode switching.
- **PlatformIO library metadata** (`library.json`, `library.properties`) for
  discovery via PlatformIO Library Manager and Arduino Library Manager.
- **Root `CMakeLists.txt`** with FetchContent-managed CMSIS dependencies and a
  bare-metal toolchain file (`cmake/arm-none-eabi-gcc.cmake`) for CMake-based
  projects.
- Auto-detection of MCU family in `ow_port.h` via PlatformIO / STM32CubeMX
  defines (`STM32F1`, `STM32F0`, `STM32G0`) alongside the explicit
  `OW_PORT_TARGET_*` knob.
- **Fuzz testing infrastructure** covering the full codebase: `onewire.c`
  pure functions (`fuzz_crc8`, `fuzz_decode_pulses`, `fuzz_present`,
  `fuzz_pair_bits`, `fuzz_encode_byte`, `fuzz_bit_from_pulse`), the Search ROM
  and resolution state machines (`fuzz_search`, `fuzz_resolution`), `ow_stats`
  internals (`fuzz_stats`) and DS18B20 decode functions
  (`fuzz_ds18b20_decode`). All harnesses compile and pass standalone
  tests with ASAN+UBSAN. CI `fuzz` job runs all targets with host-side
  Clang/libFuzzer.

## [1.7.0] - 2026-08-28

### Added

- Optional signal statistics module (`inc/ow_stats.h`, `src/ow_stats.c`):
  compile-in with `-DOW_STATS_ENABLE` to collect per-sensor pulse-width
  min/max, a global 13-bucket logarithmic histogram (0–60+ µs) and error
  counters (CRC, presence, other) across measurement cycles.  The module
  hooks into `src/ds18b20.c` automatically when enabled: pulse data is
  captured before `decode_scratchpad()` overwrites the buffer, and errors
  are counted at the four error-reporting paths.  The dump is fully
  non-blocking: `ow_stats_dump_start()` initiates it and
  `ow_stats_dump_poll()` streams one line per call, blocked only on the
  UART TX register (~87 µs/byte at 115200), so a 6-sensor report completes
  in ~30 ms without overflowing the 256-byte ring buffer.  RAM cost: ~96
  bytes.  Zero overhead when `OW_STATS_ENABLE` is not defined (all stubs
  inline to nothing).
- Example application `examples/6_statistics/main.c` (`make APP=6_statistics`):
  startup device
  search + sequential measurement with the `ow_stats` module.  Accumulates
  statistics over `STATS_DUMP_INTERVAL` cycles (default 100), then streams
  the full report over UART and resets for the next window.  Validated on
  STM32G031@64MHz with 6 × DS18B20 in parasite power mode — all sensors
  detected, 0 errors, pulse widths 5–32 µs.
- Example application `examples/2_device_search/main.c`
  (`make APP=2_device_search`):
  startup
  search + per-device polling.  Each discovered sensor is converted and read
  back individually via Match ROM (one `Convert T` per device, no broadcast
  conversion) — the minimal multi-sensor counterpart to `examples/4_scan_mode`'s
  simultaneous scan.  Parasite power is engaged with `EXT="-DPARASITE_POWER=1"`;
  note that per-device MATCH-ROM conversion is the marginal topology on a
  parasite bus, so a broadcast convert (4_scan_mode) is preferred there.
- Timing presets (`inc/onewire.h`, selected at build time): several selectable
  slot timings spanning fastest-to-slowest, all inside the DS18B20 1-Wire
  specification — `TIMING=FAST` (`ONEWIRE_ONE_PULSE=5`, `ZERO=60`, `GUARD=3`),
  `TIMING=STANDARD` (`5/60/5`, equals the historical defaults),
  `TIMING=SLOW` (`8/90/20`) and `TIMING=ROBUST` (`10/110/30`); see the
  `OW_TIMING_PARASITE` flag for the wider parasite-bus guard band. Selectable
  at build time with the Makefile `TIMING=` preset or by defining the
  `ONEWIRE_ONE_PULSE` / `ONEWIRE_ZERO_PULSE` / `ONEWIRE_GUARD_BAND` /
  `ONEWIRE_SHORT_PULSE_MAX` macros directly; the values drive the timer ARR,
  the write low-time and the read-decode threshold, so each preset adapts to
  different wire lengths and sensor tolerances without touching the per-port
  timer code. The standard preset remains the default.

### Removed

- **Breaking:** `ds18b20_read_power_supply()` / `ds18b20_read_power_supply_poll()`
  removed. Detect the bus wiring with `ds18b20_detect_parasite()` and read the
  result via `ds18b20_parasite_mode()` (the driver consumes it to engage the
  strong pull-up). This collapses the two overlapping query APIs that the 1.6.0
  parasite-power work introduced into one.

### Changed

- `examples/5_commands` reports the power-supply wiring through
  `ds18b20_detect_parasite()` /
  `ds18b20_parasite_mode()` instead of the removed `ds18b20_read_power_supply()`.

## [1.6.1]

### Fixed

- The `SYSCLK_MHZ` build knob was dead: the Makefile passed
  `-DOWN_PORT_SYSCLK_MHZ` instead of `-DOW_PORT_SYSCLK_MHZ`, so every
  non-default clock build silently used the family default frequency.
  This also means the v1.6.0 assets named `*_f0_8mhz` and `*_g0_16mhz`
  actually contained 48MHz / 64MHz firmware. The macro name is now spelled
  correctly (`Makefile`, verified by disassembly across clock variants).

### Changed

- `ONEWIRE_ONE_PULSE` is a single universal value (5µs) again: the ≤16MHz
  compensation that shortened it to 2µs is removed. Bench hardware on
  STM32F030F4P6 @8MHz showed that a 2µs master pulse breaks DS18B20 read-slot
  decoding outright (every capture stretches past the '0'/'1' threshold
  regardless of the sensor answer), while a plain 5µs pulse measures ~9µs
  there with every input-capture filter variant swept (fCK_INT N=2/4/8,
  fDTS/4 N=8) — so the capture chain, not the pulse, carries the slow-clock
  margin. Validated on hardware at STM32F030@48MHz/@8MHz and
  STM32F103@72MHz/@8MHz (6 devices, parasite power, hundreds of CRC-clean
  conversion cycles per variant; the F103@8MHz run also retires v1.6.0's
  11-12µs capture estimate, which came from different bench wiring).
  STM32G031@64MHz/@16MHz validated the same way on a WeAct STM32G031F6P6
  board — every supported clock combination is now hardware-confirmed.

 - The CH4 input-capture digital filter is standardized across all backends:
   a single documented rule in `inc/ow_port.h` picks the IC4F configuration
   whose filter time N × T_sample lands nearest ~500ns for the configured
  clock (`≤8MHz`: fCK_INT N=4; `≤16MHz`: fCK_INT N=8; above: fDTS/4 N=8),
  replacing the three hand-mirrored per-port conditionals. Firmware impact
  is limited to STM32G031@16MHz (N=4 → N=8, i.e. a 250ns → 500ns filter
  time); every other supported clock keeps byte-identical firmware, verified
  by md5 against the bench-validated binaries.

## [1.6.0] - 2026-08-23

### Added

- STM32G0 backend (`port/stm32g0/ow_port_g0.h`) for the STM32G031x6, e.g. the
  TSSOP20 STM32G031F6P6: same TIM1 CH3-output / CH4-indirect-capture scheme
  as F1/F0, with two family adaptations — the bus sits on logical PA10,
  which lives on the physical PA12 pad via the SYSCFG `PA12_RMP` remap (the
  package does not bond PA9/PA10 out), and DMA requests are routed through
  DMAMUX (TIM1_CC2 = request 21 feeds CCR3 via channel 3, TIM1_CH4 = request
  23 drains CCR4 via channel 4). Clocks ride the shared `OW_PORT_SYSCLK_MHZ`
  knob: default 64MHz (HSI16+PLL), `SYSCLK_MHZ=16` selects the raw HSI16.
  Select with `make OW_TARGET=g0`; host suite gains DMAMUX-routing tests and
  runs green on all three backends. Note: while the driver is initialised,
  pads PA11/PA12 must not be used as standalone GPIOs - reconfiguring them
  clears the remap bits and disconnects the bus. The shared slow-clock timing
  constants (≤16MHz threshold) are hardware-validated at 8MHz only, so the
  G0 raw-HSI16 build awaits bench validation like any new clock variant.

### Changed

- **Breaking:** the `HSI_8MHZ` build flag is replaced by the portable
  `OW_PORT_SYSCLK_MHZ` knob — a single integer carrying the system clock
  frequency in MHz. The old name tied a *source* choice to one family's
  frequency and could not scale (the raw HSI on an STM32G031 runs at
  16MHz, not 8). Every clock-dependent setting now derives from the new
  value: timer prescaler (`PSC = SYSCLK - 1`), input-capture filter
  selection and the '1'-slot pulse width switch on a shared ≤16MHz
  threshold, `app.c` derives its clock source per family (F1: 72 = HSE+PLL,
  8 = raw HSI; F0: 48 = HSI+PLL, 8 = raw HSI; anything else fails the
  build with a clear message) and the USART baud rate computes from it.
  Build via `make SYSCLK_MHZ=8`; defaults are unchanged.

### Fixed

- README architecture sections described the pre-unification register map
  (feed through `DMA1_Channel2`, captures into `CCR2` via `DMA1_Channel3`);
  they now document the scheme actually shipped: the CH2 marker request
  feeds `CCR3` through DMA1 channel 3, captures drain `CCR4` through
  DMA1 channel 4.

### Added

- Parasite-power support: `ds18b20_set_parasite(1)` makes the driver engage
  the strong pull-up (bus pin switched to push-pull HIGH) for every
  temperature-conversion window and EEPROM hold-off, releasing the line back
  to the external pull-up before any further bus activity. Backed by a new
  `ow_port_strong_pullup()` port hook in both the STM32F1 and STM32F0
  backends; seven host tests cover engagement windows, register state,
  mid-window flag clears and the default-off regression.
- Automatic parasite-mode detection: `ds18b20_detect_parasite()` /
  `ds18b20_detect_parasite_poll()` run a Read Power Supply query and store
  the decoded answer in the driver state, so parasite wiring is picked up at
  startup without a hard-coded flag; `ds18b20_parasite_mode()` reports the
  current configuration. Four host tests cover the detect path (parasite
  answer, external answer, no-presence abort leaves the flag untouched,
  getter/setter agreement). The example applications accept a
  `-DPARASITE_POWER=1` build flag to enable the mode out of the box, and the
  README gains a Parasite Power section with supply-current guidance.
- Public CI coverage: the host-test workflow now runs both backend suites
  (F1 and F0) as a test matrix, captures per-backend lcov traces and merges
  them into a single metric over the MCU-independent core (port plumbing is
  excluded, so adding new MCU families never dilutes the number). Every push
  to `main` republishes an HTML report and a self-hosted badge to the
  `gh-pages` branch; the badge links to
  [a5021.github.io/stm32-async-1wire](https://a5021.github.io/stm32-async-1wire/).
  This also fixes silent breakage of the previous coverage job: it captured
  from `build/test`, where no instrumented data ever landed.
- Debug assets for the STM32F0 backend, previously F1-only: Ozone project
  (`port/stm32f0/project.jdebug`), J-Flash project
  (`port/stm32f0/stm32f030f4.jflash`) and VSCode cortex-debug launch
  configurations ("Debug F0 (J-Link)" / "Debug F0 (ST-Link)") with a
  matching `Build F0 (debug)` task. The `STM32F030.svd` peripheral view is
  now downloaded together with the other build dependencies.

### Changed

- Sources and documentation aligned with the generic 1-Wire library
  concept: family-specific wording removed from `inc/ds18b20.h`,
  `CONTRIBUTING.md`, `SECURITY.md` and the Quick Start section; Quick Start
  now mentions raw `onewire.h` usage for non-DS18B20 slaves; VSCode
  IntelliSense gained an STM32F030 configuration matching
  `make OW_TARGET=f0`.
- Project renamed to `stm32-async-1wire`: the documentation now frames the
  library as a generic non-blocking 1-Wire layer for STM32 with the DS18B20
  driver as its first client. Repository URLs updated throughout (README,
  issue templates, release notes); public API and artifact names unchanged.
- Linker scripts and debug projects moved out of the repository root into
  their family port directories (`port/stm32f1/`, `port/stm32f0/`) — one
  self-contained folder per backend. The Makefile references them through
  new per-family variables (`LDS`, `JFLASH`), so `make OW_TARGET=f0
  jprogram` now uses the F0 J-Flash project instead of the F1 one. The
  Ozone projects resolve SVD/ELF paths relative to their own location.

## [1.5.0] - 2026-08-22

### Changed

- **BREAKING:** STM32F1 backend moved to the same TIM1 channel scheme as
  STM32F0: the 1-Wire bus now runs on **PA10** (previously PA8) as TIM1_CH3
  PWM output with CH4 indirect capture (IC4 <- TI3); the slot-end marker sits
  on a plain CC2 compare feeding CCR3 through DMA1 channel 3, while captures
  drain CCR4 through DMA1 channel 4. Re-wire DQ from PA8 to PA10 when
  upgrading. Timer logic, DMA channels and bus pin are now identical across
  both backends; only the prescaler and GPIO pin configuration differ. Both
  DMA request mappings verified empirically on the target (CC2 → channel 3,
  CH4 → channel 4).

## [1.4.1] - 2026-08-22

### Changed

- Release pipeline now ships the full firmware matrix: all four example apps
  (`1_basic`, `3_round_robin`, `4_scan_mode`, `5_commands`) for both backends
  (STM32F103 @72MHz/8MHz HSI, STM32F030
  @48MHz/8MHz HSI) — 16 variants with per-file SHA256 checksums.


## [1.4.0] - 2026-08-22

### Added

- STM32F0 backend (`port/stm32f0/ow_port_f0.h`): register-level `ow_port_*`
  implementation for the STM32F030x6 family (validated on an STM32F030F4P6,
  TSSOP20). The 1-Wire bus runs on **PA10** via TIM1 CH3 (PWM output,
  open-drain AF2) with indirect capture on CH4 (IC4 ← TI3, same pin) —
  PA8 is not bonded out in small F030 packages. The slot-end marker moved
  from the CH3 compare to a plain CH2 compare whose DMA request feeds CCR3;
  captures drain CCR4 through DMA1 channel 4 and the feed rides DMA1
  channel 3 (fixed request map — the F0 DMA has no CSELR mux).
- Build-time target selection: `make OW_TARGET=f0` switches the whole build
  (device headers, startup file, linker script `STM32F030X6_FLASH.ld`,
  backend include) to the STM32F030x6; the default remains STM32F103.
- Host test suite for the F0 backend: `make test-f0` runs the same 221-test
  suite against the F0 backend mock. The shared behavioural model
  (`tests/mock/hw_model.c`) and tests use backend-adaptive aliases
  (`MOCK_TIM_*` / `MOCK_BUS_*`), so both wirings are covered by one suite.
- HSI 8MHz clock option for both backends: `make HSI_8MHZ=1` selects a
  per-clock timer prescaler **and input-capture filter** so decode margins
  stay µs-equivalent at any system clock.
- Shared 1-Wire layer decode helpers `onewire_decode_pulses()` and
  `onewire_bit_from_pulse()`: the short/long pulse decode that was open-coded in
  four places (scratchpad decode, transaction read decode, search bit pairing
  and the search WRITE_READ step) is now a single shared path, covered by host
  unit tests.

### Changed

- `ds18b20_recall_eeprom_poll()` now documents that Recall EEPROM is a
  write-only command and does **not** update the tracked `ctx.resolution`.
  Callers that need the resolution to follow a possibly-different EEPROM config
  should follow the recall with `ds18b20_read_scratchpad()` to resynchronise
  `ds18b20_get_resolution()` before the next conversion.

### Fixed

- 8MHz HSI builds decoded '0' bits as '1' on both backends: the fixed
  ~5µs write pulse and the clock-scaled input-capture filter pushed read
  captures outside the decode window. The one-pulse width is now
  clock-dependent (2µs at 8MHz HSI vs 5µs otherwise) and the capture filter
  is selected per clock, so the decode threshold itself stays untouched.
- `ds18b20_select()` is only accepted while the measurement state machine is
  IDLE; calls from a running cycle, a device/alarm search, a resolution change
  or another command transaction are ignored — including from the per-device
  scan callback (`ds18b20_complete()` in scan mode), where a select is now
  explicitly rejected and the scan round continues. The API documentation and
  header notes were corrected to match this behaviour.

## [1.3.0] - 2026-08-17

### Added

- Non-blocking command transactions for the remaining DS18B20 commands, driven
  with the same poll discipline as the device search and the resolution change:
  `ds18b20_read_rom()`, `ds18b20_set_alarm_thresholds()`,
  `ds18b20_read_scratchpad()`, `ds18b20_copy_scratchpad()`,
  `ds18b20_recall_eeprom()`, `ds18b20_read_power_supply()` (each with a
  matching `*_poll()`). Every transaction performs reset → presence → write
  (+ Match ROM when a device is selected) → read or timed wait, owns TIM1/DMA
  while it runs and hands the timer back to `ds18b20_poll()` when finished.
  `ds18b20_last_command_ok()` reports whether the last transaction found a
  device present.
- `DS18B20_READ_ROM` (0x33), `DS18B20_RECALL_EEPROM` (0xB8) and
  `DS18B20_READ_POWER_SUPPLY` (0xB4) protocol constants exported by the header.
- Raw scratchpad read (`ds18b20_read_scratchpad()`): returns all 9 bytes
  including TH/TL and the CRC; on a valid read the conversion resolution is
  auto-derived from the config byte (byte 4, R1/R0).
- Alarm trigger thresholds (`ds18b20_set_alarm_thresholds(th, tl)`): writes
  TH/TL with Write Scratchpad (0x4E) without disturbing the current
  resolution; `ds18b20_copy_scratchpad()` and `ds18b20_recall_eeprom()`
  persist and restore them to/from the EEPROM with a 10 ms timed hold-off.
- Example application `examples/5_commands/main.c` (`make APP=5_commands`):
  device search, then
  the full command sequence on the first found sensor — power supply, raw
  scratchpad, TH/TL write with Copy + Recall, Read ROM — followed by
  steady-state measurement of the selected device.

### Changed

- Ownership guards: every `ds18b20_search_start()`,
  `ds18b20_alarm_search_start()`, `ds18b20_scan_start()` and
  `ds18b20_set_resolution()` entry point now rejects starting while a command
  transaction owns the timer, and `ds18b20_poll()` skips its work until the
  active transaction finishes.
- `ds18b20_select()` now also applies to the command transactions: with a
  device selected, every command (except the bare Read ROM) is addressed via
  Match ROM, so the transactions target that specific sensor.

## [1.2.0] - 2026-08-17

### Added

- Non-blocking alarm search: `ds18b20_alarm_search_start()`,
  `ds18b20_alarm_search_poll()`, `ds18b20_alarm_search_count()`. Implements the
  Maxim Alarm Search ROM (0xEC) algorithm with the same state machine as the
  device search: it reports only the DS18B20 devices currently in alarm state
  (temperature outside the TH/TL thresholds set with Write Scratchpad). The
  alarm search never repopulates the scan-mode device table, so the addresses
  found by a previous device search stay valid while the alarm state of the bus
  is polled.
- Universal non-blocking 1-Wire layer: `inc/onewire.h` + `src/onewire.c`. The
  bus primitives (reset, presence, write/read slots, merged write-then-read,
  multi-byte read) and the generic Maxim Search ROM engine
  (`onewire_search_start()`, `onewire_search_poll()`, `onewire_search_count()`,
  `onewire_search_active()`) now live in a shared layer that any 1-Wire slave
  driver (DS18B20 today, DS2413/DS2431 later) is built on. Every operation is
  scheduled on TIM1/DMA and completes asynchronously; callers poll
  `onewire_bus_done()` / `onewire_search_poll()` to advance, never wait. The
  layer is registered for capture via its own capture buffers and keeps the same
  hardware bus release to idle HIGH after every transaction. It also provides
  the Dallas/Maxim CRC-8 utility `onewire_crc8()`.

### Changed

- The 1-Wire bus primitives and the Search ROM state machine moved out of the
  driver into the shared layer. `src/ds18b20.c` now builds on `onewire_*`; the
  public `ds18b20.h` API (including the `ds18b20_bus_*`-free surface) is
  unchanged, and the driver's `ds18b20_init()` initializes the layer for you.
- The newlib-nano syscall stubs (`_read`, `_write`, `_close`, `_lseek`) in
  `src/syscall.c` are now `weak`: the layer and the driver each pull them in,
  and weak definitions keep the multi-translation-unit firmware link clean.

## [1.1.0] - 2026-08-16

### Added

- Non-blocking device search: `ds18b20_search_start()`, `ds18b20_search_poll()`,
  `ds18b20_search_count()`. Implements the Maxim Search ROM (0xF0) algorithm as
  a compact state machine that performs exactly one hardware operation per poll
  call, then hands the timer back to the measurement path automatically. See
  `examples/3_round_robin`.
- Shared application layer (`examples/app/app.h`, `examples/app/app.c`):
  non-blocking UART TX ring
  buffer, `app_init()` for clock + UART + LED setup, and a default
  `ds18b20_busy()` LED indicator. Both examples now `#include "app.h"` and
  delegate hardware setup to the shared layer.
- Match ROM prefix caching: `ds18b20_select()` builds the invariant 72-slot
  Match ROM prefix once per selection, so each subsequent measurement patches
  only the last byte (8 slots instead of 80).
- Non-blocking resolution change: `ds18b20_set_resolution()` /
  `ds18b20_set_resolution_poll()` change the conversion resolution (9..12 bit)
  between measurement cycles, mirroring the device search state machine (one
  hardware operation per poll). The config is written with
  Write Scratchpad (0x4E) to the volatile scratchpad; TH/TL are reset to 0
  (alarms disabled) and the change is not persisted to the EEPROM.
- `ds18b20_get_resolution()` reports the current resolution, auto-derived from
  every valid scratchpad read (byte 4, R1/R0 bits).
- Simultaneous multi-device conversion: `ds18b20_scan_start()` converts every
  discovered device in parallel with one broadcast `Convert T` (Skip ROM), then
  reads each one back via Match ROM in device-table order. One conversion wait
  covers all sensors (`750ms + N x read` instead of `N x 750ms`). Each reading
  is reported through `ds18b20_complete()`; `ds18b20_scan_index()`,
  `ds18b20_device_rom()` and `ds18b20_device_count()` identify the sensor. A
  missing device reports `DS18B20_TEMP_ERROR_NO_SENSOR` and the scan continues.
  `ds18b20_select()` (single-device addressing) clears scan mode; the config
  write for a resolution change is broadcast in scan mode. See
  `examples/4_scan_mode`.

### Changed

- The public API is strictly non-blocking: the high-level interface
  (`ds18b20_init()`, `ds18b20_poll()`, `ds18b20_select()`, the weak callbacks
  `ds18b20_busy()`/`ds18b20_complete()`) plus the `ds18b20_search_*` family,
  the `ds18b20_scan_*` family, the `ds18b20_set_resolution_*` family and the
  CRC utility `ds18b20_crc8()`. The internal 1-Wire bus helpers
  (`ds18b20_bus_*`), the Search ROM state machine and the resolution-change
  state machine live inside the library (`src/ds18b20.c`).
- The non-blocking device search is driven from the main loop exactly like the
  measurement state machine: each `ds18b20_search_poll()` performs one
  hardware operation. It filters by `DS18B20_FAMILY_CODE`, validates the ROM
  CRC and forces a timer update event before handing back to `ds18b20_poll()`.
- UART TX is now fully non-blocking: `uart_tx_enqueue_byte()` drops a byte when
  the ring buffer is full, and `uart_tx_flush()` was removed.
- The conversion wait is now resolution-aware: `wait_conversion()` waits
  exactly the datasheet time of the configured resolution (93.75ms @ 9-bit,
  187.5ms @ 10-bit, 375ms @ 11-bit, 750ms @ 12-bit) instead of a fixed 750ms,
  so lower resolutions complete 8× faster.
- The 1-Wire line is now released to idle HIGH purely in hardware after every
  transaction: DMA-fed writes append a trailing 0 to the CCR1 feed (last-slot
  CC4 event) and direct-write/capture operations use an OC1PE preload of 0,
  both applied exactly when the one-pulse timer stops. The software
  `T1.CCR1 = 0` in `ds18b20_bus_done()` was removed, so the bus idles HIGH
  between slots regardless of RTOS scheduling latency (verified: 5/5 devices
  found with no software release).
- Protocol constants (`DS18B20_SEARCH_ROM`, `DS18B20_MATCH_ROM`,
  `DS18B20_CONVERT_T`, `DS18B20_READ_SCRATCHPAD`, `DS18B20_WRITE_SCRATCHPAD`,
  `DS18B20_COPY_SCRATCHPAD`, `DS18B20_RES_MIN`/`DS18B20_RES_MAX`/
  `DS18B20_RES_DEFAULT`, `DS18B20_BITS_PER_BYTE`) are exported by the header
  for use with the public API and host tests.
- DMA transfer count sized to `slots-1` instead of using a sentinel value.
- Removed the global `#define CR` peripheral alias from `inc/macro.h` (now
  `inc/ow_bits.h`): it
  collided with the `RCC_TypeDef.CR` field name after CMSIS headers were
  included, expanding the field into a pointer that shifted every RCC register
  offset on 64-bit host builds.
- Added a host test suite (`make test`): the driver is compiled as a single
  translation unit against a TIM1/DMA behavioural model and a register mock.
  145 tests cover the state machine, device search, resolution change, CRC-8,
  pulse encoding, presence detection, scratchpad decode, temperature
  conversion, timing and bus release. The suite runs in CI.

### Fixed

- The scan mode stalled after the first device read on real hardware: after a
  per-device read-back the state machine sat in `CONTINUE` waiting for a UIF
  that never came, because `DECODE` arms no timer and only the inter-measurement
  pause (single-device path) or a new conversion provides one. A 1ms scheduling
  bridge timer is now armed between scan-mode reads so every device in the table
  is reported each round (verified on an 8-sensor bus: 8 readings per round).
- The non-blocking measurement state machine never started after a device
  search: the search clears the timer update flag on every operation, which
  left the driver idling in state 0 forever waiting for a UIF that never
  arrived. The search now forces a timer update event on completion so the
  first `ds18b20_poll()` call begins a measurement cycle immediately.
- The device search reported every 1-Wire device on the bus, not just DS18B20
  temperature sensors: other families (DS2401, DS1990, etc.) were stored and
  polled as if they were DS18B20s. The search now skips any device whose ROM
  family code is not `DS18B20_FAMILY_CODE` (0x28).

## [1.0.0] - 2026-08-05

### Added
- Non-blocking, interrupt-free DS18B20 driver for STM32F103C8T6:
  TIM1 Output Compare + Input Capture with DMA and a hardware state machine
  handle all 1-Wire timing (reset, write/read slots, 750 ms conversion wait).
- `HSI_8MHZ` conditional build variant for running at 8 MHz without an
  external crystal or PLL.
- Automatic download of CMSIS core/device dependencies in the Makefile
  (`make download-deps`, `make clean-deps`).
- Non-blocking, poll-driven UART debug output with a ring buffer.
- Weak callbacks `ds18b20_busy()` and `ds18b20_complete()` for LED status
  and measurement results.
- VSCode workspace configuration: build tasks, J-Link/ST-Link debug
  configurations, IntelliSense paths and SVD peripheral views.
- CI pipeline: clang-format, cppcheck, GCC `-fanalyzer`, and an ARM build
  workflow with toolchain/dependency caching.
- GCC/Binutils version detection and `--no-warn-rwx-segments` handling in
  the build system.
- `curl` fallback when `wget` is not available.

### Fixed
- BITS macro bug and missing `#endif` in `inc/macro.h` (now `inc/ow_bits.h`);
  removed ~220 lines of
  dead, duplicated code.
- Lost negative sign for temperatures between -0.5 and -0.1 °C in UART
  output.
- 1-Wire slot timing: `ONE_PULSE=5`, slot formula 5+60+5 = 70 µs.
- Undefined behavior in `uart_write_int`.
- Missing `__DSB()` memory barrier in the driver initialization.
- Extern C guard in `inc/macro.h` (missing quotes).
- cppcheck false positives from CMSIS headers.

### Changed
- Callback rename: `led_control` -> `busy`, `temp_ready` -> `complete`.
- One-letter macros renamed to descriptive names; unified TIM1 SR access.
- CMSIS dependency restructure, linker script rewrite and licensing cleanup.
- README restructured: hardware connections, architecture, API reference,
  troubleshooting, build and flash instructions.

[1.0.0]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.0.0
[1.1.0]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.1.0
[1.2.0]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.2.0
[1.3.0]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.3.0
[1.4.0]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.4.0
[1.4.1]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.4.1
[1.5.0]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.5.0
[1.6.0]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.6.0
[1.6.1]: https://github.com/a5021/stm32-async-1wire/releases/tag/v1.6.1
[1.7.0]: https://github.com/a5021/stm32-async-1wire/compare/v1.6.1...v1.7.0
[1.7.1]: https://github.com/a5021/stm32-async-1wire/compare/v1.7.0...v1.7.1
[1.8.0]: https://github.com/a5021/stm32-async-1wire/compare/v1.7.1...v1.8.0
[1.8.1]: https://github.com/a5021/stm32-async-1wire/compare/v1.8.0...v1.8.1
[2.0.0]: https://github.com/a5021/stm32-async-1wire/compare/v1.8.1...v2.0.0
[2.1.0]: https://github.com/a5021/stm32-async-1wire/compare/v2.0.0...v2.1.0
[2.1.1]: https://github.com/a5021/stm32-async-1wire/compare/v2.1.0...v2.1.1
[Unreleased]: https://github.com/a5021/stm32-async-1wire/compare/v2.1.1...HEAD
