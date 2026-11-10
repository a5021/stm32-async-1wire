# Troubleshooting guide

[Back to the README](../README.md)

## Contents

- ["No sensor detected" or "CRC check failed"](#no-sensor-detected-or-crc-check-failed)
- [Temperature readings are infrequent](#temperature-readings-are-infrequent)
- [The bus looks dead, or every timing is scaled](#the-bus-looks-dead-or-every-timing-is-scaled)
- [STM32H5 console is garbled and everything runs at half speed](#stm32h5-console-is-garbled-and-everything-runs-at-half-speed)
- [STM32F4 console is silent on PA9](#stm32f4-console-is-silent-on-pa9)
- [Build fails with "no MCU selected", or selects the wrong backend](#build-fails-with-no-mcu-selected-or-selects-the-wrong-backend)
- [Link fails with undefined references after enabling LTO](#link-fails-with-undefined-references-after-enabling-lto)
- [An operation never completes, or the drivers interfere](#an-operation-never-completes-or-the-drivers-interfere)
- [DS2413 PIO reads return 0xFF or writes fail](#ds2413-pio-reads-return-0xff-or-writes-fail)
- [Parasite-powered sensors read 85.0 °C or 127.9 °C, or conversions fail](#parasite-powered-sensors-read-850-c-or-1279-c-or-conversions-fail)
- [`ds18b20_poll()` blocks for hundreds of milliseconds](#ds18b20_poll-blocks-for-hundreds-of-milliseconds)
- [The wrong pin is used, or a peripheral conflict appears](#the-wrong-pin-is-used-or-a-peripheral-conflict-appears)
- [Debugging Tips](#debugging-tips)

## Troubleshooting

### "No sensor detected" or "CRC check failed"

- **Cause:** the most common cause is electrical. The presence pulse captured by
  the DMA/timer did not meet the timing criteria, or noise corrupted the data
  during the 72-bit read.
- **Fix:**
  - Check all wiring connections.
  - Ensure a 4.7 kΩ pull-up resistor is between the 1-Wire data line
    (PA10; logical PA10 via the PA12 remap on G0) and 3.3 V.
  - Verify stable power is supplied to the DS18B20 sensor.
  - Keep data lines short to minimize noise and capacitance.

### Temperature readings are infrequent

- **Cause:** `ds18b20_poll()` is called slowly from the main loop. The driver
  operates correctly but advances through its states (e.g. the 750 ms
  conversion wait) at a slower pace.
- **Fix:** this is often not a problem if a slow update rate is acceptable. If
  faster updates are needed, ensure the main loop runs frequently and avoids
  other blocking code. The driver never waits for a result and does not cause
  this slowdown; the one build that can delay a `poll()` call is the opt-in
  `OW_LOW_POWER=1` one, where the driver idles the core until the stage is
  done — see [7_low_power](examples.md#low-power-mode-7_low_power).

### The bus looks dead, or every timing is scaled

- **Cause:** the clock the code was compiled for does not match the clock the
  firmware actually runs at. TIM1's kernel clock must equal SYSCLK: a mismatch
  scales every 1-Wire slot by the same factor and the sensor never answers.
- **Fix:**
  - Pass the real clock (`-DOW_SYSCLK_MHZ=<MHz>`, or `SYSCLK_MHZ=<MHz>` on the
    Makefile command line), or omit it and call `SystemCoreClockUpdate()` after
    any clock-tree change.
  - Check at startup and refuse to run on 0:
    `if (!onewire_clock_ok(SystemCoreClock / 1000000u)) { ... }`.
  - Without `-DOW_SYSCLK_MHZ` the check only rejects a zero/uninitialised
    `SystemCoreClock`; it cannot detect a stale value after a clock change.
  - A split-clock target (H7 `SYSCLK_MHZ=480 TIM1_MHZ=240`) must configure TIM1
    to `OW_TIM1_MHZ`, not to SYSCLK.
  - When linking a prebuilt archive, also compare `onewire_config_token()` with
    the application's `OW_CONFIG_TOKEN`; a mismatch means different flags,
    clock or `DS18B20_MAX_DEVICES`.

### STM32H5 console is garbled and everything runs at half speed

- **Cause:** the H5 boots with HSI divided to 32 MHz (`RCC_CR.HSIDIV = /2`),
  while the build assumes 64 MHz.
- **Fix:** clear `HSIDIV` and set the flash latency
  (`FLASH_ACR = PRFTEN | LATENCY_3WS`) before `SystemCoreClockUpdate()`. The
  250 MHz build additionally needs VOS0 and 5 flash wait states. See
  [Common gotchas](building.md#common-gotchas).

### STM32F4 console is silent on PA9

- **Cause:** the F4 examples put USART1 TX on **PB6**, which is where the
  F4DISCOVERY's ST-Link VCP and the WeAct F446's CP210x answer. PA9 carries no
  console signal on those boards.
- **Fix:** wire the USB-UART adapter to PB6 (115200 8N1), or build with
  `-DOW_UART_USART1_PA9` when your board really routes USART1 to PA9. The F4
  busy LED and console pins are in [Hardware Connections](hardware.md#hardware-connections).

### Build fails with "no MCU selected", or selects the wrong backend

- **Cause:** `ow_board.h` needs exactly one ST device macro (e.g.
  `STM32H503xx`) or its family macro (`STM32H5`). An unknown `OW_MCU` is
  rejected instead of silently falling back to F1.
- **Fix:** pass `OW_MCU=<part>` to Make/CMake (or define the macro in your own
  build). Do not mix a family macro with a part macro. When vendoring `inc/` +
  `src/`, also supply the CMSIS core/device headers, `system_<family>xx.c`, the
  GCC startup file and your linker script — see
  [Vendored copy](building.md#3-vendored-copy-ow_add_librarycmake).

### Link fails with undefined references after enabling LTO

- **Cause:** a plain `ar` archive does not index LTO symbols.
- **Fix:** archive with `arm-none-eabi-gcc-ar` (the Makefile and CMake builds do
  this automatically; `cmake/ow_add_library.cmake` selects it when available),
  or build the library without `-flto`.

### An operation never completes, or the drivers interfere

- **Cause:** starting an operation while another owns TIM1/DMA, polling two
  drivers at once, or calling a second `*_init()` during activity. A second init
  is **not** a reset and does not stop an active operation.
- **Fix:** keep one bus owner at a time. Finish and retire the operation with its
  own `*_poll()` before another driver starts, and never poll two concurrently —
  `9_multi` shows the explicit owner state machine. Once activity has started, a
  clean new driver lifecycle requires an MCU reset. `ds18b20_start_measure()` and
  every `*_start()` are ignored while the bus is busy.

### DS2413 PIO reads return 0xFF or writes fail

- **Cause:** no valid reply (absent device, wrong ROM, bus not idle), a missing
  parasite strong pull-up, or a PIO pin driven externally so the reply does not
  match the latch.
- **Fix:**
  - Check `ds2413_last_reply()`: `0xFF` means no answer. Confirm the family code
    `0x3A` and that the ROM passed to `ds2413_select()` is the device on the bus.
  - On a parasite-powered DS2413 keep `-DOW_PARASITE_POWER=1`; the driver still
    issues its own 1 ms strong pull-up prime before each operation.
  - Finish any other driver operation first; the bus is single-owner.
  - The driver retries a bad reply three times. A pin that is externally driven
    reads back the actual level, not the commanded latch.

### Parasite-powered sensors read 85.0 °C or 127.9 °C, or conversions fail

- **Cause:** parasite power is not enabled in both places, or the bus cannot
  source the conversion current.
- **Fix:**
  - Define `-DOW_PARASITE_POWER=1` **and** call `ds18b20_set_parasite(1)` at
    runtime. Without both, the strong pull-up never engages.
  - `85.0 °C` is the DS18B20 power-on-reset value: the scratchpad was read
    before a conversion finished (wrong resolution/wait plan) or the device lost
    power. `4_scan_mode` programs a uniform resolution for this reason.
  - A broadcast conversion draws about 1.5 mA per sensor. Feed a multi-sensor
    fleet from a dedicated supply with a 2.2 kΩ pull-up; a sagging USB supply
    can stick a marginal sensor at `127.9 °C` with a **valid** CRC.
  - On an F4DISCOVERY, do not plug a USB cable into the OTG connector while the
    bus runs: it grounds PA10 (`OTG_FS_ID`) and holds the bus low. See
    [Parasite Power](api-reference.md#parasite-power).

### `ds18b20_poll()` blocks for hundreds of milliseconds

- **Cause:** this is the opt-in `OW_LOW_POWER=1` build. The driver idles the
  core in `__WFE()` while a long stage runs (a conversion up to 750 ms, the
  scratchpad read, or an EEPROM hold-off).
- **Fix:** expected behaviour. It is a sleep, not a wait for a result: remove
  `OW_LOW_POWER=1` to return to pure polling. In this build `app_millis()`
  under-counts, because it only sees SysTick wraps observed by the loop.

### The wrong pin is used, or a peripheral conflict appears

- **Cause:** the bus pin and the TIM1/DMA pair are fixed by the capture topology
  and are owned by the library from init until reset.
- **Fix:** use PA10 (TIM1_CH3) on every family except G0, where the physical
  PA12 pad is remapped to logical PA10. Do not reconfigure TIM1, its DMA
  channels or the bus pin; on G0 do not use PA11/PA12 as GPIO; on F4 keep the
  OTG cable unplugged. There is no deinit or release API. See
  [Before you integrate](getting-started.md#before-you-integrate) and
  [Hardware Connections](hardware.md#hardware-connections).

### Debugging Tips

- Use Debug Build: The release build (`-Os -flto -g0`) aggressively optimizes
  the driver, which may inline or eliminate static variables like `ctx`.
  Use `make debug` (`-Og -g3 -gdwarf`) for debugging.
- VSCode: Press F5 to build (debug) and launch a J-Link debug session.
  The SVD file provides peripheral register views.
- Check the startup guards: call `onewire_clock_ok()` and compare
  `onewire_config_token()` before the first bus operation.
- Monitor the State Variable: Check `ctx.current_state` in a debugger to
  see the current step in the communication sequence.
- Check the Update Flag: Read `TIM1->SR`. If the driver seems idle,
  a set UIF bit indicates a completed operation waiting to be processed
  by `ds18b20_poll()`.
- Inspect the GPIO: Use an oscilloscope on the data pin (PA10) to verify the 1-Wire
  waveforms. Look for:
  - A clean ~480 µs reset pulse (MCU pulls low, then releases).
  - A presence pulse ~60-240 µs after the reset pulse (sensor pulls low).
  - Precise write slots: a short ~5 µs low for a '1', a long ~60 µs low
    for a '0' (slot = 5 + 60 + 5 = 70 µs).
- Inspect Captured Data: after a reset, call `onewire_reset_captured()` and
  require 2 before trusting `ctx.capture[1]`; with no device the second slot
  is stale and may look like a valid presence. `ctx.capture[0]` is the reset
  pulse and `ctx.capture[1]` the reset-to-presence capture; after a read,
  inspect `ctx.pulse[]` (both in `src/ds18b20.c`).
