# Architecture and RTOS integration

[Back to the README](../README.md)

## Comparison with Common 1-Wire Techniques

The DS18B20 uses the 1-Wire bus protocol, which communicates over a single data
line with strict timing requirements. Several approaches exist to handle this
protocol on embedded systems:

| Technique | How it works | Blocking? | Timing precision | Typical use |
|---|---|---|---|---|
| **Bit-banging + delay** (e.g. OneWire Arduino) | GPIO toggling with `delayMicroseconds()`, interrupts disabled | Yes | Low (compiler/optimization dependent) | Hobbyist Arduino projects |
| **Bit-banging + timer ISR** | Timer interrupt drives GPIO transitions | Semi-blocking | Medium | RTOS-based firmware |
| **UART bit-banging** | UART at 9600/115200 baud emulates 1-Wire timings | Depends | Medium | Systems with spare UARTs |
| **Hardware 1-Wire master** | Dedicated IC (DS2482) or kernel subsystem (Linux w1-gpio) | No | High | Linux SBCs, complex systems |
| **Timer + DMA + One-Pulse Mode** (this driver) | DMA feeds CCR values to the timer, which generates bus timing without CPU-driven edges | No | Hardware-timed, 1 µs timer resolution; no CPU-induced edge jitter | STM32 firmware with available timer and DMA resources |

### Trade-offs

**Cost.** This driver consumes dedicated hardware resources — TIM1 and two DMA
channels (the capture drain from CCR4 and the marker feed into CCR3: DMA1
channels 3/4 on F0/F1/F3/G0/G4/L4 (via DMAMUX on G0/G4, via DMA1_CSELR on L4),
DMA2 streams 2/4 on F4, GPDMA1 channels 2/3 on H5, and DMA1 streams 2/3 through
DMAMUX1 on H7) — that
cannot be used for other purposes. The 1-Wire data line itself occupies one GPIO (PA10), but any approach
needs a GPIO pin for the bus, so that is not an extra cost. Bit-banging
approaches, by contrast, need only that one pin and no DMA, making them more
portable across MCUs with limited peripherals.

**Precision vs. portability.** Timer+DMA keeps the CPU out of the
timing-critical path. The timer schedules bus edges at a nominal 1 µs
resolution, so CPU activity does not introduce edge jitter. Actual timing
accuracy depends on the timer clock and the electrical characteristics of the
bus. Software delays degrade under interrupt load, and even timer-ISR approaches
incur jitter from preemption. The trade-off is complexity: this driver's
hardware configuration is ~150 lines of register-level code versus ~20 lines
for a typical bit-bang implementation.

## Architecture

### Hybrid Hardware Automation

The 1-Wire layer uses a hybrid of several hardware features:

1. Timer-Driven Sequences: TIM1 is configured in One-Pulse Mode (OPM). Each state machine step configures the timer for a specific operation (reset, write byte, read byte, wait) and starts it.
2. DMA for Data Transfer: DMA is used in two key ways:
   - Transmit: Feeds a pre-calculated sequence of Compare Register (CCR) values to TIM1->CCR3 to automatically generate the precise waveform for writing commands or bits. The feed request comes from the CH2 slot-end marker compare and rides DMA1 channel 3 on F0/F1/F3/G0/G4/L4 (DMAMUX request 21 on G0, 43 on G4, DMA1_CSELR selection 7 on L4), DMA2 stream 2 at `CHSEL=6` on F4, GPDMA1 channel 2 at `REQSEL=59` on H5, or DMA1 stream 2 through DMAMUX1 request 12 on H7.
   - Capture: Automatically stores values from the TIM1->CCR4 capture register into memory to record pulse timings during read operations or presence detection; the capture drain rides DMA1 channel 4 on F0/F1/F3/G0/G4/L4 (DMAMUX request 23 on G0, 45 on G4, DMA1_CSELR selection 7 on L4), DMA2 stream 4 on F4, GPDMA1 channel 3 at `REQSEL=61` on H5, or DMA1 stream 3 through DMAMUX1 request 14 on H7.
3. Update Event as Completion Signal: The core polling mechanism checks the Timer Update Flag (TIM1->SR UIF). This flag is set when the timer completes its one-pulse countdown, signaling that the autonomous hardware operation (e.g., sending a reset pulse, waiting 750ms) is finished.
4. True Zero-ISR Overhead: The ds18b20_poll() function checks this flag. When set, it clears the flag and advances the state machine to the next step. This makes the entire driver event-driven by hardware completion signals without using interrupts.

### Non-Blocking, Interrupt-Free Design Principles

1. No Software Delays: No `delay_us()` or similar functions.
2. No Interrupts: Does not configure or use the NVIC. Fully deterministic.
3. Hardware Completion Events: The state machine advances only when the hardware timer signals that its current automated task is complete.
4. Minimal CPU During Operations: The CPU is only actively involved to set up a hardware operation and to process the result once it completes.

> The driver ships with a built-in non-blocking device search
> (`ds18b20_search_*`) for multi-sensor buses. The Maxim Search ROM (0xF0)
> algorithm is implemented as a compact state machine in the shared 1-Wire
> layer; it performs exactly one hardware-timed operation per poll call,
> consistent with the non-blocking measurement path. See `examples/3_round_robin/main.c` for a
> complete Search ROM example.

### Shared 1-Wire Layer

All bus-level protocol lives in `src/onewire.c` (interface in `inc/onewire.h`),
a reusable 1-Wire master that the DS18B20 driver builds on:

- `onewire_init()`, `onewire_reset()`, `onewire_reset_captured()`,
   `onewire_present()`,
   `onewire_write_slots()`, `onewire_encode_byte()`,
   `onewire_write_bit()`, `onewire_read_pair()`, `onewire_write_then_read()`,
   `onewire_pair_bits()`, `onewire_read_data()`, `onewire_decode_pulses()`,
   `onewire_bit_from_pulse()`,
   `onewire_strong_pullup()`, `onewire_bus_done()` — the
   TIM1/DMA bus primitives.
- `onewire_search_start()`, `onewire_search_poll()`,
  `onewire_search_count()`, `onewire_search_active()` — the generic Maxim
  Search ROM engine, shared by `ds18b20_search_*` and
  `ds18b20_alarm_search_*`.
- `onewire_crc8()` — the Dallas/Maxim CRC-8 utility.

Each bus transaction is timed and executed by TIM1 and DMA, without CPU-driven
timing. The application calls `onewire_bus_done()` / `onewire_search_poll()` to
let the non-blocking state machine check hardware completion and advance to the
next step; no busy-wait is used. Helper functions such as CRC calculation and
pulse encoding/decoding process data in software and do not start bus
transactions. The layer owns its own capture
buffers, keeps the line released to idle HIGH after every transaction, and
is fully covered by the host test suite. See the
[API Reference](api-reference.md#1-wire-layer-shared) for the complete
`onewire_*` surface.

### Layer Contracts

Dependency direction is app → `ds18b20` → `onewire` → `ow_port` → backend:
each layer uses only the one directly below it. Two past leaks were closed
along this rule: the driver no longer calls `ow_port_*` directly (it goes
through `onewire_bus_done()` / `onewire_long_wait_pending()` /
`onewire_sleep_until_done()`), and the low-power "long stage" flag is gone —
"long" is derived from the scheduled timer
(`CEN && !UIF && ARR*(RCR+1) > OW_PORT_LONG_STAGE_US`, one rule for capture,
feed and timer waits), so multi-slot writes sleep instead of busy-polling.
`bus_done()` retires the schedule explicitly.

The following six look like leaks but are deliberate, reviewed decisions —
do not "fix" them without re-reading the reasoning:

- **Backend selection lives in `inc/ow_board.h`.** One chain, one `#include`
  branch, `#error` on no family — so the backend and its defaults cannot
  drift (`8dba9d2`). Header-side selection is the integration contract:
  consumers define their CMSIS device macro (e.g. `STM32F030x6`; CubeMX
  defines `STM32F*` itself) and `ow_board.h` picks the family. Moving
  selection into the build system would break the CubeIDE path. The token is
  public because the application layer (`app.c`) needs it too; what it selects
  — the register-level per-family facts — stays private in `src/ow_mcu.h`.
- **`ow_pulse_t` is family-sized.** F4 and H7 feed CCR3 in DMA direct mode
  (halfword, `uint16_t`); the other families use `uint8_t`. An
  always-`uint16_t` type would double the pulse buffers on 4 KB-RAM parts;
  casts would hide the F4/H7 halfword requirement the backends document. It is
  declared next to the family token, in `inc/ow_board.h`.
- **The public/private header split is acyclic.** `onewire.h` includes only
  `ow_config.h` and `ow_board.h`; the engine `src/ow_port.h` includes
  `onewire.h` and `src/ow_mcu.h`, and both of those include `ow_board.h`
  through their own guard. No header is included by a header that includes it
  back, so nothing depends on include order. The old `onewire.h` ↔ `ow_port.h`
  cycle left with the engine from the public tree.
- **`onewire_strong_pullup()` stays in the generic layer.** It is a thin
  wrapper like every other `onewire_*` primitive, and the strong pull-up is
  a bus-line mode owned by the layer that owns the hardware operations —
  not a sensor concept. Removing it would push the driver back to
  `ow_port_*`.
- **`OW_PORT_MARKER_TOGGLE` is an opt-in hook.** Default no-op, used once
  (merged-pass start marker for a logic-analyzer decoder, see
  `port/stm32f4/HARDWARE-NOTES.md`). Zero cost unless a backend defines it.
- **The port's register vocabulary is defined and withdrawn inside the private
  engine header, `src/ow_port.h`.** The engine is header-only and written in
  the single-letter register aliases (`T1`, `PA`, `RC`, `D13`, ...), the
  `OW_BITFIELDS()` mask machinery and the register shortcuts (`RCC_AHBENR`,
  `TIM_CR1`, ...). Every macro body and inline body must see the vocabulary
  while it is parsed, so `src/ow_port.h` defines it at the top and `#undef`s it
  in its withdraw block once the last body is in. The engine is now
  library-internal — only `src/onewire.c` includes it — so a
  `#include "onewire.h"` translation unit can never carry it; but
  `make test-header-hygiene` still pins both halves across the four public
  headers and the two engine headers, every family. The `8dba9d2` refactor
  moved a backend include into `onewire.h` and the leak shipped in v2.1.0.
  `src/ow_mcu.h` defines none of it on its own: its family hooks only expand
  inside `src/ow_port.h`.

Two more closures, for the record: stats emission stays in the driver
because pulse widths, ROM addresses and `DS18B20_TEMP_ERROR_*` codes are
driver-owned concepts the onewire layer must not know (see the note at the
`ow_stats.h` include in `src/ds18b20.c`); the reset-geometry macros stay in
`src/ow_port.h` because they are the port's own schedule parameters
published as port contract (see the comment there).

### Required Timer Capabilities

The contract describes the functional peripheral topology required by the
driver.  A new backend is valid only if the target MCU can realise the
complete topology simultaneously on **one timer instance** and its DMA
routing; having the individual features somewhere in the MCU is not
sufficient.  The topology is:

```
                ONE TIMER
                    │
        ┌───────────┼───────────┐
        ▼           ▼           ▼
      CH2         CH3         CH4
        │           │           │
      DMA         GPIO        IC
                    │           │
                    └── TI3 ◄───┘
```

The driver does not care which timer is used — it cares about what the timer
can do.  The following capabilities are hard requirements; a timer that lacks
any one of them cannot run this driver, which is why the port layer
(the `src/ow_mcu.h` family sections) pins each family to one specific timer
rather than letting the user choose.

#### 1. Advanced-control timer (not general-purpose)

| Capability | Why it is required |
|---|---|
| **RCR (Repetition Counter)** | Batches *N* PWM periods into a single Update Event.  Without RCR, every bit slot generates its own update — a 16-bit command would need 16 interrupt handlers or 16 poll rounds instead of one.  RCR=15 lets the timer autonomously generate 16 slots, then assert UIF once so the state machine advances in a single step.  *Only advanced-control timers (TIM1/TIM8 on STM32) have RCR.* |
| **BDTR + MOE (Main Output Enable)** | Gates the entire output stage.  The driver sets MOE once at init and never touches it again; when the timer stops (OPM) the output goes high-impedance and the external pull-up takes over.  General-purpose timers lack BDTR — their output is always driven. |
| **One-Pulse Mode (OPM)** | The timer auto-stops after the scheduled operation completes (one reset pulse, one byte write, one long conversion wait).  Each bus transaction is a self-contained hardware run; OPM guarantees the timer does not free-run and re-enter a spurious slot. |

#### 2. Channel topology

The driver requires three channels on the same timer, with a specific
capture-routing relationship:

| Channel | Role | Requirement |
|---|---|---|
| **CH3** | PWM output (active-low) | PWM Mode 2, pin drives the 1-Wire bus.  Each bit slot is one PWM period: short low (~5 µs) = '1', long low (~60 µs) = '0'.  Reset is an extended low within a ~960 µs slot. |
| **CH4** | Input capture (indirect) | **Must be routable to TI3** (the CH3 pin) via `CC4S=01`.  This is the constraint that eliminates many timer/pin combinations: only one input capture channel on each STM32 timer can watch a given output channel's pin, and on every supported family that channel is IC4→TI3.  CH4 captures presence pulses and read-slot timings after CH3 releases the bus to idle-HIGH. |
| **CH2** | Compare (end-of-slot marker) | A plain compare at `ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE` µs whose DMA request feeds the next CCR3 value from a precomputed pulse buffer.  CH2's pin is unused — only its compare event and DMA request matter. |

The indirect-capture constraint (IC4 must see TI3) is why the driver cannot
be moved to an arbitrary pin: the chosen GPIO must be the CH3/CH4 pair's
output/capture pin on the selected timer.  On STM32F1 this is PA10
(default AFIO map); on STM32F0, PA10 (AF2); on STM32F3, PA10 (AF6); on
STM32G0, the PA12 pad remapped to logical PA10 via `SYSCFG_CFGR1.PA12_RMP`;
on STM32G4, PA10 (AF6); on STM32F4, PA10 (AF1).

#### 3. DMA

Two DMA channels are required, each carrying a specific peripheral request:

| Channel | Direction | Request | Purpose |
|---|---|---|---|
| DMA channel A | Memory → Peripheral | TIM1_CC2 (CH2 compare) | Feeds CCR3 with the next pulse width on each slot boundary (the "feed" path). |
| DMA channel B | Peripheral → Memory | TIM1_CH4 (capture) | Drains CCR4 capture values into a memory buffer (the "capture" path). |

F0/F1/F3 have a fixed request map (no `DMA_CSELR` mux) - channel 3 = CC2,
channel 4 = CC4, confirmed empirically.  G0 uses a DMAMUX: TIM1_CC2 = request
21, TIM1_CH4 = request 23; G4 the same way: TIM1_CC2 = request 43, TIM1_CH4 =
request 45.  L4 keeps the classic DMA1 channels but selects the request in
`DMA1_CSELR`: selection 7 is TIM1_CH2 on channel 3 and TIM1_CH4 on channel 4
(the selector is per-channel).  F4 uses DMA2 with the per-stream `CHSEL` field
set to 6 (`TIM1_CH2`/`TIM1_CH4`): stream 2 = feed, stream 4 = capture.  A new
backend must verify the DMA request numbers for its target; the channel roles
are identical across all families.

#### 4. Clocking invariant

  The invariant is not "APB prescaler = /1" but **"the TIM1 kernel clock equals
  SYSCLK"**, which is what makes `PSC = SYSCLK_MHZ - 1` a 1 µs tick.  STM32
  timers double their clock when the APB prescaler feeding them is >1
  (`timer clock = 2 × PCLK`), so each family reaches SYSCLK its own way.  All
  supported families satisfy the invariant by construction: F1 keeps
  PPRE2=/1 (TIM1 is on APB2, so TIM1 = PCLK2 = SYSCLK); F0 and G0 never touch
  the APB prescaler, so it stays at its reset /1; F3 puts APB2 at /1 while
  taking APB1 down to /2 for the 36 MHz limit; F4 runs PPRE2=/2 at 168, 84
  and 180 MHz and *relies* on the doubling (84→168, 42→84, 90→180), with
  PPRE2=/1 only on the raw-HSI and raw-HSE paths; G4 and L4 leave every
  prescaler at /1 at both of their clocks.  See `examples/app/app.c`.
  
  #### 5. GPIO
  
  The bus pin must support alternate-function open-drain (for normal bus
  operation) and runtime switching to alternate-function push-pull (for
  parasite-power strong pull-up and the optional active-drive write path).
  The pin never leaves alternate function — only the output-stage topology
  changes.
  
  #### Supported families (same scheme, different prescaler and pin config)
  
  | Family | Timer | Bus pin | DMA routing | Notes |
  |---|---|---|---|---|
  | STM32F1 | TIM1 | PA10 (default AFIO) | Fixed: CC2→DMA1 ch3 (feeds CCR3), CH4→DMA1 ch4 | APB2=/1 by default |
  | STM32F0 | TIM1 | PA10 (AF2) | Fixed: same mapping | TSSOP20: PA8 not bonded out, CH3/CH4 is the only viable pair |
  | STM32F3 | TIM1 | PA10 (AF6) | Fixed, same pair as F0/F1: CC2→DMA1 **ch3**, CH4→DMA1 **ch4** | RM0316 Table 78; channel 4 also carries USART1_TX and channel 3 USART3_TX, harmless because no UART byte moves by DMA |
  | STM32G0 | TIM1 | PA10 via PA12 remap | DMAMUX: CC2=#21, CH4=#23 | SYSCFG `PA12_RMP`; PA11/PA12 cannot be used as GPIO while driver is active |
  | STM32G4 | TIM1 | PA10 (AF6) | DMAMUX: CC2=#43, CH4=#45 | APB2=/1, no x2 doubling; 8-bit feed tables |
  | STM32L4 | TIM1 | PA10 (AF1) | DMA1_CSELR: ch3 sel 7 = TIM1_CH2 (feeds CCR3), ch4 sel 7 = TIM1_CH4 | HSI16-only (no HSE on NUCLEO-L476RG); 80MHz HSI16+PLL or 16MHz raw HSI16; 8-bit feed tables |
  | STM32F4 | TIM1 | PA10 (AF1) | DMA2, CHSEL=6: CC2→stream2 (feeds CCR3), CH4→stream4 | Feed runs 16-bit in direct mode; see `port/stm32f4/HARDWARE-NOTES.md` |
  | STM32H5 | TIM1 | PA10 (AF1) | GPDMA1, REQSEL: CH2=#59 (feeds CCR3), CH4=#61 | No DMAMUX; 8-bit feed tables; CBR1.BNDT is a byte count (x2 per halfword capture); bench-validated (64MHz HSI, 8MHz HSE, 250MHz HSE+PLL1) |
  | STM32H7 | TIM1 | PA10 (AF1) | DMAMUX1: CC2=#12 (DMA1 stream 2, feeds CCR3), CH4=#14 (DMA1 stream 3) | 16-bit feed tables in direct mode; 240MHz default with opt-in 480/240MHz CPU/TIM1 split; D-cache off |
  
  The DMA column is the one thing here that is **not** interchangeable between
  families, and getting it wrong is silent: on a family whose CC2/CH4 requests sit
  on different channels, the feed DMA never fires and captures read back empty
  rather than misreporting. Each backend's channel pair is read out of its own
  reference manual and recorded in the assignment's comment.
  
All nine families share one TIM1/DMA core in `src/ow_port.h` (F0/F1/F3/G0/G4/L4
with DMA1 defaults, F4/H7 as thin shims overriding the DMA spelling - streams
with per-stream CHSEL on F4, DMAMUX-routed streams without it on H7 - H5 with GPDMA
CTR1/CTR2/REQSEL overrides): DMA2's per-stream `CHSEL` mux
travels inside the control-register word (GPDMA's `REQSEL` in CTR2 likewise),
and the merged write+read pass is
shared too, with no per-family arm order left.
  
#### Bus Electrical Model

- **Open-drain bus.** PA10 is configured as an alternate-function **open-drain**
  pin and idles HIGH; a single external pull-up resistor on the bus is required.
  Both the master and every slave are open-drain, so the bus is a **wired-AND**:
  if any device drives the line LOW the bus is LOW, otherwise it is pulled HIGH.
- **Normal slot signaling (master never drives HIGH).** A `write-0` or bus reset
  is the master actively pulling the line LOW; a `write-1` or read slot is the
  master **releasing** the pin to Hi-Z and letting the external pull-up return
  the line HIGH. The master therefore never actively drives the line HIGH during
  a normal slot — `write-1` is a *release*, not a push-pull HIGH.
- **Parasite strong-pull-up (the only unconditional push-pull usage in the default build).** `onewire_strong_pullup()`
  (`ow_port_strong_pullup()`) is the *only* place the pin is switched to
  push-pull in the default build; it actively sources current by driving the line
  HIGH during the temperature-conversion / EEPROM-programming window — a phase
  where the DS18B20 is silent and cannot respond on the bus — and also around the
  Convert T and Read Scratchpad command writes. Deliberately, it **stays engaged
  while the driver is parked at IDLE** so the sensor's capacitors keep charging
  for the next cycle; it is released only where the bus has to be free (at the
  reset pulse, at the scratchpad read and at the second reset).

- **Active-drive write mode (optional).** During master-only write slots the bus
  pin may be placed in alternate-function push-pull, actively driving both HIGH
  and LOW levels instead of relying on the pull-up for HIGH. The driver
  automatically retains open-drain operation for reset, presence, read slots and
  write/read transactions where the slave may drive the bus. This is what makes
  push-pull acceptable on a 1-Wire bus at all: it is confined to phases where no
  slave can answer. Enabled with `-DOW_DRIVE_ACTIVE=1`; the published default
  remains open-drain.

### State Machine Flow (hardware-timed; polled on UIF)

Explicit-start behavior
- The driver measures **only when asked**: after `ds18b20_init()` the bus stays idle, and one `ds18b20_start_measure()` call requests exactly one conversion + scratchpad-read cycle. Afterwards the driver parks at IDLE again — it never restarts a cycle on its own, so the measurement cadence, the retry policy and any idle interval are the application's decision. Nothing else starts a conversion either: finishing a device search, a resolution change or a command transaction leaves the bus idle.

- IDLE (state 0)
  - The driver is parked here. A pending UIF (raised by `ds18b20_start_measure()`) falls through into START, which turns the busy indicator **on** and arms the bus reset. The data union is not touched here: `capture[]`, `pulse[]` and `scratchpad[]` are three views of one union, and decoding must consume `pulse[]` before writing `scratchpad[]`.
  - Set state=1.

- START (state 1)
  - LED on. Run reset_bus():
    - CH3 issues active-low reset pulse (~480µs within ~960µs slot).
    - CH4 (indirect input) captures presence timing into ctx.capture[0..1] via DMA from CCR4.
  - Set state=2.

- CONVERT (state 2)
  - On UIF, onewire_present() with ctx.capture[] and onewire_reset_captured().
    - If present: send convert command via CH3+DMA. With no selected device,
      this is "Skip ROM 0xCC + Convert T 0x44" (16 slots, RCR=15). With a
      device selected via ds18b20_select(), it is "Match ROM 0x55 + 8-byte ROM
      + Convert T 0x44" (80 slots, RCR=79). Set state=3.
    - Else: report NO_SENSOR; set state=0 (parked until the next ds18b20_start_measure()).

- WAIT (state 3)
  - On UIF: wait_conversion() schedules exactly the conversion time of the
    configured resolution `ctx.resolution` (9-bit: 93.75ms; 10-bit: 187.5ms;
    11-bit: 375ms; 12-bit: 750ms). The driver states that duration; the
    engine picks the timer periods that produce it. The
    resolution is auto-derived from each valid scratchpad read and updated
    by `ds18b20_set_resolution()`; set state=4.

- CONTINUE (state 4)
  - On UIF: run reset_bus() again; set state=5.

- REQUEST (state 5)
  - On UIF, onewire_present() with ctx.capture[] and onewire_reset_captured().
    - If present: send read command via CH3+DMA. With no selected device,
      this is "Skip ROM 0xCC + Read Scratchpad 0xBE" (16 slots). With a device
      selected, it is "Match ROM 0x55 + 8-byte ROM + Read Scratchpad 0xBE"
      (80 slots). Set state=6.
    - Else: report NO_SENSOR; set state=0 (parked until the next ds18b20_start_measure()).

- READ (state 6)
  - On UIF: read_data() schedules 72 slots (RCR=71; ARR=70µs). CH3 emits ~5µs active-low kick at each slot start and then releases; CH4 captures sensor pulse timing; DMA fills ctx.pulse[72]. Set state=7.

- DECODE (state 7)
  - On UIF: decode_scratchpad() from ctx.pulse[] into ctx.scratchpad[], LED off; reject an all-0xFF frame from an absent addressed device (`NO_SENSOR`), verify the reserved scratchpad bytes (byte 5 = 0xFF, byte 7 = 0x10), then verify CRC; report temperature or CRC_FAIL; set state=0 (parked until the next ds18b20_start_measure()).

## RTOS Integration

### Bus Idle Behaviour

Relevant if `ds18b20_search_poll()` / `ds18b20_poll()` are called
from an RTOS task and scheduling delays can land between 1-Wire slots: what the
DS18B20 datasheet allows, what the bench measured, and what the suite covers.

- **Idle-HIGH is harmless.** The datasheet states the 1-Wire bus must be left
  in the inactive (high) state when suspending a transaction and that
  *"infinite recovery time can occur between bits so long as the bus is in the
  inactive (high) state during the recovery period."* The DS18B20 re-synchronises
  to the next falling edge; it has no internal timeout that expires during an
  idle-high pause.
- **Measured on a real bus:** injecting idle-high gaps of 10 µs … 5 ms between
  Search ROM slots finds all 5 devices in 100/100 runs at every gap size (bench
  experiment, quoted from the earlier hardware campaign).
- **Idle-LOW > 480 µs resets all devices** (datasheet: *"if the bus is left low
  for more than 480 µs, all components on the bus will be reset"*). This is the
  only real hazard.

Practical consequences for RTOS use:

1. A delayed poll is safe **because the line is released to HIGH in hardware**,
   not by software. Every bus operation now ends with the line idle-HIGH
   automatically: the CCR3-fed writes (`ow_port_feed`, including the merged
   search op) append a trailing 0 to the DMA feed, and the direct-write/capture
   operations (reset, read, single-slot write) use an OC3PE preload of 0 — both
   applied at the instant the one-pulse timer stops. Those `T1.CCR3 = 0`
   preloads are the only software CCR3 writes on the direct paths, latched at
   the stopping update event; the bus cannot be left LOW by a stale compare
   value, no matter how long the RTOS delays the next poll.
2. The usable scheduling latency budget is ~480 µs of LOW, not a tight
   microsecond window. Any RTOS that resumes the poll within hundreds of µs is
   fine; longer delays only require that the bus idles HIGH, which the hardware
   release guarantees by construction.

### Multithreading and Concurrency

The driver is safe to call from an RTOS task, but it is **not re-entrant and
not thread-safe by itself**. All driver state is global and shared: the DS18B20
state machine, the 1-Wire search context (`search_ctx`, in `onewire.c`), and
the TIM1/DMA peripherals (DMA1 on F0/F1/F3/G0/G4/L4, DMA2 on F4, GPDMA1 on H5,
DMA1 through DMAMUX1 on H7). There is no internal lock. The ownership guards
(`txn_can_start`, the `ds18b20_search_*` checks) only prevent *logical*
conflicts within a single-threaded model — they are plain flag checks, not
atomic across tasks.

Rules for correct RTOS use:

1. **Confine every `ds18b20_*` call to a single task, or serialise with a
   lock.** Either drive the whole driver from one task (the same one that calls
   `ds18b20_poll()`), or wrap every entry point — `ds18b20_start_measure()`,
   each `*_start`, each `*_poll`, `ds18b20_select()`, `ds18b20_scan_start()`,
   `ds18b20_search_start()` — in a mutex/semaphore taken for the entire
   sequence (start + poll loop). Two tasks touching the driver at once corrupt
   the shared state machine and the TIM1/DMA registers. Example (FreeRTOS):

   ```C
   xSemaphoreTake(ow_mutex, portMAX_DELAY);
   ds18b20_set_alarm_thresholds(0x19, 0x0F);
   while (!ds18b20_set_alarm_thresholds_poll()) {
       osDelay(1);   /* the transaction owns TIM1/DMA; just yield */
   }
   xSemaphoreGive(ow_mutex);
   ```

   While a search, a resolution change or a command transaction is running,
   `ds18b20_poll()` returns immediately without doing anything, so the operation
   must be advanced by its own `*_poll()` (as in the loop above) — not by
   `ds18b20_poll()`.

2. **TIM1, the driver's DMA feed/capture pair and the bus GPIO pin are owned
   exclusively by the driver from initialisation until reset.** No other task,
   ISR or peripheral may configure or use them:

   - **TIM1** — the prescaler, pulse-generation/CCR3-feed logic and input
     capture on CCR4 are all driver-managed.
    - **The DMA pair** — DMA1 channels 3 (feeds CCR3) and 4 (drains CCR4) on
      F0/F1/F3, the same two channels via DMAMUX requests 21 (TIM1_CC2) and 23
      (TIM1_CH4) on G0, requests 43 and 45 on G4, DMA1_CSELR selection 7 on both
      channels on L4, **DMA2 streams 2 and 4 at `CHSEL=6`** on F4, GPDMA1
      channels 2 and 3 at `REQSEL=59/61` on H5, and DMA1 streams 2 and 3 through
      DMAMUX1 requests 12 (TIM1_CC2) and 14 (TIM1_CH4) on H7.
    - **The 1-Wire data pin** — PA10 as alternate-function open-drain, or
      the physical PA12 pad remapped to logical PA10 on G0 (those pads must
      not be used as plain GPIO while the driver is active).

   The library has no deinit or release API — the exclusivity begins at
   `onewire_init()` / `ds18b20_init()` and lasts until reset. Configure any of
   these resources before initialising the driver, never after.

3. **Poll cadence vs latency.** Because the 1-Wire bit timing is generated
   entirely by hardware, `ds18b20_poll()` may be called at any rate — slow
   polling only increases latency, never causes errors (see *Bus Idle
   Behaviour*). A build with `OW_LOW_POWER=1` is the one case where the call
   does not return straight away: the driver then idles the core in `WFE()`
   while a long stage runs, so the CPU does not spin (rule 5, and
   [Examples](examples.md#examples — `7_low_power`). Two practical patterns:
   - **Dedicated polling task:** loop `ds18b20_poll(); osDelay(1);` (or
     `vTaskDelay(1)`). This gives low latency without saturating the CPU; the
     750 ms conversion wait is a hardware timer, so the task yields during it.
     If other tasks also use the driver, take the mutex around each
     `ds18b20_poll()` call (or skip the poll when the mutex is busy).
   - **Periodic timer / idle hook:** call `ds18b20_poll()` from a
     high-frequency timer callback or the RTOS idle hook.
   Avoid a tight `while (!done) poll();` busy loop — it consumes the task's
   entire timeslice.

4. **Callbacks run in task context, not in an ISR.** `ds18b20_complete()` and
   `ds18b20_busy()` are invoked synchronously from inside `ds18b20_poll()`,
   which runs in your task. You may therefore use ordinary RTOS primitives
   there — e.g. `xSemaphoreGive()` / `xTaskNotifyGive()` from
   `ds18b20_complete()` to wake a consumer task. No `...FromISR` variant is
   needed.

5. **No blocking result API.** The driver never waits for a measurement:
   there is no `ds18b20_read_temperature_blocking()`, and `ds18b20_poll()`
   never waits for a result — it advances the state machine and returns. If a
   task must sleep until a result is ready, start the operation
   (`ds18b20_scan_start()`, `ds18b20_search_start()` or a command
   transaction), then either poll in a loop that yields (`osDelay(1)`), or
   block on a semaphore that `ds18b20_complete()` releases. Do **not** add a
   TIM1 interrupt just to signal completion — polling is sufficient and
   preserves the zero-interrupt design.

   The one thing that can delay a `poll()` call is the opt-in low-power
   build: with `OW_LOW_POWER=1` the driver *idles the core* in `WFE()` while
   a long hardware stage runs, so the CPU does not spin. That is a sleep, not
   a wait for a result — the application's code is unchanged, and the result
   is ready at the same moment either way (see
   [Examples](examples.md#examples), `7_low_power`).

6. **Preemption mid-byte is safe.** A task may be preempted while a byte is
   being transmitted: the DMA completes the whole byte in hardware and the bus
   returns idle-HIGH, so resuming later is harmless (it complements *Bus Idle
   Behaviour*). Only the *next* operation must be scheduled by a `poll()` call,
   which any task may do once it holds the lock from rule 1.
