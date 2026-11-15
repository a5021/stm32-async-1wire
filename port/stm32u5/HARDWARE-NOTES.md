# STM32U585CIU6 bring-up notes

Bare STM32U585CIU6 (UFQFPN48) on the bench adapter, ST-Link V2J45S7 (SWD),
CP210x USB-UART on USART1 PA9 (COM5 at 115200 8N1), Saleae Logic on D0.
Seven DS18B20 on one parasite-powered bus on PA10 (AF1, TIM1 CH3/CH4,
GPDMA1 channels 2/3, `REQSEL` 43/45). Busy LED on PC13, active-low.

## Option bytes (2026-11-15)

```
RDP   : 0xAA (Level 0, no protection)
TZEN  : 0x0  (Global TrustZone security disabled)
```

The build compiles secure (`-mcmse`), so the CMSIS `*_S` aliases are used and
the same image works whether TZEN is 0 or 1.

## Clocks

16 MHz HSI16 is the default: `FLASH_ACR = PRFTEN | LATENCY_1WS` (VOS range 4
at reset), `RCC_CR.HSION`, `HSIRDY`, `CFGR1.SW = 01`. The reset MSI 4 MHz is
the fallback if HSI16 never becomes ready. No crystal is populated, so there
is no HSE path and no `OW_HSE_MHZ`.

## Bench results

All with `EXT="-DOW_PARASITE_POWER=1"` unless noted.

- `make test-hil HIL_MCU=STM32U585xx HIL_SCRIPT=smoke HIL_PORT=COM5`:
  PASS, 7 devices, 13 readings, 25.3..25.6 C.
- `scan`: PASS, 19 rounds, up to 7 devices.
- `stats`: PASS, 2 dumps, 7 sensors, 70 cycles, 5040 pulses, 0 errors
  (needs `HIL_TIMEOUT=120`; ten sweeps of seven sensors take ~55 s).
- `lowpower` (`7_low_power` + `-DOW_LOW_POWER=1`): PASS, 6 readings,
  25.3..25.5 C.
- LA geometry (`la_capture.py --parasite --wait-ms 750 --tolerance-us 8`):
  slot 166.25 us, ONE 5.00 us, ZERO 60.00 us, reset 485.0 us (expected 480;
  HSI16 is ~1% slow, still above the 480 us minimum), conversion wait
  751.95 ms (+1.95 ms), presence 157 us.
- LA content (`la_decode.py`): MATCH ROM + CONVERT + READ SCRATCHPAD with
  CRC ok, 25.5625 C, `ИТОГ: PASS`.

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

- 160 MHz HSI16+PLL1 (VOS1 + EPOD boost + 4 WS) is not implemented.
- No DS2431/DS2413 on this bench.
- No stress/DMA-pressure measurements for U5.
