# Hardware

[Back to the README](../README.md)

## Hardware Verified

The subsections below describe the boards the driver runs on (wiring, clocks,
console). Measurement logs are deliberately not kept in the repo. Deeper
per-port bring-up and timing notes live next to the linker scripts:
[STM32F1](../port/stm32f1/HARDWARE-NOTES.md) and
[STM32F4](../port/stm32f4/HARDWARE-NOTES.md).

### STM32F103C8T6 (Blue Pill)

Reference setup: STM32F103C8T6 (Blue Pill), a DS18B20 temperature bus and a
DS2431 EEPROM sharing one 1-Wire line (PA10, external VDD; the two drivers are
exercised by separate firmware images — see the DS2431 section), flashed via
ST-Link, USART1 TX at 115200 8N1 via a CP2102 USB-UART adapter. The DS2431
path is exercised by `make APP=8_ds2431` — a Read Memory / Write Scratchpad /
Read Scratchpad / Copy Scratchpad / Read Memory cycle on every DS2431 the
search finds — and re-checked on demand with the HIL harness
(`make test-hil HIL_SCRIPT=ds2431`); the read-back matched the write pattern on
every device.

Bring-up, DMA and timing notes: [port/stm32f1/HARDWARE-NOTES.md](../port/stm32f1/HARDWARE-NOTES.md).

### STM32F030F4P6 (TSSOP20)

The same examples run on an STM32F030F4P6 minimum board: the
1-Wire bus on **PA10** (TIM1 CH3/CH4 pair — PA8 is not bonded out in this
package), USART1 TX on PA9, busy LED on PA4, flashed via ST-Link SWD. The bus
carries 8 × DS18B20. The `9_multi` bench rig uses the same board with seven
DS18B20s plus one DS2431 (parasite power); its boot capture — two Search ROM
passes, the DS2431 read, a broadcast `Convert T` with seven scratchpad reads,
and four EEPROM write chains — is the bundled sample in the
[1-Wire viewer](https://a5021.github.io/stm32-async-1wire/viewer/).

### STM32F303VC (F3-DISCOVERY / MB1035B)

Validated on an **STM32F3-DISCOVERY** (MB1035B, STM32F303VC, 256 KB flash /
32 KB SRAM): 7 × DS18B20 in parasite power mode on one 1-Wire bus on **PA10**
(TIM1 CH3/CH4 pair, DMA1 channels 3/4 — the same fixed pair as F0/F1, see the
Supported-families table), console on USART1 TX / **PA9** (default mapping) at
115200 8N1, flashed via ST-Link SWD.

### STM32G031F6P6 (WeAct TSSOP20 board)

Validated on a WeAct STM32G031F6P6 minimum board: 6 × DS18B20 in parasite
power mode on one 1-Wire bus (logical PA10 on the physical PA12 pad), USART1
TX on logical PA9 (physical PA11), flashed via ST-Link SWD. Builds: the
default 64 MHz (HSI16+PLL) and the raw-HSI16 `SYSCLK_MHZ=16` build. The bus pads
are reachable only through the SYSCFG remap described in Hardware Connections
below; the USB-C connector of this board is wired to PA11/PA12 and must stay
unplugged while the driver owns the bus.

### STM32G474CBT6 (WeAct STM32G474CBT6 Long)

Validated on a WeAct STM32G474CBT6 Long board: 7 × DS18B20 in parasite
power mode on one 1-Wire bus on **PA10** (TIM1 CH3/CH4 pair, DMA1 channels 3/4
through DMAMUX — requests 43/45, 8-bit feed tables), console on USART1 TX /
**PA9** (AF7) at 115200 8N1 via a CP2102 USB-UART adapter, flashed via ST-Link
SWD. Builds: the default 170 MHz (HSE 8 MHz + PLL M=2, N=85, R=2, Range 1 Boost)
and the raw-HSI16 `SYSCLK_MHZ=16` build. The board carries an 8 MHz crystal;
`HSE_MHZ=8` in `chips/g474cb.mk` derives the PLL dividers. The bus pads are
directly on PA10 (no remap needed). Build with `make OW_MCU=STM32G474xx`.

### STM32L476RG (NUCLEO-L476RG)

Validated on a NUCLEO-L476RG: 7 DS18B20 in parasite power plus one DS2431 on
one 1-Wire bus on **PA10** (TIM1 CH3/CH4 pair, AF1; classic DMA1 channels 3/4
with TIM1_CH2 and TIM1_CH4 both selected as request 7 in `DMA1_CSELR`),
console on USART1 TX / **PA9** (AF7) at 115200 8N1 through an external USB-TTL
adapter (the on-board VCP is USART2 and is not used), LED on PA5, flashed via
ST-Link SWD. HSE is not populated on this board, so the backend is HSI16-only:
the default 80 MHz (HSI16 16 MHz / 1 x 10 / 2, voltage Range 1, 4 flash wait
states) and the raw-HSI16 `SYSCLK_MHZ=16` build. Both were validated with
`2_device_search` (7 ROMs, 27.6-27.9 °C), `4_scan_mode`, `6_statistics` (5040
pulses, 0 errors), `7_low_power` and `8_ds2431` (read-back baseline+1), plus
the LA geometry check (`la_capture.py --tolerance-us 5`: HSI16 is a nominal
RC and its +/-1% accuracy moves the 480 us reset by up to 4.8 us). Build with
`make OW_MCU=STM32L476xx`.

### STM32F407VGT6 (STM32F4DISCOVERY)

Validated on an **STM32F4DISCOVERY** (MB997C, STM32F407VGT6, 8 MHz HSE):
7 × DS18B20 in parasite power mode on one 1-Wire bus on **PA10** (TIM1 CH3/CH4
pair, DMA2 streams 2/4 — the feed stream runs 16-bit), with the console on
USART1 TX / **PB6** at 115200 8N1 (AF7; the F4DISCOVERY carries no signal on
the default PA9 USART1 pad, so the console rides the remapped PB6) through a
USB-TTL adapter; flashed via the on-board ST-Link SWD. **PB6 is an
F4DISCOVERY-specific choice, not an F4 family default** — a WeAct F446RET6
answers with its CP210x VCP on **PB6** too (measured, not the PA9 the silkscreen
implies), so the default build works on both boards with no flag. For an F4
board whose console really is on PA9, build with
`-DOW_UART_USART1_PA9` (`make OW_MCU=STM32F446xx EXT=-DOW_UART_USART1_PA9`)
or attach the USB-TTL adapter to PB6. `SYSCLK_MHZ=16` selects raw HSI.
The F4-specific DMA topology and timing choices are documented in
[port/stm32f4/HARDWARE-NOTES.md](../port/stm32f4/HARDWARE-NOTES.md).

### STM32F401CC (F401 Black Pill)

The **same `src/ow_mcu.h` F4 section** drives the 84 MHz-capped STM32F401CC: TIM1,
the DMA2 dual-stream topology and the `CHSEL=6` request map are identical to
the F407 (RM0368 §9.3.3 Table 29 via
[port/stm32f4/HARDWARE-NOTES.md](../port/stm32f4/HARDWARE-NOTES.md)), so the
port layer is chip-generic. The chip-specific parts (CMSIS device header
`stm32f401xc.h`, `startup_stm32f401xc.s`, the `STM32F401CC_FLASH.ld` linker
script, the `app.c` 84 MHz HSE+PLL clock-config branch, and the library's 84 MHz
clock default) are selected with `OW_MCU=STM32F401xC` — all of it in
`chips/f401xc.mk`.

Bench-validated on a WeAct F401 Black Pill at the 84 MHz default (25 MHz
crystal, HSE+PLL); `SYSCLK_MHZ=16` remains the no-crystal raw-HSI fallback.

**The crystal is the one thing the part cannot decide.** This board carries a
**25 MHz** crystal, not the 8 MHz the F407DISCOVERY does, and the F4 PLL divides
the crystal down before multiplying: `PLLM = HSE_MHZ` puts the PLL input at 1 MHz
and `PLLN = 2 × SYSCLK` sets the VCO, so 25 MHz gives M=25/N=168 for the same
84 MHz that 8 MHz reaches with M=8/N=168. `chips/f401xc.mk` therefore declares
`CHIP_HSE_MHZ = 25`; every other board passes `HSE_MHZ=<n>` to the build. This
matters more than it looks: with the divider hardcoded to 8, a 25 MHz crystal
puts 3.125 MHz into a PLL input specified for 1–2 MHz, the PLL never locks, and
the old unbounded wait left the board silent with no way to tell a dead board
from a wrong one.

Expected wiring matches the F407: bus on **PA10** (TIM1 CH3/CH4, DMA2 streams
2/4), parasite mode on the same 2.2 kΩ pull-up, and the console on **PB6** —
not PA9, because the F4 UART path is pinned to PB6 for the F4DISCOVERY, which
carries no signal on the default PA9 USART1 pad.

### STM32F446RE (WeAct F446RET6)

Validated on a **WeAct STM32F446RET6** (ST-Link V2J45S7, STM32F446 Rev A,
512 KB flash): 7 × DS18B20 in parasite power mode on one 1-Wire bus on **PA10**
(TIM1 CH3/CH4 pair, same DMA2 streams 2/4 topology as the F407), console on
USART1 TX / **PB6** (the board's CP210x VCP answers there at 115200 8N1 — the
default build needs no UART flag), flashed via OpenOCD over the on-board
ST-Link.

### STM32C552CET6 (WeAct 48-pin Core Board)

Validated on a WeAct STM32C5 48-pin Core Board: 7 × DS18B20 in parasite power
plus one DS2431 on one 1-Wire bus on **PA10** (AF1; TIM1 CH3/CH4 pair, LPDMA1
channels 2/3 — requests 25/27, no DMAMUX, 8-bit feed tables), console on
USART1 TX / **PA9** (AF7) at 115200 8N1 via a CP210x USB-UART adapter, busy
LED on **PE2** (active-low), flashed via ST-Link SWD and STM32CubeProgrammer
2.23. The board carries an 8 MHz crystal (Y2).

Clock paths: the default 144 MHz from **HSE -> PSI** (PSI reference 8 MHz,
output 144 MHz, 4 flash wait states), the **48 MHz HSIDIV3** reset clock, and
raw **8 MHz HSE**. All three were validated with apps 2–9; `0_dma_pressure`
ran 100 frames at each clock with `cndtr_late=0` and no DMA errors, and
`8_ds2431` completed its EEPROM write/read-back. The DS18B20 readings were
26.1–26.7 °C. Build with `make OW_MCU=STM32C552xx` (144 MHz), add
`SYSCLK_MHZ=48` for HSIDIV3, or `SYSCLK_MHZ=8 HSE_MHZ=8` for raw HSE.

### STM32H503CB (WeAct STM32H503Cx Core Board)

Validated on a WeAct STM32H503Cx Core Board: 7 × DS18B20 in parasite
power mode on one 1-Wire bus on **PA10** (AF1; TIM1 CH3/CH4 pair, GPDMA1
channels 2/3 — requests 59/61, no DMAMUX, 8-bit feed tables), console on
USART1 TX / **PA9** (AF7) at 115200 8N1 via a USB-TTL adapter, flashed via
ST-Link SWD. Builds: the default 64 MHz raw HSI, the raw-HSE `SYSCLK_MHZ=8`
build, and 250 MHz via HSE+PLL1 (M=HSE/4, N=125, P=2, VOS0, 5 flash wait
states). The board carries an 8 MHz crystal (Y2); `HSE_MHZ=8` in
`chips/h503cb.mk` derives the PLL dividers. All seven DS18B20 examples were flashed
and captured at each clock with correct 22.8-23.1 °C readings and 0 errors in
`6_statistics`. The busy LED on **PC13** is **active-low** (blue LED on the
WeAct board). Stress experiment: at level 5 on 8/64/250 MHz a PRIO 3 load
starves the default PRIO 0 feed/capture; raising feed/capture to PRIO 3 makes
all three clocks clean at guard 1/5/100 (`cndtr_late=0`). The library default
remains PRIO 0; `OW_PORT_DMA_PRIO=3` is the build knob that raises both
channels (the same knob selects `DMA_SxCR_PL` on F4/H7 and `DMA_CCR_PL` on the
classic DMA1 backends).
Build with `make OW_MCU=STM32H503xx`.

### STM32H743VI (WeAct Studio STM32H7XX Core Board)

Validated on a WeAct Studio STM32H7XX (STM32H743VI, ST-Link, 2 MB flash): 7 DS18B20 in parasite
power mode on one 1-Wire bus on **PA10** (AF1; TIM1 CH3/CH4 pair, DMA1 streams
2/3 through DMAMUX1 — requests 12/14, 16-bit feed tables), console on
USART1 TX / **PA9** (AF7) at 115200 8N1 via a USB-TTL adapter, flashed via
ST-Link SWD. Clock: 240 MHz via HSE+PLL (M=HSE/5, N=96, P=2 through PLL1P,
LDO supply setup, VOS1→VOS0 on Rev V, 4 flash wait states; 25 MHz crystal,
`HSE_MHZ=25` in
`chips/h743vi.mk`). All five HIL scripts passed on the parasite bench
(`smoke` 7 devices, `scan` 20 rounds, `stats` 70 cycles / 5040 pulses /
0 errors, `ds2431` 2 devices read back baseline+1, `lowpower` 6 readings),
and the wire geometry matches the configuration on the analyzer (5.0 / 60.0 us
slots, 166.0 us parasite slot, 481.0 us reset, 750.27 ms conversion wait).

The opt-in 480/240 MHz split (`SYSCLK_MHZ=480 TIM1_MHZ=240`: core 480 MHz,
HCLK 240 MHz, TIM1 240 MHz) was validated on the same rig with the same five
scripts (`smoke` 7 devices / 13 readings, `scan` 19 rounds, `stats` 70 cycles /
5040 pulses / 0 errors, `ds2431` baseline+1, `lowpower` 6 readings) and the
same wire geometry on the analyzer (5.0 / 60.0 us pulses, 166.0 us slot,
481.0 us reset, 750.27 ms conversion wait). The register read-back showed
`PLL1DIVR.DIVP1 = 0` (P=1), `D1CFGR` D1CPRE=/1 and HPRE=/2, and
`PWR_CSR1.ACTVOS = 11` (VOS0) with `FLASH_ACR` at 4 wait states.

The busy LED on **PE3** is **active-high** (blue LED on the WeAct board).
Build the default with `make OW_MCU=STM32H743xx`, the split clock with
`make OW_MCU=STM32H743xx SYSCLK_MHZ=480 TIM1_MHZ=240`.

## Hardware Connections

Logic-analyzer marker used by the DMA-pressure rig: PA2 on the F030 and PA11 on
the F446/H503 (`STRESS_MARKER=1`); see
[Examples](examples.md#measuring-dma-feed-margin-0_dma_pressure).

### Connections: STM32F103 (Blue Pill)

#### DS18B20 Sensor

| STM32F103 Pin | Function     | DS18B20 Pin |
|---------------|--------------|-------------|
| PA10          | 1-Wire Data  | DQ (Data)   |
| 3.3 V          | Power        | VDD         |
| GND           | Ground       | GND         |

Note: A 4.7 kΩ pull-up resistor is required between the PA10 and 3.3 V lines.

#### DS2431 EEPROM

Same bus as the DS18B20 (one 1-Wire line, shared pull-up):

| STM32F103 Pin | Function    | DS2431 Pin |
|---------------|-------------|------------|
| PA10          | 1-Wire Data | DQ (Data)  |
| 3.3 V          | Power       | VDD        |
| GND           | Ground      | GND        |

Both slaves can sit on the same line electrically, and one firmware image can
drive both: finish and retire an operation on one driver before the other starts
one, and never poll both at once. Both inits may run before any bus operation
(each `onewire_init()` re-applies the same idle configuration); `9_multi` does
this and then hands the bus over through one explicit owner state machine. The
DS2431 example runs the shared bus search with `DS2431_FAMILY_CODE` (0x2D), so
DS18B20s on the line are ignored; the DS18B20 driver's own search filters on
0x28.

#### Debug UART (optional)

| STM32F103 Pin | Function            | USB-UART Adapter |
|---------------|---------------------|------------------|
| PA9           | USART1 TX (115200)  | RX               |
| GND           | Ground              | GND              |

Connect a USB-UART adapter to see diagnostic output (sensor errors,
temperature readings). No RX connection is needed — the firmware is
transmit-only.

The UART output uses a ring buffer with polled TX (TXE flag checked
in main loop) — fully non-blocking, no interrupts.

#### Busy LED and SWD

| Pin | Function | Notes |
|-----|----------|-------|
| PC13 | Busy LED (optional) | Active-low (Blue Pill onboard LED) |
| PA13/PA14 | SWDIO/SWCLK | ST-Link SWD programming |

### Connections: STM32F030 (e.g. STM32F030F4P6, TSSOP20)

| Pin  | Function            | Notes                              |
|------|---------------------|------------------------------------|
| PA10 | 1-Wire Data         | TIM1_CH3, open-drain AF2 (default topology) |
| PA9  | USART1 TX (115200)  | RX line of the USB-UART adapter    |
| PA4  | Busy LED (optional) | Active-low                         |
| PA13/PA14 | SWDIO/SWCLK    | ST-Link SWD programming            |

Note: the same 4.7 kΩ pull-up is required between PA10 and 3.3 V.

### Connections: STM32F303VC (F3-DISCOVERY / MB1035B)

| Pin | Function | Notes |
|-----|----------|-------|
| PA10 | 1-Wire Data | TIM1_CH3, open-drain AF6 after the default AF map |
| PA9  | USART1 TX (115200) | AF7; RX line of the USB-UART adapter (the on-board ST-Link VCP is on PA2/PA3) |
| PE8  | Busy LED (optional) | **Active-high** (LED4 blue) — the opposite polarity of F0/F1/G0 |
| PA13/PA14 | SWDIO/SWCLK | ST-Link SWD programming |

Note: the board's 8 MHz clock is driven onto OSC_IN by the ST-Link (bypass
mode, not a crystal), which is what the 72 MHz default is built against; the
64 MHz and 8 MHz builds need no external clock.

### Connections: STM32G031F6P6 (TSSOP20)

The STM32G0 backend is hardware-validated (see Hardware Verified above); the
notes below cover the TSSOP20 package wiring:

| Pin | Function | Notes |
|-----|----------|-------|
| PA12 | 1-Wire Data (logical PA10) | TIM1_CH3 AF2 after the SYSCFG `PA12_RMP` remap; open-drain AF (default topology) |
| PA11 | USART1 TX (logical PA9) | AF1 after the `PA11_RMP` remap |
| PA4 | Busy LED (optional) | Active-low |
| PA13/PA14 | SWDIO/SWCLK | ST-Link SWD programming |

Important: while the driver is initialised, pads PA11/PA12 must not be used as standalone GPIOs - configuring them as PA11/PA12 clears the SYSCFG remap bits and silently disconnects the bus.

Note: the same 4.7 kΩ pull-up is required between the bus pin and 3.3 V.

Note: "open-drain" above describes the **default/idle** bus topology, not a
static pin configuration. With the optional active-drive write mode
(`-DOW_DRIVE_ACTIVE=1`) the pin is temporarily switched to push-pull during
master-only write slots and restored to open-drain afterwards — see
[Bus Electrical Model](architecture.md#bus-electrical-model).

### Connections: STM32G474CBT6 (WeAct STM32G474CBT6 Long)

| Pin  | Function            | Notes                              |
|------|---------------------|------------------------------------|
| PA10 | 1-Wire Data         | TIM1_CH3 AF6, open-drain (default topology) |
| PA9  | USART1 TX (115200)  | AF7; RX line of the USB-UART adapter |
| PA8  | Busy LED (optional) | **Active-high** (blue LED on WeAct board; CxT6 = PA8 per WeAct BSP) |
| PA13/PA14 | SWDIO/SWCLK    | ST-Link SWD programming            |

Note: the same 4.7 kΩ pull-up is required between the bus pin and 3.3 V.
The board carries an **8 MHz crystal** (`HSE_MHZ=8` in `chips/g474cb.mk`);
the 170 MHz PLL (M=2, N=85, R=2, Range 1 Boost) and the raw-HSI16
`SYSCLK_MHZ=16` build are supported. Build with `make OW_MCU=STM32G474xx`.
PC13 on this board is the user button (pull-up to VCC), not an LED.

### Connections: STM32L476RG (NUCLEO-L476RG)

| Pin  | Function            | Notes                              |
|------|---------------------|------------------------------------|
| PA10 | 1-Wire Data         | TIM1_CH3 **AF1**, open-drain (default topology) |
| PA9  | USART1 TX (115200)  | AF7; RX line of the external USB-TTL adapter |
| PA5  | Busy LED (optional) | **Active-high** (green LD2 on the NUCLEO) |
| PA13/PA14 | SWDIO/SWCLK    | ST-Link SWD programming            |
| GND  | Ground              | Common with the USB-TTL ground      |

Note: the same 4.7 kΩ pull-up is required between the bus pin and 3.3 V.
The NUCLEO-L476RG has **no HSE crystal populated**, so only the internal HSI16
clocks are supported: 80 MHz via HSI16+PLL (default) and raw HSI16 at 16 MHz.
The on-board ST-Link VCP is USART2 (PA2/PA3) and is deliberately not used; wire
the external USB-TTL adapter to PA9 and GND. Build with
`make OW_MCU=STM32L476xx`.

### Connections: STM32F407VGT6 (STM32F4DISCOVERY)

The STM32F4 backend is hardware-validated (see Hardware Verified above). The
bus pin defaults to PA10 (the same wiring as F0/F1). The F4DISCOVERY board
carries no signal on the default PA9 USART1 pad, so the console rides USART1
TX remapped to PB6 (AF7); see the complete build line in Hardware Verified:

| Pin  | Function            | Notes                              |
|------|---------------------|------------------------------------|
| PA10 | 1-Wire Data         | TIM1_CH3, open-drain AF1; also OTG_FS_ID |
| PB6  | USART1 TX (115200)  | USART1 AF7 (remapped from PA9); RX line of the USB-UART adapter |
| PD12 | Busy LED (optional) | **Active-high** (F4DISCOVERY LD4: pin → LED → GND); the F4 console is on PB6, not the F0/F1/G0 PA4 pin |
| PA11 | Free (former LA marker) | Left untouched by the driver; logic-analyzer sync, if needed, goes through the opt-in OW_PORT_MARKER_TOGGLE() hook (no-op by default); also USB OTG FS D− |
| PA13/PA14 | SWDIO/SWCLK    | ST-Link SWD programming            |

Important: the board's USB OTG FS connector is wired to the pins above —
**do not plug a USB cable into it while the driver runs**. A standard A-cable
grounds OTG_FS_ID on **PA10** and holds the 1-Wire bus LOW; **PA11** is USB D−
and **PA12** is USB D+, so a connected cable also conflicts with any use of PA12.

Note: a pull-up is required between the bus pin and the supply — 4.7 kΩ for a
single device, 2.2 kΩ for a parasite-powered multi-drop fleet (fed from a
dedicated supply; see the `4_scan_mode` note). The board's 8 MHz HSE drives
the default 168 MHz PLL clock.

### Connections: STM32F446RE (WeAct F446RET6)

The F446 backend is hardware-validated over its full clock range (see the
F446 entry in Hardware Verified). Bench wiring on this board:

| Pin  | Function            | Notes                              |
|------|---------------------|------------------------------------|
| PA10 | 1-Wire Data         | TIM1_CH3, open-drain AF1 (same topology as the F407) |
| PB6  | USART1 TX (115200)  | AF7; the on-board CP210x VCP answers here on the default build |
| PB2  | Busy LED (optional) | Active high; silkscreen "B2". Select with `-DOW_F4_LED_PB2` (`make OW_MCU=STM32F446xx EXT=-DOW_F4_LED_PB2`) |
| PA13/PA14 | SWDIO/SWCLK    | On-board ST-Link SWD programming            |

Important: the board's user button is on **PC13** (pull-up to VCC, not an LED),
and the bus pull-up for a parasite-powered fleet is 2.2 kΩ fed from a dedicated
supply, as on the F407. The F446 bench uses the same 8 MHz crystal and reaches
180 MHz only through the PWR over-drive sequence (see
[Clocking invariant](architecture.md#4-clocking-invariant)).

### Connections: STM32H503CB (WeAct STM32H503Cx Core Board)

| Pin  | Function            | Notes                              |
|------|---------------------|------------------------------------|
| PA10 | 1-Wire Data         | TIM1_CH3 AF1, open-drain (default topology) |
| PA9  | USART1 TX (115200)  | AF7; RX line of the USB-UART adapter |
| PC13 | Busy LED (optional) | **Active-low** (blue LED on WeAct board) |
| PA13/PA14 | SWDIO/SWCLK    | ST-Link SWD programming            |

Note: the same 4.7 kΩ pull-up is required between the bus pin and 3.3 V.
The board carries an **8 MHz crystal** (Y2; `HSE_MHZ=8` in `chips/h503cb.mk`);
the 250 MHz PLL1 (M=HSE/4, N=125, P=2, VOS0, 5 flash wait states) and the
raw-HSE `SYSCLK_MHZ=8` build are supported. Build with `make OW_MCU=STM32H503xx`.
There is no J-Flash project for this part (JTAG CoreID unverified) — flash
via ST-Link (see `port/stm32h5/project-h503cb.jdebug`).

### Connections: STM32H743VI (WeAct Studio STM32H7XX Core Board)

| Pin  | Function            | Notes                              |
|------|---------------------|------------------------------------|
| PA10 | 1-Wire Data         | TIM1_CH3 AF1, open-drain (default topology) |
| PA9  | USART1 TX (115200)  | AF7; RX line of the USB-UART adapter |
| PE3  | Busy LED (optional) | **Active-high** (blue LED on WeAct board) |
| PA13/PA14 | SWDIO/SWCLK    | ST-Link SWD programming            |

Note: the parasite-powered bench runs a 2.2 kΩ pull-up from a dedicated
supply; a normally powered bus takes the same 4.7 kΩ pull-up to 3.3 V as the
other families. The board carries a **25 MHz crystal** (`HSE_MHZ=25` in
`chips/h743vi.mk`); default is 240/240 MHz CPU/TIM1. Rev V also supports an
opt-in 480/240 MHz split (`SYSCLK_MHZ=480 TIM1_MHZ=240`): CPU=480 MHz, HCLK=240 MHz,
TIM1=240 MHz. Both use the LDO supply, VOS1→VOS0 and 4 flash wait states. Build
the split mode with `make OW_MCU=STM32H743xx SYSCLK_MHZ=480 TIM1_MHZ=240`.
There is no J-Flash project for this part — flash via ST-Link.
