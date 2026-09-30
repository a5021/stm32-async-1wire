# F446RET6 bench — OC3PE sweep (Step C)

*Date:* 2026-09-30
*Board:* WeAct STM32F446RET6 (ST-LINK V2J45S7, 3.23 V, STM32F446 Rev A, 512 KiB flash)
*Console:* 115200 C2 8N1 on CP210x UART bridge (host COM5)
*Probe:* OpenOCD (ST-LINK), Saleae clone over sigrok-cli/D0+D1
*Bus:* 7x DS18B20 in parasite-power wiring, PA10 (TIM1_CH3) + IC4 capture

## Question

Does the OC3PE output-compare preload bit matter on the F446RET6 bench?
`-DOW_PORT_OC3PE=0` removes the preload from `ow_port_capture`,
`ow_port_read_pair` and the single-slot branch of `ow_port_write_slots`
(see the entry under `### Changed` in CHANGELOG.md).

## Build-verification gotcha (why this bench exists)

`EXT` (the `-D...` user flags) is not tracked by the object-name stamp in
Makefile. Rebuilding the same target with a *different* `EXT` silently reuses
the previous build's objects — the exact "compiles clean, links, and is wrong
on hardware only" failure the stamp comment warns about. This sweep was first
poisoned by it: the "OC3PE=1" rebuild after the OC3PE=0 flash still linked the
OC3PE=0 objects, so the board "behaved like OC3PE=0" regardless of the flag.
Both sweep images below were disassembled to confirm the `TIM_CCMR2` immediate
before flashing (0xB278 with OC3PE on, 0xB270 with the bit stripped).
The Makefile now folds `EXT` into `OBJ_STAMP` so this cannot happen again.

## Result

| Build | CCMR2 mask | 2_device_search | 1_basic |
|---|---|---|---|
| `-DOW_PORT_OC3PE=1` (default) | 0xB278 | 7/7 sensors, ~21.0-21.3 C, CRC ok | CRC check failed (Skip-ROM broadcast across 7 devices = expected contention, not a driver bug) |
| `-DOW_PORT_OC3PE=0` | 0xB270 | measurement stalls | UART silent |

### OC3PE=0 — failure signature

- UART: no output at all.
- State machine: `ctx.current_state` frozen at `CONVERT` (0x02).
- TIM1: `CEN=0`, `SR=0x0F` persistent (UIF never consumed by
  `ds18b20_poll()` → the PSR-search/res/txn guard sits in front of it),
  `CCR3=0`.
- Logic analyzer: bus HIGH for the whole capture, no pulses after boot.
- CPU: alive, main loop spinning (PC inside `uart_poll_tx`/`app_millis`).

Interpretation: without OC3PE the preload is gone, so at the terminal update
stop the direct-write/capture paths cannot end cleanly on idle-HIGH; the first
slot sequence never completes and the state machine parks in CONVERT forever.
OC3PE=0 is therefore **not a usable bus-release mode** on this port — the
preload is load-bearing, as the (now parameterized) historical behaviour said.

## Frequency matrix (pre-commit gate)

All 7 examples at every supported F446 clock: 180 (HSE+PLL+over-drive, default),
168, 84 (HSE+PLL), 16 (raw HSI), 8 (raw HSE = `OW_HSE_MHZ`). 35 firmware images,
one 40 s UART capture each, straight `make` from the fixed `OBJ_STAMP` (no `make
clean` needed between configs — EXT and SYSCLK are now part of the stamp).

Build: `make APP=<n> OW_TARGET=f4 OW_CHIP=f446xx SYSCLK_MHZ=<f> EXT="-DOW_PARASITE_POWER=1"`
(`7_low_power` adds `-DOW_PORT_LOW_POWER=1`, `6_statistics` auto-adds
`OW_STATS_ENABLE=1`). Scope: all apps default OC3PE=1.

| Frequency | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|
| 180 MHz | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 168 MHz | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 84 MHz  | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 16 MHz  | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 8 MHz   | PASS | PASS | PASS | PASS | PASS | PASS | PASS |

**35/35 PASS.** Every image flashed with `** Verified OK **`, clean console text
at all five clock's UART divisors, all 7 bench ROMs enumerated in the
search-based examples with valid CRC8 temps, no lockups (every log still
producing output at the end of its 40 s window). `1_basic` shows the expected
Skip-ROM broadcast "CRC check failed" on all five clocks (same accepted
behaviour as the F3 matrix), no other errors.

Checksum with error-free measurement: `2_device_search` reads 7/7 at every
clock, ~21.1-21.3 C, matching the OC3PE=1 sweep at 180 MHz in this directory.

## Logs

- `_matrix_summary.txt` — per-run PASS/FAIL and capture-line counts for the 35-run matrix.
- `1_basic_<f>.log` ... `7_low_power_<f>.log` (`<f>` = 180/168/84/16/8) — one 40 s capture per (example, clock).
- `2_device_search_oc3pe1.log` — full 7-sensor readout, two passes, no CRC errors (pre-matrix sweep).
- Manifest: all images rebuilt from a `make clean` tree, disassembly-verified,
  flashed via `openocd program ... verify reset exit` (`** Verified OK **`).