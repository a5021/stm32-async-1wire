# WeAct STM32C5 48-pin Core Board - bring-up notes

Status: initial backend. `2_device_search` is bench-proven on the WeAct board
with seven parasite-powered DS18B20 at 144 MHz and 48 MHz HSI; the remaining
examples and the low-power path are still to be checked.

## Part and board

- **MCU:** STM32C552CET6, Cortex-M33 with FPU, 144 MHz maximum, 512 KB flash,
  128 KB SRAM, 48-pin LQFP (CET6).
- **Board:** WeAct Studio STM32C5 48-pin Core Board, schematic Rev 1.0.
- **Board hardware:** 8 MHz HSE crystal (Y2), 32.768 kHz LSE (Y1), USB-C on
  PA11/PA12, SWD on PA13/PA14, user key on PA0, LED on PE2, SPI flash on
  PA4-PA7, BOOT0 on PH2, 5 V input with a 3.3 V regulator.

## Pin plan

- **1-Wire bus:** PA10. Alternate functions in the C552 datasheet: TIM1_CH3 on
  AF1, USART1_RX on AF7, LPUART1_RX on AF3.
- **Console:** USART1_TX on PA9 (AF7). RX is not needed for the examples.
- **Logic-analyzer marker:** PA2, free on the board headers. PA11/PA12 are the
  USB-C D-/D+ pair and must not be used as the marker. The marker is opt-in;
  leave it off if the application uses PA2 for USART2 or TIM2.
- PA10 is available on the board headers.

## Clock

- After reset the CMSIS system file starts from HSIDIV (48 MHz).
- Maximum SYSCLK is 144 MHz; APB1, APB2 and APB3 also run at up to 144 MHz.
- The WeAct blink example configures **HSE 8 MHz -> PSI 144 MHz**, flash
  latency 4 WS, AHB/APB1/APB2/APB3 all divided by 1. The PSI (programmable
  speed oscillator) replaces the PLL used by the older families.
- The datasheet also lists HSI 144 MHz and PSI 160/144/100 MHz as clock
  sources. The HSI-144 path is not proven on this board yet.
- TIM1 sits on APB2. With APB2 divided by 1, `PSC = SYSCLK_MHZ - 1` gives the
  same 1 us tick as the other families: 143 at 144 MHz.

Open checks: bare-metal PSI programming, voltage scaling and flash latency at
144 MHz, and whether HSI-144 is a simpler bring-up clock.

Bench result: HSI-144 works once `HSISON` is set and `HSISRDY` is waited for;
the reset source is HSIDIV3 (48 MHz), and only HSIDIV3 is enabled after reset.
The 144 MHz path selects HSIS and programs 4 flash wait states; the 48 MHz path
leaves HSIDIV3 selected and the reset latency. APB1/2/3 stay at /1 in both.

## DMA

- The C5 has **LPDMA1** (8 channels) and **LPDMA2** (4 channels), with the same
  linked-list channel register model as GPDMA: CLBAR, CFCR, CSR, CCR, CTR1,
  CTR2, CBR1, CSAR, CDAR. Request routing is per-channel `CTR2.REQSEL`; there is
  no DMAMUX.
- Request numbers from `stm32c5xx_ll_dma.h`:
  `LL_LPDMA1_REQUEST_TIM1_CC2 = 25`, `LL_LPDMA1_REQUEST_TIM1_CC4 = 27`.
- Proposed assignment: feed on **LPDMA1_CH2** (TIM1_CC2), capture on
  **LPDMA1_CH3** (TIM1_CC4).
- Feed entries stay 8-bit: LPDMA converts widths in hardware to the CCR3
  halfword. Capture reads the CCR4 halfword into 16-bit durations. `CBR1.BNDT`
  counts bytes, so a halfword source needs `count * 2`.
- The CTR1 bit names match the H5 backend (`SDW_LOG2`, `DDW_LOG2`, `SINC`,
  `DINC`), and `CFCR.TCF/SUSPF`, `CCR.EN`, `CBR1.BNDT` are present.

Open checks: confirm the request numbers against the C5 reference manual and
that LPDMA1 channels 2/3 have no unexpected restrictions.

## CMSIS and startup

- Device package: `STMicroelectronics/stm32c5xx-dfp`, Release 2.1.0, commit
  `a5f65bc64535cfa723e9d25f58d7ce23d0937aed`.
- Headers: `Include/stm32c5xx.h`, `Include/stm32c552xx.h`,
  `Include/system_stm32c5xx.h`.
- System source: `Source/Templates/system_stm32c5xx.c`.
- Startup is **C, not assembly**: `Source/startup_stm32c552xx.c`. It uses the
  CMSIS `__PROGRAM_START` path and the `.copy.table` / `.zero.table` linker
  sections, which the repository's CMSIS core already provides.
- GCC linker: `Source/Templates/gcc/linker/stm32c552xe_flash.ld`
  (512 KB ROM at 0x08000000, 128 KB RAM at 0x20000000).
- SVD: `SVD/STM32C552.svd`.
- HAL/LL reference: `STMicroelectronics/stm32c5xx-drivers`, commit
  `7bbce49b0a50f70b3ed705061d74bdbaf508d51b`. The library itself stays
  bare-metal; the driver package is only a reference.

## Interrupts and low power

- TIM1 update vector is `TIM1_UPD_IRQn = 37` (split update vector, like H5).
- Planned low-power path is unchanged: UIE plus SEVONPEND wake the core from
  WFE, UIF remains the completion flag, and no ISR is installed.
- Open check: confirm SEVONPEND and pending-bit retirement on C5.

## Bench status

- 2026-11-08: `2_device_search` at 144 MHz HSI on the WeAct 48-pin board with
  seven parasite-powered DS18B20. Search found all seven ROMs; the temperatures
  read 26.1-26.7 C; USART1 on PA9 (AF7) printed at 115200 through a CP210x.
- 2026-11-08: the same test at 48 MHz (HSIDIV3) finds all seven ROMs and reads
  the same temperatures; `CFGR1.SWS = 0`, `PSC = 47`, `BRR = 0x1A1`.
- `1_basic` is not a valid test on this bus: it uses Skip ROM, so all seven
  sensors answer at once and the scratchpad CRC fails. That is the expected
  multi-device collision, not a C5 timing fault.

## Open questions

1. PSI vs HSI clock tree at 144 MHz, voltage scaling and flash latency.
2. LPDMA request mapping and channel restrictions.
3. TIM1 CH4 indirect capture via TI3 and `TISEL` on C5.
4. PA10 AF1 and USART1 AF7 on the actual board.
5. WFE wake-up and TIM1 update pending behaviour.
6. Flashing/debug: ST-Link over SWD, option bytes and boot selection.
7. CMSIS package pinning and license review.

## Sources

- Board repository:
  https://github.com/WeActStudio/WeActStudio.STM32C5_48Pin_CoreBoard
  at `149fc7c4183b0d049a38e5d918c19523cf8e72e3`.
- Device package:
  https://github.com/STMicroelectronics/stm32c5xx-dfp
  at `a5f65bc64535cfa723e9d25f58d7ce23d0937aed`.
- HAL/LL reference:
  https://github.com/STMicroelectronics/stm32c5xx-drivers
  at `7bbce49b0a50f70b3ed705061d74bdbaf508d51b`.
- Datasheet DS14928 Rev 1: `Doc/stm32c552ce.pdf` in the board repository.
- Board schematic Rev 1.0:
  `Hardware/WeAct-STM32C5xxCxTx_Coreboard_V10 SchDoc.pdf`.
- Clock example: `Examples/01-Blink/.../project/generated/hal/mx_rcc.c`.
