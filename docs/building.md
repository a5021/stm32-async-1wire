# Building, testing and integration

[Back to the README](../README.md)

## Building

### Prerequisites

-   **Toolchain:** `arm-none-eabi-gcc` (GCC 12+ recommended) and related
    utilities (`objcopy`, `size`). Clang is **not** a supported firmware
    toolchain — Clang references in this project refer to optional
    host-side tooling only (fuzz testing, `clang-format`, static analysis).
-   **wget** (or **curl**): Required for downloading CMSIS build dependencies.
-   **Host toolchain** for the test and lint suites: **gcc** (`make test*`,
    `make test-mocks`, `make test-manifest`), **clang with libFuzzer**
    (`make fuzz-all`), **cppcheck** (CI static analysis), **doxygen** (API docs),
    **lcov** (only to turn the `.gcda` files from `COVERAGE=1` into a report — CI
    does this step, there is no Makefile target for it), and **python3**
    (`make test-manifest`).
-   **cmake** ≥ 3.14: only for the CMake build path (`find_package` /
    FetchContent); the Makefile path does not need it.
-   **clang-format**: use the exact version CI pins (see the `format` job in
    `.github/workflows/ci.yml`) — a different release can disagree about the same
    source.
-   **POSIX shell:** On Linux/macOS any shell works; on Windows the `Makefile`
    targets (including `make download-deps`) need a Linux-like environment —
    Git Bash, MSYS2 or WSL. `cmd`/PowerShell are **not** supported for builds.
-   **Programmer tools:**
    -   **ST-LINK:** `st-flash` (Linux/macOS) or `ST-LINK_CLI.exe` (Windows)
    -   **J-LINK:** `JFlashExe` / `JFlash.Exe` / `JLinkGDBServerCL.exe`

> **Building on Windows.** The `Makefile` uses POSIX shell constructs
> (`mkdir -p`, `rm -rf`, `awk`, `sleep`, `command -v`) and downloads
> dependencies via `wget`/`curl`, so the build itself cannot run from native
> `cmd`/PowerShell. Use Git Bash, MSYS2 or WSL and ensure `make`, `wget` (or
> `curl`) and `arm-none-eabi-gcc` are on `PATH`. The Windows-native flashers
> listed above are only invoked by `make program` / `make jprogram` — they do
> not replace the POSIX build environment.

### CMSIS Dependencies

ARM CMSIS core headers and STM32 device files (F0/F1/F3/F4/G0/G4/H5) are not stored in the
repository. They are downloaded automatically at build time to
`CMSIS/core/` and `CMSIS/device/`:

```bash
make download-deps
```

To remove them (this also drops a root-level CMake FetchContent clone directory
`_deps/`; the `build/_deps/` tree that a normal `cmake -B build` creates is
removed by `make clean`):

```bash
make clean-deps
```

License files are also downloadable:

```bash
make download-licenses
```

### Build

```bash
make            # Release build (-Os -flto -g0)
make debug      # Debug build (-Og -g3 -gdwarf)
```

Output goes to `build/` (`ds18b20_<app>.elf`, `.hex`, `.bin` — e.g. `ds18b20_1_basic.elf` for the default `APP=1_basic`).

### Common Targets

| Target | Description |
|--------|-------------|
| `make` / `make all` | Build release |
| `make debug` | Build with debug symbols |
| `make test` | Build and run host tests (PC toolchain) |
| `make test-all-families` | Run the plain host suite once per backend in `OW_KNOWN_TARGETS` (f0/f1/f3/f4/g0/g4/l4/h5/h7), the local equivalent of the CI family matrix |
| `make check` | Pre-push gate: formatting, `cppcheck`, version declarations, the HIL parsers and the published stress report; needs no compiler or ARM toolchain |
| `make test-stress-report` | Regenerate the stress report from `docs/stress/results.json`, compare it with the committed page, and decode every linked capture (no hardware) |
| `make format-check` | Check formatting with the file list and `clang-format` version the `format` job uses |
| `make format` | Rewrite the same files in place |
| `make cppcheck` | Static analysis, one pass per compiled configuration (run `make deps-all` first) |
| `make clean` | Remove build artifacts, including any CMake install prefix outside `build/` |
| `make download-deps` | Download CMSIS dependencies |
| `make deps-all` | Download CMSIS for every family at once, including the F446 part the F407 download does not carry (needed by `make cppcheck`, and by the full host-test matrix in CI) |
| `make clean-deps` | Remove downloaded dependencies (`CMSIS/`, plus a root-level `_deps/`; `build/_deps/` goes with `make clean`) |
| `make program` | Flash via ST-LINK |
| `make jprogram` | Flash via J-LINK |
| `make test-f0` | Build and run host tests against the STM32F0 backend mock |
| `make test-f3` | Build and run host tests against the STM32F3 backend mock |
| `make test-g0` | Build and run host tests against the STM32G0 backend mock |
| `make test-f4` | Build and run host tests against the STM32F4 backend mock |
| `make test-g4` | Build and run host tests against the STM32G4 backend mock |
| `make test-h5` | Build and run host tests against the STM32H5 backend mock |
| `make test COVERAGE=1` | Host tests with gcov instrumentation (`.gcno`/`.gcda` land in the tree root; turning them into a report is a CI step — `lcov --capture` + `genhtml` — not a Makefile target) |
| `make test-active` | Build and run host tests for the active-drive write path (`-DOW_DRIVE_ACTIVE=1`) |
| `make test-active-f0` | Same as above against the STM32F0 backend mock |
| `make test-active-f3` | Same as above against the STM32F3 backend mock |
| `make test-active-g0` | Same as above against the STM32G0 backend mock |
| `make test-active-f4` | Same as above against the STM32F4 backend mock |
| `make test-active-g4` | Same as above against the STM32G4 backend mock |
| `make test-lowpower` | Same suite rebuilt with `-DOW_LOW_POWER=1` (WFE path, F1) |
| `make test-lowpower-f0` | Same as above against the STM32F0 backend mock |
| `make test-lowpower-f3` | Same as above against the STM32F3 backend mock |
| `make test-lowpower-g0` | Same as above against the STM32G0 backend mock |
| `make test-lowpower-f4` | Same as above against the STM32F4 backend mock |
| `make test-lowpower-g4` | Same as above against the STM32G4 backend mock |
| `make test-ndebug` | Same suite with `-DNDEBUG -DOW_TEST_PARAM_GUARD`: asserts compiled out, so the rejected-size returns (0) are observable; adds `test_param_guard`. Firmware reject paths (`ONEWIRE_PARAM_CHECK`) return 0 — the trap lives only under the test harness, keeping `fprintf` out of firmware images |
| `make test-ndebug-f0` | Same as above against the STM32F0 backend mock |
| `make test-ndebug-f3` | Same as above against the STM32F3 backend mock |
| `make test-ndebug-g0` | Same as above against the STM32G0 backend mock |
| `make test-ndebug-f4` | Same as above against the STM32F4 backend mock |
| `make test-ndebug-g4` | Same as above against the STM32G4 backend mock |
| `make test-ds2431` | Run the DS2431 EEPROM driver suite alone (26 tests against the F1 mock; also executed by `make test` and by every family wrapper) |
| `make test-hil` | Run the on-demand hardware-in-the-loop harness against an attached bench (see [Hardware-in-the-Loop (HIL)](#hardware-in-the-loop-hil)). Not in CI — it needs an ST-Link and real sensors, and skips cleanly when none is attached. `HIL_SCRIPT=smoke\|scan\|stats\|lowpower\|ds2431\|dma_pressure` picks the check; `HIL_MCU`, `HIL_PORT`, `HIL_EXT`, `HIL_TIMEOUT`, `HIL_SYSCLK_MHZ`, `HIL_TIM1_MHZ` and `EXPECTED_DEVICES` tune it |
| `make test-chips` | Check the part matrix: every `chips/<part>.mk` names repository files that exist, its scalars are well formed, and an unknown family or part is rejected. No toolchain and no CMSIS download needed (`tests/check_chips.sh`) |
| `make test-clocks` | Compile-check the per-family clock defaults, including the F4 parts whose own default differs from the family one (`test-clocks-f0/f1/f3/f4/g0/g4/h5`, where the F4 one covers `f401xc`, `f401xe` and `f446xx`) |
| `make fuzz-all` | Build and run all fuzz harnesses (requires host-side Clang; `FUZZ_TIME=N` for duration). Each harness seeds its working corpus from `tests/fuzz/corpus/` and a shared 1-Wire command dictionary (DS18B20 + DS2431 opcodes); CI caches the grown corpus |
| `make fuzz-crc8` | Fuzz `onewire_crc8` alone |
| `make fuzz-measure` | Fuzz the DS18B20 measurement state machine (single + scan + Match ROM + parasite) against a reference model |
| `make fuzz-txn` | Fuzz the DS18B20 command-transaction engine (ROM/scratchpad/thresholds/copy/recall/power-supply) against a reference model |
| `make fuzz-ds2431-txn` | Fuzz the DS2431 EEPROM command-transaction engine (Read Memory, Write/Read/Copy Scratchpad) against a reference model |
| `make fuzz-lowpower` | Fuzz the low-power long-stage predicate and drive the `WFE` sleep loop itself (`OW_LOW_POWER=1`; the harness models the TIM1 update event via `-DMOCK_WFE_HOOK`; requires host-side Clang) |
| `make download-licenses` | Download the CMSIS third-party license files into `CMSIS/` |
| `make gccversion` | Show the detected compiler and linker versions |
| `make help` | Show all targets |

Optional build flags (append via `EXT="..."` or `OW_DRIVE_ACTIVE=1`):

| Flag | Effect |
|------|--------|
| `OW_DRIVE_ACTIVE=1` | Enable the optional active-drive write path (`-DOW_DRIVE_ACTIVE=1`): during master-only write slots the bus pin is temporarily switched to push-pull (see [Bus Electrical Model](architecture.md#bus-electrical-model). The default remains open-drain. |
| `TIMING=SLOW` | Apply a compile-time timing preset (default `STANDARD`; also `FAST`/`SLOW`/`ROBUST`/`CUSTOM`). Expands into `-DONEWIRE_ONE_PULSE=... -DONEWIRE_ZERO_PULSE=... -DONEWIRE_GUARD_BAND=... -DONEWIRE_SHORT_PULSE_MAX=...` for that preset. Override any single value with `EXT="-DONEWIRE_GUARD_BAND=100"`. See [Configuration → Timing](api-reference.md#timing-1). |
| `EXT="-DOW_PARASITE_POWER=1"` | Parasite-powered bus: raises the default guard band from 5 µs to 100 µs and builds every DS18B20 example (1-7) with `ds18b20_set_parasite(1)` — the strong-pull-up window is engaged at runtime per conversion. See 6_statistics. |
| `EXT="-DOW_LOW_POWER=1"` | Enable the opt-in low-power path: the TIM1 update **interrupt source** (UIE) is enabled only to generate a pending event that wakes `WFE()` via `SEVONPEND`, so the **driver** sleeps during long 1-Wire stages (> 1 ms) while the hardware completes the transaction. The sleep sits inside `ds18b20_poll()`, so no application call site changes. No ISR is installed and `NVIC_EnableIRQ` is never called. Without this define builds are byte-identical to the original. |

### Flash

-   **ST-LINK:** `make program` (uses `st-flash` / `ST-LINK_CLI.exe`)
-   **J-LINK:** `make jprogram` (uses `JFlashExe` / `JFlash.Exe`)

### Testing

The driver ships with a host test suite that runs entirely on the PC, no
hardware required:

```bash
make test        # host tests against the STM32F1 backend mock
make test-f0     # same suite against the STM32F0 backend mock
make test-f3     # same suite against the STM32F3 backend mock
make test-g0     # same suite against the STM32G0 backend mock
make test-f4     # same suite against the STM32F4 backend mock
make test-g4     # same suite against the STM32G4 backend mock
make test-h5     # same suite against the STM32H5 backend mock
make test-ds2431 # DS2431 EEPROM suite (26 tests; also runs inside every make test)
```

Both `src/onewire.c` and `src/ds18b20.c` are compiled as a single translation
unit (`tests/mock/ds18b20_test_access.c`) against a behavioural model of the
TIM1/DMA hardware (`tests/mock/hw_model.c`) and a register mock of the target
CMSIS header (`tests/mock/stm32f0xx.h` / `stm32f1xx.h` / `stm32f3xx.h` /
`stm32f4xx.h` / `stm32g0xx.h` / `stm32g4xx.h` / `stm32h5xx.h`) — each suite runs
the full driver against its own
backend's channel/DMA wiring. (The driver itself is an amalgamated translation unit
    too: `src/ds18b20.c` `#include`s its four functional parts, so the whole
   driver shares the `ctx`/`txn_ctx`/`res_ctx`/`dev_roms` statics in one
   object file.) The DS2431 driver is split instead: core TU `src/ds2431.c`
   owns the context and the public API, engine TU `src/ds2431_txn.c` works
   on the caller-owned context via `src/ds2431_internal.h`
   (`tests/mock/ds2431_test_access.c` pulls both plus `src/onewire.c`
   into one test object). The base suite runs 309 tests against the F0/F1 mocks, 310
   against the F3 and F4 backends (the F4 target also builds and runs its
   180 MHz clock-default host variant `ds18b20_test_f4_180mhz.exe` as an
   extra, same count) and 311 against G0 (which adds two DMAMUX
   request-routing tests), 312 against G4 (same two DMAMUX tests plus the
   G4 console-baud test; the 16 MHz variant `ds18b20_test_g4_16mhz.exe`
   runs as an extra, same count) and 313 against H5 (which also runs the
   250 MHz and 8 MHz host variants `ds18b20_test_h5_250mhz.exe` and
   `ds18b20_test_h5_8mhz.exe` as extras, same count). Every wrapper runs
   the DS2431 suite alongside it (26 tests, see `make test-ds2431`).
    The suite covers:

-   State machine transitions (idle → start → measure → read → decode)
-   Non-blocking device search (Search ROM, ROM CRC validation, multi-device)
-   Non-blocking alarm search (Alarm Search ROM, 0xEC command feed, scan-table
    isolation)
-   Non-blocking resolution change (`ds18b20_set_resolution_*`): exact wait
    timings for 9/10/11/12 bit, Skip ROM and Match ROM config writes, CCR3-feed
    bus release, ownership guards, presence-abort and scratchpad auto-derivation
-   Non-blocking command transactions (`ds18b20_read_rom`,
    `ds18b20_set_alarm_thresholds`, `ds18b20_read_scratchpad`,
    `ds18b20_copy_scratchpad`, `ds18b20_recall_eeprom`,
    `ds18b20_detect_parasite`): command feed builds (Skip/Match ROM),
    resolution-preserving TH/TL writes, raw scratchpad read + CRC +
    resolution auto-derivation, 10 ms Copy/Recall hold-offs, power-supply
    decode, ownership guards, presence-abort and result reporting
-   CRC-8 (Dallas/Maxim) verification
-   1-Wire pulse encoding and presence detection
-   1-Wire layer coverage: reset/presence timing, write-then-read merge,
      multi-slot writes, multi-byte reads, search engine (device + alarm),
      ownership guards and the merged search capture buffers
-   DMA buffer/transfer contracts (`test_dma`), DMAMUX request routing on G0
    (`test_dmamux`)
-   DMA per-operation contract table (`test_dma_contract`): for every bus
    operation the exact source and destination addresses, the transfer counts,
    the transfer width and direction, the address-increment bit, the timer
    repetition count, the peripheral request each channel must carry, and the
    accounting after the run (every scheduled transfer fired, both channels
    drained). The same table runs on all nine families; the per-family
    encoding of the same guarantee is pinned separately
    (`test_dma_cr_value_macros`)
-   the temporal contract of every operation (`test_dma_contract`): the CCR3
    reload follows the slot-end compare and never precedes it, the pulse in
    effect holds for the whole slot, the slot boundaries fall where the
    repetition count says, and the operation ends on the terminal update with
    UIF set and the timer stopped
-   the DMA guarantee ledger (`test_dma_contract`): which DMA guarantees hold
    on the family under test and which are vacuous there and why. It is
    checked, not documented — a family that is not on the list fails the
    suite, and the request ids it claims are read back from the live
    registers after a real operation is armed
-   Scratchpad decode and temperature conversion (incl. negative values)
-   Timing configuration and register setup
-   Bus release behaviour between slots
-   Channel broadcast (`test_broadcast`), UART app driver (`test_app_uart`) and
    test-scaffold accessors/harness edge-case branches (`test_harness_api`)
-   `ow_stats` capture and error counters plus the non-blocking dump protocol
    (`test_ow_stats`)
-   DS2431 EEPROM command transactions (`test_ds2431` in the separate
    `make test-ds2431` suite): Read Memory over Skip/Match ROM, Write/Read
    Scratchpad with TA/ES echo, logged-PF rejection and written-data read-back
    verification (incl. a variable-length short reply), Copy Scratchpad with
    the t_PROG hold-off and the device's 0xAA completion pattern (a refused copy
    reports failure), presence-abort and out-of-range paths, plus a full
    end-to-end write cycle

Separate opt-in builds extend the suite: `make test-active` (active-drive, 8
tests + the 26-test DS2431 suite, every family), `make test-lowpower` (full
suite + low-power WFE path, 325 tests against F0/F1, 326 against F3/F4,
327 against G0, 328 against G4, plus the same 26 DS2431 tests) and
`make test-ndebug` (322 / 323 / 324 / 325 + the same 26, asserts off).

### Does the suite actually notice a broken DMA contract?

A green suite only means something if it *can* go red. `tests/mutation_check.py`
answers that for the DMA contract: it applies a deliberate defect to the port
source in a scratch git worktree (the working tree is never touched), builds
the host suite for a family, and records whether the suite failed.

```bash
make test-mutations                              # the whole matrix (slow)
python tests/mutation_check.py --list            # the defects it injects
python tests/mutation_check.py --families g0,g4   # a subset
```

A canary runs it on every pull request: the `contract-canary` job applies one
deliberate defect - a wrong timer repetition count - to two families, one per
DMA IP generation, and requires the suite to notice it. The defect goes into
a scratch git worktree, so the checkout is never touched. It is the cheap,
always-on part of the same idea; the full matrix is far too slow for a pull
request and stays on demand.

The defects are the ones a DMA contract is most likely to break by: a wrong
buffer, a wrong transfer count, a wrong transfer class (direction/width), a
wrong timer repetition count, a wrong capture width, a missing request
routing, and the per-family constants (the DMAMUX request id, the GPDMA
REQSEL, the F4 stream CHSEL), plus two against the model itself, to show
that the temporal claims are not vacuous, and two that start the timer before
the DMA is armed, to show that the arming order is checked. The last run
caught **74 of 74** applied defects, on every family it covers.

It earned its keep: the run found that the request routing of the merged
write+read (search) pass was asserted nowhere, that the contract table's
rows ran back to back on shared state - the routing register is sticky, so a
row that forgot to program its routing saw the previous row's value and
passed - and that the model used to answer an unresolvable DMA address by
quietly skipping the transfer. All three are fixed; the matrix has been
green since.

It also caught a bug in the model itself: the temporal slot count was
computed as `(uint8_t)((RCR & 0xFF) + 1)`, so the 256-slot case truncated
to zero and the walk ran away. Nothing had ever pointed the temporal model
at a long operation.

And it caught a hole nothing else could see: the port started the timer and
programmed the DMA in an order no test could check, because the model reads
the *final* register state and the final state is the same either way. On
hardware that order matters - an edge that arrives while the channel is
still disarmed is lost, and the first slot of the operation would silently
capture nothing. The port now starts the timer through a vocabulary step
(`OW_PORT_TIMER_START()`), so the model sees the exact moment the timer
begins to run and can check that the channels the operation arms are armed
*before* that moment.

Which guarantees hold on which family is stated in one table - the DMA
guarantee ledger in `test_dma_contract.c` - and it is checked, not
documented: a family that is not on the list fails the suite, and the
request ids it claims are read back from the live registers. The "n/a" cells
in the matrix above are the families whose request map is fixed in the
silicon: the port programs nothing there, so there is nothing to assert.

### Hardware-in-the-Loop (HIL)

An optional, on-demand harness (`tests/hil/`) drives the examples on a real
bench. It is **not** part of CI — a hosted runner has neither an ST-Link nor
sensors. It identifies the attached board from the ST-Link probe, builds the
matching example, flashes it with the STM32CubeProgrammer CLI, reads the UART
and checks the capture:

```bash
make test-hil                       # smoke: 2_device_search on the reference bench
make test-hil HIL_SCRIPT=scan       # 4_scan_mode
make test-hil HIL_SCRIPT=stats      # 6_statistics
make test-hil HIL_SCRIPT=lowpower   # 7_low_power
make test-hil HIL_SCRIPT=ds2431     # 8_ds2431 - the EEPROM cycle on every DS2431 found
make test-hil HIL_SCRIPT=smoke HIL_MCU=STM32L476xx HIL_PORT=COM5
                                    # NUCLEO-L476RG: external USB-TTL on USART1 PA9
make test-hil HIL_SCRIPT=dma_pressure HIL_MCU=STM32F030x6
                                    # 0_dma_pressure — guard-band matrix, logic analyzer needed
python tests/hil/dma_pressure.py --levels 0,4,5 --guards 1,5 --clocks 48,8
                                    # custom matrix (make cannot forward the args)

# regenerate the published report, the one-frame captures and the capture /
# negative-control data
python tests/hil/dma_pressure.py --json docs/stress/results.json \
                                 --sr-dir docs/stress/captures
python tests/hil/dma_capture.py --json docs/stress/capture.json
```

The board is auto-detected (`STM32F103xB` reference bench, the
`STM32H503xx`/`STM32H743xx` WeAct boards, or the `STM32L476xx` NUCLEO); pin it
with `HIL_MCU=` - a mismatch against the attached target
fails rather than flashing the wrong part. The H503 and F446 profiles also wire
PA11 to D1: the DMA-pressure harness builds with `STRESS_MARKER=1`, brackets
each frame with that marker and checks the run identity and injection bits
before it trusts a capture. The COM port is scanned unless
`HIL_PORT=` names one, `HIL_EXT=` overrides the bench's strong pull-up (unset
uses the bench profile, an explicit empty value means an externally powered
bus), and `HIL_TIMEOUT=` forces a fixed window
instead of the per-device budget. `HIL_SYSCLK_MHZ=` and `HIL_TIM1_MHZ=` pass
`SYSCLK_MHZ=` / `TIM1_MHZ=` to the build (for the H7 480/240 split) and add the
timer clock to the run's `# hil:` identity header. When no probe or target is
attached the
harness skips and exits 0, so it is safe to run anywhere. Raw captures land
under `bench_logs/hil/` (gitignored).

**A run names itself.** Every log the harness saves starts with `# hil:` lines
carrying the script, the example, the part, the clock the build used, the
build's EXT flags (`ext=none` when there are none, comma-separated otherwise),
the UART TX buffer size the harness asked for (`uart_tx=shipped` when the
example's own size is used), the library version, the commit, whether the tree
was dirty, and the SHA-256 of the image that was flashed. The version and the
clock are read from the same files the build reads (`inc/ds18b20.h`, the
Makefile's part mapping, `chips/<part>.mk`), so they cannot drift from it. A
log with `dirty=yes` does not describe a commit - the image hash is what pins
that run.

**The wire itself can be measured too.** `tests/hil/la_capture.py` captures
the 1-Wire line (PA10) with a logic analyzer and checks the measured pulse
widths and slot period against the configuration in `inc/ow_config.h` — the
schedule a consumer's bus has to live with, checked on the wire and not only
in a register. It needs `sigrok-cli` (set `SIGROK_CLI` to point at it) and
numpy:

```bash
python tests/hil/la_capture.py --parasite   # the parasite bus: guard 100 us
python tests/hil/la_capture.py --parasite --wait-ms 750   # and the conversion wait
```

`--wait-ms` also checks the **duration** of a long wait - the one part of the
schedule the host tests cannot prove. The engine and the host model both read
the repetition counter as "periods - 1", so a consistently wrong convention
passes every host test; on the wire it is a gap of the wrong length, one
period being 62.5 ms for a 12-bit conversion. The F1 bench measured 750.31 ms
against a 750 ms plan.

`tests/hil/la_decode.py` reads the **content** of the same capture - the bits,
the ROM search, the command bytes and the scratchpad - and checks them against
the CRC the device computed. With `--log` it compares the temperature decoded
from the wire with the one the application printed: two independent
computations of the same physical value. That is the one comparison the host
model cannot make, because there it is the same code on both sides.

Arm the capture first (its own shell), then flash - the boot search is what
puts the whole run on the wire:

```bash
python tests/hil/la_capture.py --parasite --seconds 30 \
    --out bench_logs/hil/la_search.bin
# in another shell, while it captures:
make test-hil HIL_SCRIPT=smoke

python tests/hil/la_decode.py bench_logs/hil/la_search.bin \
    --log bench_logs/hil/smoke_20261022-165226.log
```

`--parasite` matches the bench's build; without it the geometry check expects
the 70 us default slot and fails on a correct parasite capture (the artifact
runts also make a "nearest gap" pick the runt instead of the period).

Those captures can also be read as traffic instead of measurements:
[`tools/1wire_vis`](../tools/1wire_vis/) decodes a sigrok `.sr` file slot by slot
in the browser — resets and presence, Search ROM, every command byte and the
device CRC — with no upload and no server. The bundled 9_Multi recording (8 MHz,
DS2431 reads and `0F`/`AA`/`55`/`F0` write chains) is published at
<https://a5021.github.io/stm32-async-1wire/viewer/>.

It is strict on purpose: a slot width between '1' and '0', an unknown command
or a CRC mismatch is a failure, not a guess or a skipped transaction. A search
(`0xF0`) is rebuilt from the directions the master wrote, and its ROM set must
equal the log's. The readings are paired conversion by conversion from the
boot the log covers - the wire's 1/16 C value must round to the tenth the
application printed, which is an equality, not a tolerance. A capture armed
before the flash also holds the previous image's round; the pairing starts at
the last search, and the capture must cover the run the log came from.

One truncation is expected rather than a failure: arming the capture before
`make test-hil` means the programmer resets the previous image, often in the
middle of a search pass. The master stops writing direction bits, the line
goes idle and the new image starts with its own reset. Such a pass yields no
ROMs, so there is nothing to compare and nothing is guessed; the report marks
the exchange `обмен прерван` and keeps the transactions around it. Every other
cut stays an error - a half-written command, a frame shorter than its command
says, a read that stops mid-frame - and a capture with no completed
transaction at all is FAIL. Both sides are covered in
`tests/hil/test_la_decode.py`.

The DS2431 cycle is read the same way. The function table follows the family
byte of the selected ROM, because `0x55` and `0xF0` are ROM commands at the
start of a transaction and memory commands later, and they mean different
things on a `28` and a `2D`; a function sent after SKIP ROM is refused as
ambiguous instead of guessed. The Read Scratchpad reply is 13 bytes, and the
inverted CRC-16 the device appends is recomputed from `0xAA` plus the eleven
frame bytes. With `--log`, the pattern the example wrote, the frame it
verified and the EEPROM it read back must match the wire byte for byte - and
the read-back must carry the written bytes.

Agreeing is not enough on its own: a log that prints the pattern it *meant* to
write, or a device that ignored Copy Scratchpad, leaves the wire and the log
identical while the EEPROM still holds the old byte. So the decoder follows
each device's cycle - write, scratchpad frame, copy, read - and fails when the
frame returns something else, when the read-back differs from the write, and
when a copied row is never read at all.

The example writes the row's own next value (every byte plus one) rather than
a fixed pattern, so the baseline and the write differ on every run and every
device. When the row changed and the device returned the new bytes, the report
says the copy is proven. When the baseline already held the pattern - a
capture from an image that wrote a fixed one - it says so as a note: the read
path is proven, the copy is not. Without the baseline in view it claims
neither.

The 2026-10-23 F1 bench capture is the first one that carries the proof. Both
`2D` parts went `DE AD BE EF 01 02 03 04 -> DF AE BF F0 02 03 04 05` on the
wire, the Read Memory after Copy returned the new row with the device's CRC,
and the report closed with `копирование доказано` for both. The same artifact
still holds a cycle from the image that preceded the flash: its baseline
already equalled its write, so it appears only as the equal-baseline note.
Having both cases in one report is deliberate - it shows what the note and the
proof look like.

The same proof was repeated through the documented command, with no explicit
`HIL_EXT`: the header recorded the profile's `-DOW_PARASITE_POWER=1`, both rows
went `E2 B1 C2 F3 05 06 07 08 -> E3 B2 C3 F4 06 07 08 09`, and the report
again closed with `копирование доказано`.

The report opens with the capture and log hashes, the sample rate and channel,
the slot geometry and the `# hil:` identity, and ends with one `ИТОГ: PASS/FAIL`
line, so a stored artifact can be re-checked and the verdict read off without
reconstructing it from the prose above.

A capture also writes a `<file>.meta` sidecar next to it with the rate, channel,
duration, tool and the SHA-256 of the samples. The decoder takes the rate from
the sidecar and refuses a `--rate` or `--channel` that contradicts it - the
samples themselves do not say at what rate they were taken, and decoding at
another rate silently rescales every width. Artifacts older than the sidecar
still decode: the flags (or 4M/D0) are used, and the report says the metadata
is absent.

**What each layer can prove.** A green line in one of them is not evidence for
the next:

| Evidence | Check | Proves | Cannot prove |
|----------|-------|--------|--------------|
| Host model | `make test`, `make test-ds2431` against the F0/F1 mock | state machines, command order, CRC math, error paths | timing, electrical behaviour, what a real slave does |
| Signal geometry | `la_capture.py` | pulse widths, slot period, long waits - the schedule the bus sees | which bytes those pulses carry |
| Wire content | `la_decode.py` | bits, ROM/search, commands and payloads with the device's CRC; with `--log`, that Copy changed the row and the device's read-back returned the new bytes | retention after power-off, or margin over temperature and voltage (the part's datasheet, not this run) |
| Real device | `run.py` log + read-back | the driver against an actual part, including the EEPROM read | negative electrical scenarios the bench cannot inject |

The negative controls for the analyzer need no bench and live in
`tests/hil/test_la_decode.py`: no presence, a silent device (all ones, bad
CRC), a damaged ROM CRC, a presence inside a frame, a capture cut one slot
short, a copy with no read-back, and a wire and a log that agree on a write
that did not take.

The measured geometry and decoded frames of the F1 bench, and what they are
for, are in `port/stm32f1/HARDWARE-NOTES.md`.

### Integration: four ways to use the library

Pick the path that matches your build system:

| Path | When to choose |
|------|---------------|
| **CMake subproject / FetchContent** | Recommended. Zero-copy: the library is pulled as a subproject and brings its own CMSIS. |
| **CMake `find_package`** | You consume a pre-installed tree; you supply your own CMSIS headers. |
| **Vendored copy + `ow_add_library.cmake`** | You copy `inc/` + `src/` into your tree and drive the build with CMake. |
| **Plain Make / `make lib`** | You build with plain GCC/make and link a static archive into your firmware. |
| **PlatformIO** | You use PlatformIO's library discovery (`lib_deps`). |

#### 1. CMake subproject / FetchContent (recommended)

No files are copied: the library is added as a subproject and manages its own CMSIS dependencies through FetchContent.

```cmake
include(FetchContent)
FetchContent_Declare(stm32_async_1wire
    GIT_REPOSITORY https://github.com/a5021/stm32-async-1wire.git
    GIT_TAG        v3.0.1
)
FetchContent_MakeAvailable(stm32_async_1wire)
target_link_libraries(your_app PRIVATE stm32_async_1wire)
```

Pass the device macro (or family macro) on the command line:

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake \
      -DOW_MCU=STM32H503xx -B build .
cmake --build build
```

This is the path to prefer: nothing has to be downloaded or configured first, and CI builds it against every family on every change.

The cache variables are:

| Variable | Meaning |
|----------|---------|
| `OW_MCU` | Exact STM32 device macro (`STM32F103xB`, `STM32H503xx`, ...); selects the part file automatically |
| `OW_TARGET` / `OW_CHIP` | Legacy family (`f0`...`h5`) and part (`f401xc`, ...) selectors; still honoured, but `OW_MCU` is the documented interface |
| `OW_SYSCLK_MHZ` | System clock in MHz; empty = the part's own default from `chips/<part>.mk` (mirrors the Makefile's `SYSCLK_MHZ`) |
| `OW_EXTRA_DEFINES` | Extra `-D` definitions applied to every target, e.g. `"-DOW_PARASITE_POWER=1;-DOW_LOW_POWER=1"` |
| `OW_BUILD_EXAMPLES` | Build the example applications (`OFF` by default) |
| `OW_CMSIS_CORE_INCLUDE` / `OW_CMSIS_DEVICE_INCLUDE` | Your own CMSIS `core` and device include dirs (STM32Cube, PlatformIO, manual checkout). Pass both together to skip both FetchContent clones; one without the other is an error |

Already carrying CMSIS (STM32Cube/PlatformIO project)? Pass both include dirs and nothing is downloaded:

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake \
      -DOW_MCU=STM32H503xx \
      -DOW_CMSIS_CORE_INCLUDE=/path/to/CMSIS/Core/Include \
      -DOW_CMSIS_DEVICE_INCLUDE=/path/to/cmsis_device_h5/Include -B build .
```

#### 2. CMake `find_package`

Install the library once, then consume it from an install prefix:

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake \
      -DOW_MCU=STM32H503xx -DCMAKE_BUILD_TYPE=Release -B build .
cmake --build build
cmake --install build --prefix build/prefix
```

```cmake
find_package(stm32_async_1wire REQUIRED)
target_link_libraries(your_app PRIVATE stm32_async_1wire::stm32_async_1wire)
```

Two things to know:

- **The consumer provides CMSIS.** The include paths the library compiled against were FetchContent checkouts inside its build tree; they are not baked into the install export. Add your own CMSIS `core` and device include directories (from STM32Cube, PlatformIO, or a manual checkout) on top of the imported target.
- **The package carries its part selection.** The device macro (`STM32H503xx`) and clock (`OW_SYSCLK_MHZ`) are baked into the installed target as public definitions. Build the package for the part your firmware is for.

#### 3. Vendored copy + `ow_add_library.cmake`

Copy `inc/` and `src/` into your project tree, then use the drop-in CMake helper:

```text
your-project/
├── inc/     <- copy of stm32-async-1wire/inc/
└── src/     <- onewire.c, ds18b20.c, ds2431.c, ow_stats.c
```

```cmake
include(path/to/stm32-async-1wire/cmake/ow_add_library.cmake)
ow_add_library(stm32_async_1wire
    MCU STM32H503xx
    SYSCLK_MHZ 64                                     # optional; omit for the SystemCoreClock path
    PARASITE                                          # -DOW_PARASITE_POWER=1
    CMSIS_INCLUDE_DIRS ${CMSIS_CORE} ${CMSIS_DEVICE})
target_link_libraries(your_app PRIVATE stm32_async_1wire)
```

It creates a static library from the four driver sources, adds `inc/` and your CMSIS include directories, and attaches the device macro, the clock define, the feature defines (`PARASITE`, `LOW_POWER`, `ACTIVE_DRIVE`, `STATS`) and the family's `-mcpu`/`-mthumb` — all `PUBLIC`, so your own sources compile against the same target. It does **not** fetch CMSIS: pass your own include directories through `CMSIS_INCLUDE_DIRS`. Anything else (`MAX_DEVICES` for `-DDS18B20_MAX_DEVICES`, or any `OW_*` the helper has no knob for) goes through `EXTRA_DEFINITIONS`.

**The copy is not self-contained.** A consumer also needs:

- CMSIS core headers (`cmsis_gcc.h`, `core_cm*.h`, ...)
- ST device headers for your family (`stm32h5xx.h` + `stm32h503xx.h`)
- `system_<family>xx.c` and the GCC startup file for your part
- Your linker script

And **exactly one** target macro: the ST device macro (`STM32H503xx`) or its family macro (`STM32H5`). `OW_SYSCLK_MHZ` is optional — omit it and the port reads `SystemCoreClock` at startup (keep it current with `SystemCoreClockUpdate()`).

This helper is for the vendored layout only. On the FetchContent / `add_subdirectory` path you do not need it — the library's own `CMakeLists.txt` already provides the `stm32_async_1wire` target with CMSIS managed.

#### 4. Plain Make / `make lib`

If you build with plain GCC/make (no CMake), use `make lib` to compile just the four driver sources into a static archive:

```bash
make OW_MCU=STM32H503xx lib
```

This produces `build/libstm32_async_1wire_<stamp>.a` with the same flags as the firmware build (`-mcpu`, device macro, clock define, includes, optimization), where `<stamp>` is the same part/clock/optimization stamp the object files carry (e.g. `h5_h503cb_64mhz_hse8`). Link it into your own firmware:

```bash
arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -T my_app.ld \
    -Ipath/to/inc -Ipath/to/CMSIS/core -Ipath/to/CMSIS/device \
    -DSTM32H503xx my_app.c build/libstm32_async_1wire_h5_h503cb_64mhz_hse8.a \
    -lc -lm -lnosys -Wl,--gc-sections -o my_app.elf
```

The archive is built with the same part/clock/optimization stamping as the firmware, so `make OW_MCU=STM32H503xx SYSCLK_MHZ=250 lib` produces a different archive from the 64 MHz default — there is no silent object reuse across clock variants.

#### 5. PlatformIO

The repository ships with `library.json` so PlatformIO can discover the library automatically:

```ini
[env:bluepill]
platform  = ststm32
board     = bluepill_f103c8
framework = stm32cube
lib_deps  = a5021/stm32-async-1wire
```

Or point to a local checkout:

```ini
lib_deps = symlink:///path/to/stm32-async-1wire
```

A ready-made consumer lives in `tests/integration/platformio/` — copy that directory as a starting point. CI does not run PlatformIO itself; it checks `library.json` consistency and that the consumer compiles with the manifest's own include set via `make test-manifest`.

#### Common gotchas

- **H5 reset clock.** On STM32H5 the HSI runs at 64 MHz but reset divides it to 32 MHz (`RCC_CR.HSIDIV = /2`). Before relying on 64 MHz, clear `HSIDIV` and set `FLASH_ACR = PRFTEN | LATENCY_3WS`, then call `SystemCoreClockUpdate()`. Skip it and the console runs at half rate and every 1-Wire slot is 2× out of spec.
- **Parasite power.** Define `-DOW_PARASITE_POWER=1` **and** call `ds18b20_set_parasite(1)` at runtime; without both, parasite-powered sensors report a fixed error temperature.
- **LTO.** If you build with `-flto`, archive with `arm-none-eabi-gcc-ar` (a plain `ar` leaves the LTO symbols out of the index and the link fails with undefined references to the driver API). The Makefile and CMake builds do this automatically.
- **Clock agreement.** If you do pass `OW_SYSCLK_MHZ`, it must match the clock your firmware actually runs (TIM1's kernel clock must equal SYSCLK), or the bus timing is scaled by the mismatch.

### STM32CubeIDE

1.  Run `make download-deps` once to fetch CMSIS headers.
2.  In STM32CubeIDE: **File → New → STM32 Project from an Existing Makefile**.
3.  Point to the repository root directory.
4.  The IDE auto-generates the CDT project; select your MCU target
    (e.g. STM32F103C8).
5.  Build and flash as usual.

### Configuration Notes

-   **Target Name:** The firmware target name is `ds18b20_$(APP)` (e.g. `ds18b20_1_basic`).

-   **Build Directory:** Default is `build/`.

-   **Optimization Level:**

    -   **Release:** `-Os -flto -g0` (default).
    -   **Debug:** `-Og -g3 -gdwarf`.

-   **MCU Flags:** `STM32F030x6` (Cortex-M0) with `OW_MCU=STM32F030x6`;
    `STM32F103xB` (Cortex-M3) by default; `STM32F303xC` (Cortex-M4) with
    `OW_MCU=STM32F303xC`; `STM32F407xx` (Cortex-M4) with `OW_MCU=STM32F407xx`,
    `STM32F401xC` with `OW_MCU=STM32F401xC`, `STM32F401xE` with
    `OW_MCU=STM32F401xE` or `STM32F446xx` with `OW_MCU=STM32F446xx`;
    `STM32G031xx` (Cortex-M0+) with `OW_MCU=STM32G031xx`.

-   **Target Selection:** `make OW_MCU=STM32F030x6` builds for the STM32F0 backend
    (48MHz default clock, `port/stm32f0/STM32F030X6_FLASH.ld`),
    `make OW_MCU=STM32F303xC` for the STM32F3 backend (72MHz default clock,
    `port/stm32f3/STM32F303XC_FLASH.ld`),
    `make OW_MCU=STM32F407xx` for the STM32F4
    backend (168MHz default clock, `port/stm32f4/STM32F407VGT6_FLASH.ld`),
    `make OW_MCU=STM32F401xC` for the F401CC variant of the same backend
    (84MHz default clock, `port/stm32f4/STM32F401CC_FLASH.ld`) and
    `make OW_MCU=STM32F401xE` for the 512KB/128KB xE parts
    (`port/stm32f4/STM32F401RE_FLASH.ld`, same 84MHz default), and
    `make OW_MCU=STM32F446xx` for the STM32F446
    (180MHz default clock with the over-drive sequence,
    `port/stm32f4/STM32F446RE_FLASH.ld`),
    `make OW_MCU=STM32G031xx` for the STM32G0 backend (64MHz default clock,
    `port/stm32g0/STM32G031X6_FLASH.ld`). Each part's
    identity lives in its own `chips/<part>.mk`. The default
    target is STM32F103 (bus on PA10 for F0/F1/F3/F4, logical PA10 via PA12 remap
    for G0).

-   **8MHz RC Build:** On the STM32F103 target the firmware runs on HSE 8MHz
    + PLL ×9 = 72MHz by default. Pass `SYSCLK_MHZ=8` to use the internal RC
    oscillator (HSI) at 8MHz without an external crystal or PLL:

    ```bash
    make SYSCLK_MHZ=8
    make debug SYSCLK_MHZ=8
    ```

    On the STM32F030 target the default clock is already HSI+PLL (48MHz);
    there `SYSCLK_MHZ=8` selects the raw 8MHz HSI instead. On the STM32F303
    the default is the 8MHz HSE-bypass signal ×9 = 72MHz, and `SYSCLK_MHZ=8`
    selects the raw 8MHz HSI (64MHz is the HSI ceiling there). On F4 the
    default is HSE+PLL (168/84/180MHz per part), and `SYSCLK_MHZ=16` selects
    the raw 16MHz HSI.

    The knob maps to the portable `OW_SYSCLK_MHZ` define — a single
    value in MHz that every clock-dependent setting derives from: the
    timer prescaler, the input-capture filter and the USART baud rate
    adjust automatically. Useful for testing on bare minimum hardware
    (no HSE crystal).

### Compile-time Tunables (`inc/ow_config.h`)

All genuinely tunable build constants live in `inc/ow_config.h`.  Every
macro carries a `#ifndef` guard so that a `-D` on the command line (Makefile
EXT, PlatformIO `build_flags`) overrides the default without editing the
header.  (These are preprocessor macros, not CMake options: a `cmake
-DOW_STATS_ENABLE=1` variable would not reach the compiler.  The CMake
example build enables stats automatically for `6_statistics` — see below.)
Protocol-inherent values (`ONEWIRE_MAX_SLOTS`,
`DS18B20_RES_MIN/MAX/DEFAULT`) and the system clock (`OW_SYSCLK_MHZ`,
passed by the build or resolved from `SystemCoreClock` by the port core)
remain outside this file and are NOT listed here.

The four feature flags use **value style**: define to **1** to enable,
omit or set to 0 to disable.  Old presence-only style
(`-DOW_LOW_POWER` without `=1`) no longer compiles correctly.

| Macro | Default | Notes |
|-------|---------|-------|
| `ONEWIRE_ONE_PULSE` | 5 | '1'-bit duration in µs — HW-validated on every clock |
| `ONEWIRE_ZERO_PULSE` | 60 | '0'-bit duration in µs |
| `ONEWIRE_GUARD_BAND` | 5 (100 when `OW_PARASITE_POWER=1`) | Slot release margin in µs |
| `ONEWIRE_SHORT_PULSE_MAX` | 10 | Short-pulse detection window in µs |
| `OW_PARASITE_POWER` | 0 | 1 = set parasite timing defaults (guard band) |
| `OW_LOW_POWER` | 0 | 1 = enable opt-in WFE sleep path |
| `OW_DRIVE_ACTIVE` | 0 | 1 = enable push-pull write path |
| `OW_STATS_ENABLE` | 0 | 1 = compile in per-sensor pulse statistics |
| `OW_BUS_DRIVE` | 3 (`OW_BUS_DRIVE_MAX`) | Drive strength of the bus pin: 0 = `WEAK`, 1 = `MEDIUM`, 2 = `STRONG`, 3 = `MAX`. `MAX` is the default because the parasite strong pull-up sources the whole fleet from this pad; F1 has no `OSPEEDR` register, so `MAX` clamps to `STRONG` there. |
| `DS18B20_MAX_DEVICES` | 8 | Max devices in the device table (8 B each) — single-sensor builds can set 1 and save 56 B of `.bss` |

**Consistency token.** `OW_CONFIG_TOKEN` mixes the flags and clock frequencies
that must agree between the archive and the application (`OW_PARASITE_POWER`,
`OW_LOW_POWER`, `OW_DRIVE_ACTIVE`, `OW_STATS_ENABLE`, `OW_BUS_DRIVE`,
`DS18B20_MAX_DEVICES`, `OW_SYSCLK_MHZ`, `OW_TIM1_MHZ`).
Compare it at startup with `onewire_config_token()` and refuse to run on a
mismatch — see the
[1-Wire Layer reference](api-reference.md#1-wire-layer-shared).

**Override examples**

```bash
make                              # defaults, all flags =0
make EXT="-DOW_LOW_POWER=1"  # enable WFE sleep path
make EXT="-DOW_PARASITE_POWER=1"  # parasite guard-band default
make EXT="-DONEWIRE_SHORT_PULSE_MAX=15 -DDS18B20_MAX_DEVICES=16"
```

CMake: the `OW_STATS_ENABLE` flag is not a CMake option.  With
`-DOW_BUILD_EXAMPLES=ON` it is applied automatically to
the `6_statistics` example only (its dedicated library variant), mirroring
`make APP=6_statistics`:

```bash
cmake -DOW_MCU=STM32F030x6 -DOW_BUILD_EXAMPLES=ON -B build .
cmake --build build --target 6_statistics
```

Other CMake feature flags can be forwarded to the compiler via a standard
`CMAKE_C_FLAGS` (or a toolchain-file edit) — the `#ifndef` guard in
`ow_config.h` picks them up:

PlatformIO (`platformio.ini`):

```ini
build_flags = -DOW_LOW_POWER=1 -DOW_STATS_ENABLE=1
```

Feature flags can also be set via a Makefile knob (no EXT needed):

```bash
make OW_DRIVE_ACTIVE=1        # → -DOW_DRIVE_ACTIVE=1
```

## VSCode Integration

The repository includes `.vscode/` workspace configuration for a
convenient development workflow.

### Building

Press **Ctrl+Shift+B** to run the default build task (`make`). Other
tasks are available via **Ctrl+Shift+P** → "Tasks: Run Task":

- `Build (release)` — `make` (default)
- `Build (debug)` — `make debug`
- `Build F0 (debug)` — `make OW_MCU=STM32F030x6 debug` (debug build for the STM32F030 target)
- `Build F3 (debug)` — `make OW_MCU=STM32F303xC debug` (debug build for the STM32F3 target)
- `Build G0 (debug)` — `make OW_MCU=STM32G031xx debug` (debug build for the STM32G031 target)
- `Build F4 (debug)` — `make OW_MCU=STM32F407xx debug` (debug build for the STM32F4 target)
- `Clean` — `make clean`
- `Program (J-Link)` / `Program (ST-Link)` — flash the device
- `Download dependencies` — `make download-deps`

### Debugging

1. In the **Run and Debug** panel (`Ctrl+Shift+D`), select the debug
   configuration: **"Debug F1 (J-Link)"** / **"Debug F1 (ST-Link)"** for
   the STM32F103 target, **"Debug F0 (J-Link)"** / **"Debug F0 (ST-Link)"**
   for the STM32F030 target, **"Debug F3 (J-Link)"** / **"Debug F3
   (ST-Link)"** for the STM32F303 target, **"Debug G0 (J-Link)"** / **"Debug
   G0 (ST-Link)"** for the STM32G031 target, or **"Debug F4 (J-Link)"** /
   **"Debug F4 (ST-Link)"** for the STM32F4 target. The F0
   configurations build with `OW_MCU=STM32F030x6` automatically, the F3 ones with
   `OW_MCU=STM32F303xC`, the G0 ones with `OW_MCU=STM32G031xx`, the F4 ones with
   `OW_MCU=STM32F407xx`.
2. Open `examples/1_basic/main.c` and set a breakpoint in `main()`.
3. Press **F5** — Cortex-Debug will build the firmware in debug mode,
   flash it, run to `main()`, and halt.

The SVD file for the selected family is downloaded by `make download-deps`
and loaded automatically for peripheral register views in the debug
sidebar. Standalone SEGGER Ozone users can open `port/<mcu>/project.jdebug`
from any backend directory; the project resolves its SVD and ELF paths
relative to its own location. The F3 directory holds
`project-f303vc.jdebug` for the F303VC; the F4 directory holds one project per
part — `project.jdebug` for the F407VGT6 default, `project-f401cc.jdebug` for
the F401CC, `project-f401re.jdebug` for the xE parts and
`project-f446re.jdebug` for the F446, since each part has its own device name
and SVD.

**J-Link:** Connect a SEGGER J-Link debugger via SWD.  
**ST-Link:** Connect an ST-Link programmer (built into most Blue Pill
boards) via SWD.
