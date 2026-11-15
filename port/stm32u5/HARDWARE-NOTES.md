# STM32U585CIU6 bring-up notes

Bare STM32U585CIU6 (UFQFPN48) on the bench adapter, ST-Link V2J45S7 (SWD),
CP210x USB-UART on USART1 PA9 (COM5 at 115200 8N1), Saleae Logic on D0.
Seven DS18B20 on one parasite-powered bus on PA10 (AF1, TIM1 CH3/CH4,
GPDMA1 channels 2/3, `REQSEL` 43/45). Busy LED on PC13, active-low.
25 MHz crystal on PH0/PH1.

## Option bytes (2026-11-15)

```
RDP   : 0xAA (Level 0, no protection)
TZEN  : 0x0  (Global TrustZone security disabled)
```

The build compiles secure (`-mcmse`), so the CMSIS `*_S` aliases are used and
the same image works whether TZEN is 0 or 1.

## Clocks

Default is 160 MHz: 25 MHz HSE -> PLL1 (M=5, 5 MHz input, RGE 4..8 MHz;
N=64, 320 MHz VCO; R=2, SYSCLK = PLL1R). VOS range 1 and the EPOD booster are
required; flash latency is 4 WS. `PLL1MBOOST=DIV2` (12.5 MHz EPOD input) works
on this part. U5 takes SYSCLK from **PLL1R**, not PLL1P, so `PLL1REN` must be
set and the R divider is the one that matters.

The 16 MHz HSI16 build (`SYSCLK_MHZ=16`) is the fallback: `FLASH_ACR =
PRFTEN | LATENCY_1WS`, `HSION`, `HSIRDY`, `CFGR1.SW = 01`.

The reset MSI 4 MHz is only the failure state: a failed HSE/PLL/EPOD wait
leaves the tree there and `app_init()` prints the error at the 4 MHz console
divisor. A 4 MHz *build* is not supported: on this bench the polled read path
returns CRC failures at 4 MHz, so the supported clocks are 160 and 16 MHz.

## Bench results

All with `EXT="-DOW_PARASITE_POWER=1"`, default 160 MHz unless noted.

- `make test-hil HIL_MCU=STM32U585xx HIL_SCRIPT=smoke HIL_PORT=COM5`:
  PASS, 7 devices, 13 readings, 25.6..25.9 C.
- `scan`: PASS, 20 rounds, up to 7 devices.
- `stats`: PASS, 2 dumps, 7 sensors, 70 cycles, 5040 pulses, 0 errors
  (needs `HIL_TIMEOUT=120`; ten sweeps of seven sensors take ~55 s).
- `lowpower` (`7_low_power` + `-DOW_LOW_POWER=1`): PASS, 6 readings,
  25.6..26.0 C.
- `HIL_SYSCLK_MHZ=16` smoke: PASS, 7 devices, 13 readings, 25.6..26.1 C.
- LA geometry at 160 MHz (`la_capture.py --parasite --tolerance-us 8`):
  slot 166.00 us, ONE 5.00 us, ZERO 60.00 us, reset 481.25 us, presence
  157.2 us.
- LA content (`la_decode.py`): MATCH ROM + READ SCRATCHPAD with CRC ok,
  26.0000 C, `ИТОГ: PASS`.
- DS2431 (`HIL_SCRIPT=ds2431`): PASS, 1 device, 2 EEPROM reads, read-back is
  baseline+1 at 160 MHz.
- DS2413 (`HIL_SCRIPT=ds2413`): PASS, 11 blinks, commands
  `12121212121`, read-back `12121212121`, no errors. The open-drain PIOA/PIOB
  pins need external pull-ups; without them both read 0.
- LA content (`la_decode.py` on `8_ds2431`): SEARCH ROM sees the DS2413
  (`3A 9F 47 03 00 00 00 3A`) and the DS2431 (`2D F8 47 60 26 00 00 69`),
  both CRC ok; `ИТОГ: PASS`. The decoder does not yet understand the DS2413
  PIO commands (`0x5A`/`0xF5`), so a `9_multi` capture stops there by design.

The `--wait-ms 750` check is meant for an example whose longest pause is the
conversion wait. On `2_device_search` the application's inter-scan pause is
several seconds, so the check reports that longer gap; run it on
`6_statistics`/`1_basic` if the wait duration itself is the target.

## Flashing

CubeProgrammer parses `.hex` correctly:

```
STM32_Programmer_CLI -c port=SWD -w build/ds18b20_2_device_search.hex 0x08000000 -v -rst -run
```

`st-flash write file.hex` does **not** parse Intel HEX: it writes the ASCII
text to flash (the vector table then reads `0x30303030`). Either pass
`--format ihex` or flash the `.bin`:

```
st-flash --reset write build/ds18b20_2_device_search.bin 0x08000000
```

## Not covered yet

- No stress/DMA-pressure measurements for U5.
