# Getting started

[Back to the README](../README.md)

## Requirements

- Microcontroller: any STM32 with a single advanced-control timer instance
  that satisfies the complete [Required Timer Capabilities](architecture.md#required-timer-capabilities)
  and DMA topology (currently supported: STM32F030x6, STM32F103C8T6,
  STM32F303VC, STM32F407VGT6, STM32F401CC, STM32F446RE,
  STM32G031x6, STM32G474CB, STM32L476RG, STM32C552CET6, STM32H503CB,
  STM32H743VI; see the per-family sections in `src/ow_mcu.h`).
- Sensor: DS18B20 temperature sensor, DS2431 EEPROM and/or DS2413 dual-channel
  switch; the bundled drivers take turns owning the bus, one operation at a
  time (see
  [DS2431 EEPROM Driver](api-reference.md#ds2431-eeprom-driver) and
  [DS2413 Driver](api-reference.md#ds2413-dual-channel-switch-driver))
- Toolchain: GCC ARM (arm-none-eabi)
- Clock Configuration: defaults and fallbacks are listed below; `SYSCLK_MHZ=N`
  overrides the part default.

| Family / part | Default | Alternatives and notes |
| --- | --- | --- |
| STM32F103 | 72 MHz HSE+PLL | 8 MHz raw HSI (`SYSCLK_MHZ=8`) |
| STM32F030 | 48 MHz HSI/2+PLL | 8 MHz raw HSI |
| STM32F303VC | 72 MHz HSE-bypass+PLL | 64 MHz HSI/2+PLL (the HSI ceiling; no HSI16); 8 MHz raw HSI. The ST-Link drives the board crystal onto OSC_IN in bypass mode. |
| STM32F407 | 168 MHz HSE+PLL | 16 MHz raw HSI; the crystal frequency (`SYSCLK_MHZ=<HSE_MHZ>`) |
| STM32F401CC | 84 MHz HSE+PLL | 16 MHz raw HSI; the crystal frequency |
| STM32F446 | 180 MHz HSE+PLL + over-drive | 16 MHz raw HSI; the crystal frequency |
| STM32G031 | 64 MHz HSI16+PLL | 16 MHz raw HSI16 |
| STM32G474 | 170 MHz HSE+PLL + Range 1 Boost | 16 MHz raw HSI16 |
| STM32L476 | 80 MHz HSI16+PLL (M=1, N=10, R=2, Range 1, 4 WS) | 16 MHz raw HSI16. The NUCLEO-L476RG has no HSE crystal populated, so there is deliberately no crystal clock. |
| STM32C552CET6 | 144 MHz HSE->PSI | 48 MHz HSIDIV3 reset clock; raw 8 MHz HSE (`SYSCLK_MHZ=8`, `HSE_MHZ=8`); 8-bit feed tables |
| STM32H503 | 64 MHz raw HSI | 8 MHz raw HSE (`SYSCLK_MHZ=8`, `HSE_MHZ=8`); 250 MHz HSE+PLL1 (M=HSE/4, N=125, P=2, VOS0, 5 WS) |
| STM32H743 | 240 MHz HSE+PLL (M=HSE/5, N=96, P=2, LDO, VOS0, 4 WS; 25 MHz crystal) | 480/240 MHz split (`SYSCLK_MHZ=480 TIM1_MHZ=240`, HPRE=/2 keeps HCLK=240 MHz) |

On F4 the crystal is a separate knob, `HSE_MHZ=N`, because it is the *board's*
property while `SYSCLK_MHZ` is the application's: the PLL takes its M divider
from it, so a 25 MHz board reaches the F401's 84 MHz cap with M=25/N=168, an
8 MHz board the F407's 168 MHz with M=8/N=336 and the F446's 180 MHz with
M=8/N=360. Part files carry a default for the board they are named after —
`chips/f401xc.mk` says 25 MHz for the WeAct F401 Black Pill — and `HSE_MHZ=N`
overrides it. The default is 8 MHz elsewhere except H7 (25 MHz, the WeAct
Studio STM32H7XX crystal). A wrong value is not a compile error: the PLL simply
never locks, so the HSE, PLL and (on the F446) over-drive waits are bounded and
`app_init()` reports the failure over the console and stops rather than running
on a clock its timings were not compiled for.

The 180 MHz mode is the one clock on F4 that is above 168 MHz, and it is a
different code path rather than a different number: it needs the PWR over-drive
sequence (`ODEN`→`ODRDY`, `ODSWEN`→`ODSWRDY`), its own flash-latency and APB
values, and PLLN=360. Only the F446 offers it, so a build that asks for 180 MHz
on any other F4 part is rejected at compile time rather than quietly run out of
spec — each part's ceiling is `OW_F4_MAX_SYSCLK_MHZ` in the `inc/ow_board.h`
F4 block. APB2 is `/2` at 180 MHz as it is at 168, so the timer clock still
doubles to 2 × 90 = 180 MHz and the 1 µs-tick invariant
(`PSC = SYSCLK_MHZ - 1`) is unchanged.

## Before you integrate

The bus hardware is fixed by design, not configurable. Check these five
facts before wiring or porting:

- **Bus pin is PA10 (TIM1_CH3).** F0 AF2, F1 default map, F3 AF6, F4 AF1,
  C5 AF1, G4 AF6, H5/H7 AF1; G0 uses the PA12 pad remapped to logical PA10 via
  `SYSCFG_CFGR1.PA12_RMP` (see Hardware Connections). There is no pin or
  timer choice: the IC4→TI3 capture topology requires this pair.
- **TIM1 + DMA pair + bus pin belong to the library after init.**
  `onewire_init()` takes exclusive ownership until MCU reset; there is no
  deinit or release API (`inc/onewire.h:97-104`). Do not use TIM1, those DMA
  channels, or the bus pin elsewhere while the layer is in use.
- **Initialization is not a reset.** A second `onewire_init()` /
  `ds18b20_init()` / `ds2431_init()` / `ds2413_init()` call does not stop an
  active operation, and it does not clear stale timer/DMA/low-power or
  slave-driver state. Calling all three driver inits once, before any bus
  operation, is supported (each `onewire_init()` re-applies the same idle
  configuration; `9_multi` does this). Starting a new driver lifecycle after
  activity requires an MCU reset (`inc/onewire.h`).
- **One driver owns the bus at a time.** `ds18b20_init()`, `ds2431_init()` and
  `ds2413_init()` all claim the same TIM1/DMA/pin. Finish and retire an
  operation on one driver before another starts one, and never poll two at
  once; `9_multi` shows the explicit owner state machine that hands the bus
  over.
- **Clock invariant: TIM1 kernel clock must equal SYSCLK.** Pass the real
  clock via `OW_SYSCLK_MHZ`, or omit it and keep CMSIS `SystemCoreClock`
  current via `SystemCoreClockUpdate()`. A wrong clock silently scales
  every 1-Wire slot. Check at startup with
  `onewire_clock_ok(SystemCoreClock / 1000000u)` and refuse to run on 0.

## Quick Start

### 1. Include the Driver

```c
#include "ds18b20.h"
```

> Working with a 1-Wire slave that has no driver in this repo (DS2408, ...)?
> Include `"onewire.h"` instead and build directly on the shared bus primitives
> (`onewire_reset()`, `onewire_write_then_read()`, the Search ROM engine) —
> no DS18B20 code is pulled in. DS2431 and DS2413 are the exceptions: they ship
> with the repo as their own drivers — include `"ds2431.h"` / `"ds2413.h"`
> (see `examples/8_ds2431` and `examples/9_multi`).

### 2. Initialize the Driver

The **only** define the library requires is the MCU selection: the
STMicroelectronics device macro for your part (e.g. `-DSTM32H503xx`) or its
family macro (e.g. `-DSTM32H5`). `src/ow_mcu.h` uses it to include the matching
CMSIS device header and to pick the backend. Everything else is optional.

The system clock is one such optional setting. `OW_SYSCLK_MHZ` fixes the
TIM1 prescaler that generates the 1-Wire timing at compile time. The Make and
CMake builds pass it for the part being built
(`-DOW_SYSCLK_MHZ=$(CHIP_SYSCLK_MHZ)` from `chips/<part>.mk`: 48 on
STM32F0, 72 on STM32F1, 72 on STM32F3, 64 on STM32G0, 170 on STM32G4, and on
F4 the part's own clock: 168 on F407, 84 on F401, 180 on F446), and the
examples enable that clock in their `app_init()`. If your firmware runs the
MCU at a different frequency, pass it explicitly — e.g.
`-DOW_SYSCLK_MHZ=8` for an 8 MHz HSI build. If you omit it entirely, the
port core reads the CMSIS `SystemCoreClock` at startup instead — keep it
updated with `SystemCoreClockUpdate()` after any clock-tree change. TIM1's
kernel clock must equal SYSCLK either way.

**Single sensor (Skip ROM).** With one DS18B20 on the bus there is no need for
a ROM search — the driver uses Skip ROM (broadcast) addressing by default:

```c
#include "ds18b20.h"

int main(void) {
    ds18b20_init();  // One-time initialization

    ds18b20_start_measure();  // Request one measurement cycle

    while (1) {
        ds18b20_poll();  // Call repeatedly from main loop
        // ... report in ds18b20_complete(), decide the cadence here:
        //     when to call ds18b20_start_measure() again ...
    }
}
```

**Multiple sensors (bus search).** To find every device, run the non-blocking
Search ROM state machine, then address each sensor by ROM with
`ds18b20_select()`:

```c
#include "ds18b20.h"

static uint8_t found_roms[8][DS18B20_ROM_BYTES];
static uint8_t found_count = 0;

// Called for every DS18B20 the search finds; return 0 to keep searching.
static uint8_t on_device_found(const uint8_t* rom) {
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        found_roms[found_count][i] = rom[i];
    }
    found_count++;
    return 0;
}

int main(void) {
    ds18b20_init();  // One-time initialization

    // The search owns the bus until it finishes: do not call ds18b20_poll()
    // while it is running.
    ds18b20_search_start(on_device_found, 8);
    while (!ds18b20_search_poll()) {
        // Repeatedly advance the search; returns 1 when it is finished.
    }

    if (found_count > 0) {
        ds18b20_select(found_roms[0]);  // Measure the first sensor
        ds18b20_start_measure();        // Request one measurement cycle

        while (1) {
            ds18b20_poll();  // Call repeatedly from main loop
            // Other application code... decide here when to request the
            // next cycle with ds18b20_start_measure()
        }
    }
    return 0;
}
```

See `examples/1_basic/main.c` for a complete single-sensor setup and
`examples/2_device_search/main.c` for the search + round-robin loop.

### 3. Implement Callbacks (Optional)

Both callbacks are optional; the driver ships empty weak implementations. The
shared example layer `examples/app/app.c` supplies a strong `ds18b20_busy()`
that drives the onboard LED (F0/G0: PA4, F1: PC13, F3: PE8, F4: PD12 — or PB2
with `-DOW_F4_LED_PB2` on a WeAct F446RET6, G4: PA8 on a WeAct G474 Long,
L4: PA5 on a NUCLEO-L476RG, C5: PE2 on a WeAct C5 Core Board, H5: PC13 on a
WeAct H503, H7: PE3 on a WeAct H743).
There is no shared
`ds18b20_complete()`: each example implements its own, because the output
format differs.

> **The callback example below is STM32F1-specific** (Blue Pill onboard LED on
> PC13, active-low). Pin and polarity differ per MCU — see
> [Hardware Connections](hardware.md#hardware-connections): F0/G0 use PA4 active-low;
> F1 uses PC13 active-low; F3 uses PE8 **active-high** (LED4 blue on the
> F3-DISCOVERY); F4 uses PD12 **active-high** (LD4 green on the F4DISCOVERY),
> or PB2 active-high with `-DOW_F4_LED_PB2` (the WeAct F446RET6's B2 LED —
> PC13 is that board's user button, not an LED); G4 uses PA8 active-high
> (WeAct G474 Long blue LED — PC13 there is the user button too); L4 uses PA5
> **active-high** (green LD2 on the NUCLEO-L476RG); C5 uses PE2 **active-low**
> (WeAct C5 Core Board); H5 uses PC13 **active-low** (WeAct H503 blue LED); H7
> uses PE3 **active-high** (WeAct H743 blue LED). On F0/G0/G4/L4 the same logic
> targets `GPIOA` instead of `GPIOC`; C5 and H7 target `GPIOE` (PE2 and PE3).

```c
// Busy indicator — e.g. LED toggling during measurement
// STM32F1 (Blue Pill): onboard LED on PC13, active-low.
void ds18b20_busy(unsigned action) {
    if (action) {
        // Turn LED on (measurement in progress)
        GPIOC->BSRR = GPIO_BSRR_BR13;
    } else {
        // Turn LED off (measurement complete)
        GPIOC->BSRR = GPIO_BSRR_BS13;
    }
}

// Measurement complete callback — handle result or error
void ds18b20_complete(int16_t temp) {
    if (temp >= -550 && temp <= 1250) {
        // Valid temperature in tenths of °C
        printf("Temperature: %d.%d°C\n", temp/10, abs(temp%10));
    } else {
        // Error condition
        switch (temp) {
            case DS18B20_TEMP_ERROR_NO_SENSOR:
                printf("Error: No sensor detected\n");
                break;
            case DS18B20_TEMP_ERROR_CRC_FAIL:
                printf("Error: CRC check failed\n");
                break;
        }
    }
}
```
