# API, performance and configuration

[Back to the README](../README.md)

## Contents

- [API Reference](#api-reference)
  - [Core Functions](#core-functions)
  - [1-Wire Layer (shared)](#1-wire-layer-shared)
  - [CRC Utility](#crc-utility)
  - [Device Search](#device-search)
  - [Alarm Search](#alarm-search)
  - [Per-Device Addressing](#per-device-addressing)
  - [Command Transactions](#command-transactions)
  - [Simultaneous Multi-Device Conversion](#simultaneous-multi-device-conversion)
  - [Parasite Power](#parasite-power)
  - [Signal Statistics Module (`ow_stats`)](#signal-statistics-module-ow_stats)
  - [Resolution Change](#resolution-change)
  - [Weak Callbacks](#weak-callbacks)
  - [Error Codes](#error-codes)
  - [DS2431 EEPROM Driver](#ds2431-eeprom-driver)
  - [DS2413 Dual-Channel Switch Driver](#ds2413-dual-channel-switch-driver)
  - [Multi-Device Application (`9_multi`)](#multi-device-application-9_multi)
- [Performance](#performance)
  - [Footprint](#footprint)
- [Configuration](#configuration)
  - [Part selection: one `OW_MCU` key](#part-selection-one-ow_mcu-key)
  - [Timing](#timing-1)

## API Reference

### Core Functions

```c
void ds18b20_init(void);
```
Initialize the DS18B20 driver. Enables the peripherals the backend needs (the bus
GPIO port, TIM1 and the DMA controller — DMA1 on F0/F1/F3/G0/G4/L4, DMA2 on F4,
GPDMA1 on H5, DMA1 through DMAMUX1 on H7) and sets up the timer prescaler for a
1 µs resolution. System clock configuration is handled separately in the
application (see `app.c`). This function does NOT start a measurement.

```c
void ds18b20_start_measure(void);
```
Request exactly one measurement cycle (broadcast `Convert T` + scratchpad read)
on the currently selected device, or on every discovered device in scan mode.
The result is reported through `ds18b20_complete()`. When the cycle finishes the
driver parks at IDLE and waits for the next request — it never starts a cycle on
its own, so the measurement cadence, retries and idle intervals are entirely the
application's decision. Ignored (no-op) while a measurement cycle, a device
search, a resolution change or a command transaction owns the bus.

```c
void ds18b20_poll(void);
```
The Core Driver Function: Must be called from the main loop. It checks the Timer
Update Flag (UIF). If the flag is set, it means the hardware has finished the
previous operation (e.g., sending a command, waiting for conversion). The
function then clears the flag and advances the internal state machine to the next
step. The driver's state is persistent, so this function can be called at any
rate without risk of getting stuck. At IDLE it does nothing until
`ds18b20_start_measure()` requests a cycle.

### 1-Wire Layer (shared)

The 1-Wire bus primitives and the Search ROM engine are **not** part of the
driver — they live in the shared 1-Wire layer (`inc/onewire.h` +
`src/onewire.c`) that `src/ds18b20.c` is built on. Full interface:

```c
void        onewire_init(void);
uint8_t     onewire_bus_done(void);
void        onewire_reset(volatile uint16_t *reset_pulses);
uint8_t     onewire_reset_captured(void);
uint8_t     onewire_present(const volatile uint16_t *pulses,
                            uint8_t captured);
uint8_t     onewire_write_slots(const ow_pulse_t *pulses, uint16_t slots);
uint8_t     onewire_write_bit(uint8_t bit);
void        onewire_read_pair(volatile uint16_t *pair_pulses);
void        onewire_write_then_read(uint8_t bit);
void        onewire_pair_bits(const volatile uint16_t *pair_pulses,
                              uint8_t *id_bit, uint8_t *cmp_bit);
uint8_t     onewire_read_data(volatile uint8_t *dst, uint8_t bytes);
void        onewire_decode_pulses(uint8_t *dst, const volatile uint8_t *pulse,
                                 uint8_t nbytes);
void        onewire_encode_byte(ow_pulse_t *out, uint8_t byte);
static inline uint8_t onewire_bit_from_pulse(uint16_t dur);
void        onewire_strong_pullup(uint8_t on);
#if OW_LOW_POWER
uint8_t     onewire_long_wait_pending(void);
void        onewire_sleep_until_done(void);
#endif
uint8_t     onewire_crc8(const uint8_t *data, uint8_t len);
uint16_t    onewire_crc16(const uint8_t *data, uint8_t len);
void        onewire_search_start(onewire_search_sink_t sink,
                                 uint8_t max_devices, uint8_t command,
                                 uint8_t family);
uint8_t     onewire_search_poll(void);
uint8_t     onewire_search_count(void);
uint8_t     onewire_search_active(void);
uint32_t    onewire_config_token(void);
uint8_t     onewire_clock_ok(uint32_t sysclk_mhz);
```

`onewire_write_slots()` and `onewire_read_data()` report whether the operation
was scheduled: they return **1** on success and **0** when the size argument is
out of range and nothing was started (`slots` must be 1..256, `bytes` 1..32).
In firmware the reject path returns 0: the caller observes the 0 instead of a
silent no-op, so an invalid size can never turn into an undiscovered
`onewire_bus_done()` hang. An `assert` is compiled in only under
`DS18B20_TEST_HARNESS` or `OW_TEST_PARAM_GUARD` (and is disabled by `NDEBUG`);
it is not a debug-build feature. `onewire_write_bit()` has no size argument and
always returns 1.

`onewire_present()` takes the capture buffer **and** the number of edges that
reset actually captured, which `onewire_reset_captured()` reads from the capture
DMA's remaining-transfer count. A reset with no device leaves the second slot
untouched, so its contents are whatever the shared buffer held before — possibly
a perfectly valid-looking presence duration. The captured count is the hardware
fact that separates "no second edge" from "a presence pulse was measured":
`onewire_present()` requires both edges before it reports a device. Call
`onewire_reset_captured()` after `onewire_bus_done()` reports the reset complete
and before the next capture is armed.

Both arguments describe that one reset: the buffer the reset was armed with and
how many of its two edges the DMA actually transferred. The count is not
reset-specific - every completed capture (read pair, merged write+read, data
read) drains the same counter, so `onewire_reset_captured()` returns 2 again,
and before the first reset it returns 2 as well. Read against an older buffer
it can therefore confirm a presence pulse that belongs to a previous reset: the
pair is one hardware fact, meaningful only between that reset's
`onewire_bus_done()` and the next capture arm.

There is no byte-oriented command helper: a command is encoded by the caller
with `onewire_encode_byte()` into a slot buffer and then handed to
`onewire_write_slots()`, so the caller owns the buffer geometry. The DS18B20
driver builds its command slots the same way (`src/ds18b20_measure.c`,
`src/ds18b20_txn.c`, `src/ds18b20_resolution.c`). A multi-slot write buffer must
hold `slots + 1` entries, the last one equal to `ONEWIRE_RELEASE_PULSE` — the
hardware bus-release sentinel, not an extra slot (a single-slot write uses a
separate path and reads only entry 0). `onewire_encode_byte()` and the
`ow_pulse_t` type are declared in `inc/onewire.h` alongside the rest of the
layer; `ow_pulse_t` is 8 bits wide on F0/F1/F3/G0/G4/L4/H5 and 16 bits on F4/H7.

Two startup checks guard the two silent mismatches between the archive and the
application. `onewire_config_token()` reports the `OW_CONFIG_TOKEN` the
library was compiled with — compare it with the application's own
`OW_CONFIG_TOKEN` (it mixes `OW_PARASITE_POWER`, `OW_LOW_POWER`,
`OW_DRIVE_ACTIVE`, `OW_STATS_ENABLE`, `OW_BUS_DRIVE`, `DS18B20_MAX_DEVICES`,
`OW_SYSCLK_MHZ` and `OW_TIM1_MHZ`) and refuse to run on a mismatch. `onewire_clock_ok()`
compares a runtime MHz reading (e.g. `SystemCoreClock / 1000000u`) against the
clock the port timing was built for; a return of 0 means the runtime clock does
not match the build, so every 1-Wire slot is scaled.

#### Timing

Slot timing is fixed at **compile time** (`inc/ow_config.h`): four `ONEWIRE_*`
macros define the bit-slot geometry, and every bus operation is scheduled with
those exact durations. There is no runtime profile switching and no timing
state — the values fold into the TIM1/DMA register arithmetic and can be
overridden individually via `-D`.

```c
#define ONEWIRE_ONE_PULSE       5   /* µs: short low = write-1 / read pulse       */
#define ONEWIRE_ZERO_PULSE     60   /* µs: long low = write-0                     */
#define ONEWIRE_GUARD_BAND      5   /* µs: slot tail (external-power default)     */
#define ONEWIRE_SHORT_PULSE_MAX 10  /* µs: pulse <= this decodes as bit '1'       */
void onewire_strong_pullup(uint8_t on);  /* parasite power: drive bus HIGH */
```

On a parasite-powered bus the release margin must be wider: define
`OW_PARASITE_POWER` as 1 (`-DOW_PARASITE_POWER=1`) to select the 100 µs guard
band default, or pass `-DONEWIRE_GUARD_BAND=...` explicitly. The Makefile
presets in [Configuration → Timing](#timing-1) select whole value sets.

Any other 1-Wire slave driver can use the same layer. Each bundled driver calls
`onewire_init()` from its own `*_init()`; the DS18B20 driver also keeps the
layer's Search ROM engine for its `ds18b20_search_*` /
`ds18b20_alarm_search_*` wrappers.

`onewire_long_wait_pending()` and `onewire_sleep_until_done()` exist only in
`OW_LOW_POWER=1` builds. They are the bus-layer vocabulary the bundled drivers
use to idle the core during a long hardware stage; an application normally uses
the driver start/poll pairs and never calls them directly.

The DS2413 dual-channel switch has its own non-blocking driver
(`inc/ds2413.h` + `src/ds2413.c`), documented in
[DS2413 Dual-Channel Switch Driver](#ds2413-dual-channel-switch-driver).

### CRC Utility

```c
uint8_t ds18b20_crc8(const uint8_t *data, uint8_t len);
```
Calculates the Dallas/Maxim CRC-8 used by the driver to validate ROM codes and
scratchpad data. Exposed publicly as a small utility (e.g., for host tools).
It is a thin DS18B20-namespaced wrapper around the shared `onewire_crc8()`
(see [1-Wire Layer](#1-wire-layer-shared)); both compute the same checksum.
The shared layer also provides `onewire_crc16()` — the Maxim CRC-16/ARC
remainder used by the DS2431 (the device transmits the inverted form; the
caller complements it when comparing against the wire).

### Device Search

```c
void ds18b20_search_start(ds18b20_search_sink_t sink, uint8_t max_devices);
uint8_t ds18b20_search_poll(void);
uint8_t ds18b20_search_count(void);
```
Non-blocking Maxim Search ROM (0xF0) over the whole bus, implemented as a
compact state machine that performs exactly one hardware operation per
`ds18b20_search_poll()` call. `sink` is invoked once per found DS18B20 device
with its 8-byte ROM address; `max_devices` caps the reported count. Poll
`ds18b20_search_poll()` from the main loop until it returns 1. The measurement
state machine is never left IDLE, so `ds18b20_start_measure()` works immediately
afterwards; the timer stays idle in the meantime. `ds18b20_search_count()` returns how many
devices were found. Only devices with family code `DS18B20_FAMILY_CODE` (0x28)
are reported. See `examples/3_round_robin/main.c`.

### Alarm Search

```c
void ds18b20_alarm_search_start(ds18b20_search_sink_t sink, uint8_t max_devices);
uint8_t ds18b20_alarm_search_poll(void);
uint8_t ds18b20_alarm_search_count(void);
```
Non-blocking Maxim Alarm Search ROM (0xEC): reports only the DS18B20 devices
currently in alarm state, i.e. whose last measured temperature is outside the
TH/TL thresholds configured with Write Scratchpad (0x4E). It shares the device
search engine, so the callback, `max_devices` cap, family filter and ownership
rules are identical; only the command byte and the reported set differ. Unlike
`ds18b20_search_start()`, the alarm search never repopulates the scan-mode
device table (`ds18b20_device_count()` / `ds18b20_device_rom()` keep the
addresses from the last device search). Poll `ds18b20_alarm_search_poll()` from
the main loop until it returns 1, then read `ds18b20_alarm_search_count()`.

### Per-Device Addressing

```c
void ds18b20_select(const uint8_t *rom);
```
Selects which DS18B20 device the non-blocking measurement path targets, using
its 64-bit ROM address (LSB first, e.g. from a bus search). With a device
selected, the driver sends the **Match ROM (0x55)** command plus the device ROM
before every Convert T / Read Scratchpad operation, so only that device
responds. Pass `NULL` to clear the selection and return to the legacy **Skip
ROM (0xCC)** single-sensor behaviour.

`ds18b20_select()` is only accepted while the measurement state machine is
**IDLE** — calls made while a cycle is running, during a device/alarm search,
a resolution change or another command transaction are ignored. In particular
it is **rejected from the per-device scan callback** (`ds18b20_complete()` in
scan mode): that callback runs at decode time mid-round, so a select there is
ignored and the scan round continues. To switch out of scan mode, call
`ds18b20_select()` from the main loop after the scan completes, then
`ds18b20_scan_start()` to resume simultaneous conversion.

`ds18b20_select()` targets one device and does not automatically cycle through
the devices found by a search. For round-robin measurements, the application
stores the ROM addresses returned by the search and selects the next device
between measurement cycles; see `examples/3_round_robin/main.c`.

To convert all discovered devices in parallel, use the separate scan mode:
`ds18b20_scan_start()` broadcasts one Convert T command, then reads each device
by ROM address and reports results in device-table order. See
`examples/4_scan_mode/main.c`. Scan mode and single-device selection are
mutually exclusive.

### Command Transactions

The remaining DS18B20 commands run with the same non-blocking discipline as the
device search and the resolution change: each transaction owns TIM1/DMA while
it runs (reset → presence → write → read | timed wait) and leaves the bus idle
when it finishes.

```c
void     ds18b20_read_rom(uint8_t *rom);
uint8_t  ds18b20_read_rom_poll(void);
void     ds18b20_set_alarm_thresholds(uint8_t th, uint8_t tl);
uint8_t  ds18b20_set_alarm_thresholds_poll(void);
void     ds18b20_read_scratchpad(uint8_t *buf);
uint8_t  ds18b20_read_scratchpad_poll(void);
void     ds18b20_copy_scratchpad(void);
uint8_t  ds18b20_copy_scratchpad_poll(void);
void     ds18b20_recall_eeprom(void);
uint8_t  ds18b20_recall_eeprom_poll(void);
void     ds18b20_set_parasite(uint8_t parasite);
void     ds18b20_detect_parasite(void);
uint8_t  ds18b20_detect_parasite_poll(void);
uint8_t  ds18b20_parasite_mode(void);
uint8_t  ds18b20_last_command_ok(void);
```

- Every command is a `start`/`poll` pair: call the start function, then poll
  the matching `*_poll()` from the main loop until it returns 1, then call
  `ds18b20_start_measure()` when you want a measurement again. Commands are
  ignored mid-cycle, while a device search, an
  alarm search or a resolution change owns the timer, or while another command
  transaction is still running. Result buffers must stay valid until the
  transaction finishes.
- With a device selected via `ds18b20_select()`, the command is preceded by
  Match ROM (0x55) + device ROM so only that device responds; without a
  selection it broadcasts via Skip ROM (0xCC). `ds18b20_read_rom()` is always
  sent bare (0x33) — valid only when exactly one device is on the bus.
- `ds18b20_read_scratchpad()` returns the raw 9 scratchpad bytes; verify with
  `ds18b20_last_command_ok()` or the CRC byte (`buf[8] ==
  ds18b20_crc8(buf, 8)`). A valid read also updates the auto-derived
  resolution (`ds18b20_get_resolution()`).
- `ds18b20_set_alarm_thresholds(th, tl)` writes TH/TL into the volatile
  scratchpad with Write Scratchpad (0x4E), keeping the current resolution in
  the config byte. The DS18B20 8-bit sign-extended temperature code is used
  (e.g. 0x19 = +25°C, 0x0F = +15°C).
- `ds18b20_copy_scratchpad()` persists TH/TL/CFG to the EEPROM and
   `ds18b20_recall_eeprom()` loads the EEPROM copy back into the scratchpad.
   Both wait the datasheet hold-off (10 ms) with the timer before finishing.
   Recall is a write-only command: it does not return the restored config, so
   the driver's tracked `ctx.resolution` is **not** updated. If the EEPROM
   resolution may differ from the current one, follow `ds18b20_recall_eeprom()`
   with `ds18b20_read_scratchpad()` to resynchronise `ds18b20_get_resolution()`
   before the next conversion (see `examples/5_commands/main.c`, which chains recall → scratchpad
   read for this reason).
- `ds18b20_detect_parasite()` runs a Read Power Supply query and stores the
   answer straight into the driver state — after
  `ds18b20_detect_parasite_poll()` returns 1 (check `ds18b20_last_command_ok()`)
  the wiring is configured and `ds18b20_parasite_mode()` reports it. On a mixed
  bus the open-drain answer is a wired-AND: any externally powered device masks
  the parasite report, so detect per-device in Match ROM addressing mode for
  heterogeneous wiring.
- `ds18b20_set_parasite(1)` enables parasite-power support (default 0 —
  external VDD wiring, no pin mode changes). The strong pull-up is harmless
  for externally powered devices, so a mixed bus works with the flag set —
  mechanism and wiring budget in [Parasite Power](#parasite-power).
- `ds18b20_last_command_ok()` reports whether the last transaction found a
  device present (and, for read commands, read its data back).

Example — set TH/TL and persist them to the EEPROM:

```c
ds18b20_set_alarm_thresholds(0x19, 0x0F);   // +25°C / +15°C
while (!ds18b20_set_alarm_thresholds_poll()) {
    /* keep calling from the main loop */
}
ds18b20_copy_scratchpad();                  // persist to the EEPROM
while (!ds18b20_copy_scratchpad_poll()) {
    /* keep calling from the main loop */
}
```

### Simultaneous Multi-Device Conversion

```c
void ds18b20_scan_start(void);
uint8_t ds18b20_device_count(void);
const uint8_t* ds18b20_device_rom(uint8_t index);
uint8_t ds18b20_scan_index(void);
```

Convert every discovered device in parallel. `ds18b20_scan_start()` switches the
driver into scan mode, then `ds18b20_start_measure()` runs one round: a
broadcast `Convert T` (Skip ROM 0xCC) so all sensors convert simultaneously,
followed by reading each one back via Match ROM in device-table order, with
every result reported through `ds18b20_complete()`. N devices take one
conversion wait plus N reads instead of N conversion waits. A missing device
reports `DS18B20_TEMP_ERROR_NO_SENSOR` and the scan continues. See
`examples/4_scan_mode/main.c`.

- The device table must be populated first by the non-blocking device search
  (`ds18b20_search_*`).
- Scan mode assumes a single resolution across all sensors (the config is written
  broadcast) and is mutually exclusive with the single-device
  `ds18b20_select()` addressing — calling `ds18b20_select()` clears scan mode,
  call `ds18b20_scan_start()` again to resume.
- `ds18b20_device_count()` returns how many DS18B20 devices are stored;
  `ds18b20_device_rom(index)` returns the 8-byte ROM (LSB first) of one of them,
  or NULL for an out-of-range index (the pointer is valid until the next search).
- `ds18b20_scan_index()` returns the index of the device whose result
  `ds18b20_complete()` just reported (valid during scan mode).

Example:

```c
ds18b20_scan_start();     // enter scan mode (once)
ds18b20_start_measure();   // request one simultaneous-conversion round
while (1) {
    ds18b20_poll();     // scan reports each device via ds18b20_complete()
    // ... when the next round is due, call ds18b20_start_measure() again ...
}

// inside ds18b20_complete(): identify the sensor
void ds18b20_complete(int16_t temp) {
    uint8_t idx = ds18b20_scan_index();
    printf("Sensor %u: %d.%d C\n", idx, temp / 10, abs(temp % 10));
}
```

### Parasite Power

With VDD tied to GND the DS18B20 draws its operating current from the data
line. The bus pull-up then has to deliver the conversion current — about
**1.5 mA per converting sensor** — for the whole conversion window (up to
750 ms at 12-bit), which a passive resistor cannot do. The driver solves this
by switching the bus pin to push-pull HIGH for every conversion and EEPROM
hold-off window (`ds18b20_set_parasite(1)`). It releases the pin back to the
passive pull-up only where the bus has to be free — the reset pulses and the
scratchpad read — and deliberately keeps it engaged while parked at IDLE.

```c
ds18b20_init();
ds18b20_detect_parasite();          // query the wiring (0xCC + Read Power Supply)
while (!ds18b20_detect_parasite_poll()) {
    /* the transaction owns TIM1/DMA: ds18b20_poll() returns immediately by
       design, so only ds18b20_detect_parasite_poll() advances it */
}
/* ds18b20_parasite_mode() now reflects the detected wiring */
```

Wiring guidance, from bench validation on an STM32F103 with a 2.2 kΩ pull-up —
deliberately stronger than the 4.7 kΩ recommended for externally powered buses
(see [Hardware Connections](hardware.md#hardware-connections)), because on a parasite bus
the pull-up must also source the conversion current:

- A handful of sensors on short wires (<30cm) converts reliably on the MCU pin
  alone: a six-device broadcast cycle completed without a single CRC error.
- Budget ~1.5 mA per simultaneously converting sensor against the pin's drive
  capability (roughly 20-25 mA source depending on the family) and the VOH droop
  across your pull-up
  arrangement; keep the bus HIGH above the DS18B20's ~2.96 V minimum.
- For longer buses or larger fleets use a dedicated strong-pull-up IC, or add
  a P-MOSFET high-side switch driven by its own GPIO. The current API cannot
  drive a separate gate: `onewire_strong_pullup()` only switches the bus pin
  itself to push-pull HIGH, while a P-MOSFET needs a LOW gate to conduct. A
  gate-pin option would be a port/API extension, not a wiring change.

The example applications accept a compile-time flag to run over parasite
wiring out of the box:

```bash
make APP=1_basic EXT="-DOW_PARASITE_POWER=1"        # all seven DS18B20 examples gate ds18b20_set_parasite(1) on it
```

### Signal Statistics Module (`ow_stats`)

An optional compile-in module that collects per-sensor pulse-width statistics
and a global histogram across measurement cycles.  Enabled by defining
`OW_STATS_ENABLE=1` at build time.  When the macro is 0, every inline
body compiles away to nothing — zero overhead in production builds.

```c
#include "ow_stats.h"

void ow_stats_init(void);
void ow_stats_capture_pulse(const volatile uint8_t *pulse, uint8_t n,
                            const uint8_t *rom);
void ow_stats_count_error(int16_t error, const uint8_t *rom);
void ow_stats_capture_overrun(void);
void ow_stats_dump_start(void);
uint8_t ow_stats_dump_poll(void);
void ow_stats_reset(void);
uint32_t ow_stats_tick(void);
```

- `ow_stats_init()` — zero-initialise the statistics context.  Call once at
  startup.
- `ow_stats_capture_pulse()` — snapshot raw pulse widths before
  `decode_scratchpad()` overwrites the capture buffer via the union alias.
  Updates the 13-bucket logarithmic histogram (0–60+ µs; 13 of the 16
  `OW_STATS_HIST_BUCKETS` array slots are populated, indices 0–12;
  buckets 0–2, 3–4, 5–6, 7–9, 10–12, 13–14, 15–19, 20–24, 25–29,
  30–39, 40–49, 50–59, 60+ µs) and per-sensor min/max pulse counters.  Called
  automatically from `ds18b20.c` when `OW_STATS_ENABLE=1` is set.
- `ow_stats_count_error()` — record an error event: a CRC or reserved-byte
  mismatch, or an addressed device that answered with an all-0xFF frame
  (reported as `NO_SENSOR`).  A device that fails the presence check after a
  reset is reported through `ds18b20_complete()` but is **not** counted here.
  Called automatically from the DECODE state of `ds18b20.c`.
- `ow_stats_capture_overrun()` — record a `TIMx_SR.CC4OF` event: a capture
  arrived before the DMA read the previous one. The port calls it when the
  operation completes, so a starved capture is distinguishable from an empty
  bus even when the DMA counters are not readable. The dump's total line ends
  with `<n>o`.
- `ow_stats_dump_start()` — begin a non-blocking UART dump.  Call from the
  main loop after the desired number of cycles (tracked via `ow_stats_tick()`).
- `ow_stats_dump_poll()` — advance the dump by one line (header, sensor line,
  histogram or total).  Non-blocking: each call enqueues one line into the
  UART TX ring buffer; the main loop drains it via `uart_poll_tx()`.  Returns
  1 when the dump is complete.
- `ow_stats_reset()` — zero all counters and the histogram, keep the sensor
  ROM table.  Call after `ow_stats_dump_poll()` returns 1.
- `ow_stats_tick()` — increment the cycle counter; returns the new value.  The
  module never increments it by itself: `t=` in a dump line is whatever the
  application counts with this call (the `6_statistics` demo ticks once per
  reported measurement, so there it is a count of measurements, not of dumps).

The dump also needs five weak output hooks the application provides as strong
definitions: `ow_stats_putchar()`, `ow_stats_puts()`, `ow_stats_print_int()`,
`ow_stats_print_hex()` and `ow_stats_tx_enqueue()`.  Defaults in `ow_stats.c`
are no-ops, so without them the dump stays silent; the shared example layer
`examples/app/app.c` (linked into every example) implements them on top of the
UART TX ring buffer.

RAM cost: ~304 bytes (8 sensors × 28 B + 16-entry `uint32_t` histogram [64 B] +
cycle/error/overrun counters + 8 B dump state; 13 of the 16 histogram buckets,
indices 0–12, are populated).

Example — dump every `STATS_DUMP_SWEEPS` sweeps (default 10). A sweep is
one pass over every sensor, so the period is a count of passes rather than
of individual measurements, and it reads the same on a one-sensor bus as on
a seven-sensor one:

```c
#include "ow_stats.h"

#ifndef STATS_DUMP_SWEEPS
#define STATS_DUMP_SWEEPS 10u
#endif

static uint8_t dump_busy = 0;
static uint32_t sweep_count = 0;

void ds18b20_complete(int16_t temp) {
    // ... handle temperature reading ...
    // ... advance to the next sensor, wrapping back to index 0 ...
    if (select_index == 0) { /* one sweep completed */
        sweep_count++;
    }
    (void)ow_stats_tick(); /* keeps total_cycles a count of measurements */
    if (sweep_count >= STATS_DUMP_SWEEPS && !dump_busy) {
        ow_stats_dump_start();
        dump_busy = 1;
    }
}

int main(void) {
    ow_stats_init();
    // ... ds18b20_init(), device search, ds18b20_start_measure() ...
    for (;;) {
        if (dump_busy) {
            if (ow_stats_dump_poll()) {
                dump_busy = 0;
                ow_stats_reset();
                ds18b20_start_measure(); // resume measuring with a clean window
            }
        } else {
            ds18b20_poll();
        }
    }
}
```

Build with the statistics module:

```bash
make APP=6_statistics                                  # external power (OW_STATS_ENABLE auto-added)
make APP=6_statistics EXT="-DOW_PARASITE_POWER=1"            # parasite power
```

> Note: the `6_statistics` target injects `-DOW_STATS_ENABLE=1` and nothing
> else. The period is the example's own `STATS_DUMP_SWEEPS`, defined once in
> `examples/6_statistics/main.c`, so it is overridable with
> `EXT="-DSTATS_DUMP_SWEEPS=N"`. The demo needs no inter-measurement pause: it
> requests the next cycle as soon as the dump is done, so the conversion time
> itself is the cadence and how long a batch takes depends on how many sensors
> are on the bus. A `---` separator marks each sweep, so the wait is visible
> rather than silent.

UART output format (compact, one sensor per line):

```text
--- stats [100 c] ---
28 20 78 92 07 00 00 67:5-29 n17 e0
28 78 B8 AC 0B 00 00 2C:5-31 n17 e0
28 64 69 AB 0B 00 00 1F:5-29 n17 e0
28 FC AE AA 0B 00 00 F3:5-30 n17 e0
28 7E 63 AD 0B 00 00 3F:5-30 n16 e0
28 F1 39 AD 0B 00 00 D9:5-30 n16 e0
h:2=3304 8=2023 9=1873
t=100c 0e
```

Fields per sensor line: `ROM:min-max n=count e=errors` (errors = CRC +
no-sensor + other combined).  Histogram shows only non-empty buckets;
`h:B=count` where B is the bucket index.  Total line: `t=Nc Ee` where N =
cycle count, E = total error count.

### Resolution Change

```c
void ds18b20_set_resolution(uint8_t bits);
uint8_t ds18b20_set_resolution_poll(void);
uint8_t ds18b20_get_resolution(void);
```

Change the temperature conversion resolution between measurement cycles,
non-blocking and without interrupts, mirroring the device search state machine:

- `bits` is the new resolution in bits — `DS18B20_RES_MIN` (9) …
  `DS18B20_RES_MAX` (12). Out-of-range values are ignored. The change is only
  accepted while the measurement state machine is IDLE and no device search is
  running; it is ignored otherwise.
- The configuration is written to the volatile scratchpad with **Write
  Scratchpad (0x4E)** (TH/TL are reset to 0, disabling the alarm triggers) and
  takes effect immediately; it is **not** persisted to the EEPROM (Copy
  Scratchpad would need a strong pull-up under parasitic power).
- Poll `ds18b20_set_resolution_poll()` from the main loop until it returns 1,
  then call `ds18b20_start_measure()` for the next cycle. That measurement waits
  exactly as long as
  the new resolution requires (e.g. 93.75 ms at 9-bit instead of 750 ms).
- `ds18b20_get_resolution()` returns the current resolution. It is updated by a
  successful resolution change and auto-derived from every valid scratchpad
  read (byte 4, R1/R0), so it also tracks a resolution changed externally.

Example — drop to 9-bit to measure 8× faster:

```c
ds18b20_set_resolution(9);
while (!ds18b20_set_resolution_poll()) {
    /* keep calling from the main loop; the poll never waits for a result */
}
/* ds18b20_get_resolution() == 9; the next ds18b20_start_measure() uses the
   fast wait */
```

### Weak Callbacks

```c
void ds18b20_busy(unsigned action);
```
Called to indicate busy/idle status — toggle an LED, for example. `action` is non-zero for busy (measurement in progress), 0 for idle.

```c
void ds18b20_complete(int16_t temp);
```
Called when a measurement cycle completes — provides temperature data in tenths of degrees Celsius, or an error code (`DS18B20_TEMP_ERROR_*`).

### Error Codes

- `DS18B20_TEMP_ERROR_NO_SENSOR`: No sensor detected on the bus.
- `DS18B20_TEMP_ERROR_CRC_FAIL`: Data corruption detected via CRC mismatch.
- `DS18B20_TEMP_ERROR_GENERIC`: Unspecified communication error.

### DS2431 EEPROM Driver

The DS2431 1 kbit EEPROM has its own non-blocking driver (`inc/ds2431.h` +
`src/ds2431.c`) on the shared 1-Wire layer. `ds2431_init()`, `ds18b20_init()`
and `ds2413_init()` may all run in one image before any bus operation; after
that the three drivers take turns owning the bus, one operation at a time, and
are never polled concurrently (see `examples/9_multi`). Address DS2431s with
`ds2431_select()`.

```c
void    ds2431_init(void);
void    ds2431_select(const uint8_t* rom);
void    ds2431_set_parasite(uint8_t parasite);
uint8_t ds2431_parasite_mode(void);
uint8_t ds2431_last_command_ok(void);
```

- `ds2431_select()` — Match ROM to one device (8-byte ROM); accepted only while
  the driver is idle.
- `ds2431_set_parasite(1)` — strong pull-up hold-off for Copy Scratchpad under
  parasite power; `ds2431_parasite_mode()` reads it back.
- `ds2431_last_command_ok()` — 1 when the last command completed cleanly: the
  Read Scratchpad TA/ES echo matched, the device did not flag the scratchpad
  invalid (PF), the written data read back unchanged, and a Copy Scratchpad
  returned the device's 0xAA completion pattern.

```c
void    ds2431_read_memory(uint8_t* dst, uint16_t addr, uint8_t len);
uint8_t ds2431_read_memory_poll(void);
uint8_t ds2431_write_scratchpad(uint16_t addr, const uint8_t* data, uint8_t len);
uint8_t ds2431_write_scratchpad_poll(void);
void    ds2431_read_scratchpad(uint8_t* buf);
uint8_t ds2431_read_scratchpad_poll(void);
void    ds2431_copy_scratchpad(void);
uint8_t ds2431_copy_scratchpad_poll(void);
```

Each operation is a non-blocking state machine: start it, then poll `*_poll()`
from the main loop until it returns 1. Read Memory (0xF0) reads 1-8 bytes;
Write Scratchpad (0x0F) fills the 8-byte page-aligned block, Read Scratchpad
(0xAA) reads it back plus the TA/ES echo and verifies the data against the
payload that was written, and Copy Scratchpad (0x55) programs the EEPROM page,
its poll honouring the t_PROG hold-off and then checking the device's completion
pattern. `ds2431_read_scratchpad()` requires a buffer of
`DS2431_SCRATCH_FRAME` bytes (11: TA1/TA2/ES plus eight data bytes); it returns
`(E[2:0] - T[2:0] + 1)` data bytes after the header (the driver clocks the
maximum reply, verifies the CRC over what was sent and zeroes the unused tail),
and rejects a set PF flag — a short Write Scratchpad, one that does
not fill the whole block, makes the device flag the scratchpad invalid and
refuse the later copy, so both the read-back and the Copy report failure. The
full cycle — Read Memory → Write Scratchpad → Read Scratchpad → Copy Scratchpad
→ Read Memory — is in `examples/8_ds2431`.

### DS2413 Dual-Channel Switch Driver

The DS2413 has two open-drain PIO pins. Its non-blocking driver
(`inc/ds2413.h` + `src/ds2413.c`) shares the 1-Wire layer and the one-operation
at-a-time bus rule with the DS18B20 and DS2431 drivers. All three inits may run
before the first bus operation.

```c
void    ds2413_init(void);
void    ds2413_select(const uint8_t* rom);
uint8_t ds2413_read_pio(uint8_t* out);
uint8_t ds2413_read_pio_poll(void);
uint8_t ds2413_write_pio(uint8_t states);
uint8_t ds2413_write_pio_poll(void);
uint8_t ds2413_last_command_ok(void);
uint8_t ds2413_last_pio(void);
uint8_t ds2413_last_latches(void);
uint8_t ds2413_last_reply(void);
```

- `ds2413_select()` — Match ROM to one device, or `NULL` for Skip ROM. Broadcast
  write is useful to set several switches at once; broadcast read is only
  meaningful with one DS2413 on the bus, because several devices answering at
  once produce a wired-AND the driver cannot attribute.
- `ds2413_read_pio(&state)` — PIO Access Read (0xF5). The reply is validated
  (0xFF = no answer, low nibble must be the complement of the high nibble) and
  split into the two halves of the low nibble: bit 0 = PIOA pin, bit 1 = PIOA
  latch, bit 2 = PIOB pin, bit 3 = PIOB latch. `ds2413_last_pio()` and `state`
  carry the **actual pins** normalized to bits 0/1 (`DS2413_PIOA`,
  `DS2413_PIOB`); `ds2413_last_latches()` carries the output latches in the same
  layout. An externally driven pin therefore reads differently from its latch.
- `ds2413_write_pio(states)` — PIO Access Write (0x5A). Both output latches are
  written together; take the current latch byte from `ds2413_last_latches()` to
  change one channel, because a pin read cannot recover an overridden latch. The
  driver sends the state byte with bits 2-7 set to 1 and its complement, then
  requires the 0xAA acknowledge.
- `ds2413_last_command_ok()` — 1 for a valid read reply or a 0xAA write
  acknowledge; `ds2413_last_reply()` exposes the raw byte for diagnostics.

Every operation primes the parasite-powered device with a 1 ms strong pull-up,
then runs reset → presence → command → reply. A failed reply is retried up to
three times (the same count as the Linux `w1_ds2413` driver); the whole sequence
stays non-blocking. `examples/9_multi` searches for the DS2413 and alternates
PIOA/PIOB every 500 ms while it waits between measurement cycles.

### Multi-Device Application (`9_multi`)

`examples/9_multi` is the end-to-end application: a peak/low temperature logger
with seven DS18B20 sensors, one DS2431 EEPROM and a DS2413 switch (when present)
on the same wire. It is also the reference for using three drivers in one image.
They cannot be polled at the same time, so the application owns one explicit
bus-owner state machine and hands the bus over in a fixed order:

1. DS18B20 search, which fills the driver's scan-mode device table;
2. a second full-family search to locate the DS2431 (the search engine runs one
   family filter per pass);
3. a boot read of the four EEPROM blocks into RAM — the EEPROM is only written
   again when a record actually changes;
4. measurement rounds: one broadcast `Convert T` for every sensor, each result
   reported through `ds18b20_complete()`. If a scan round fails, the application
   switches to the sequential Match ROM path;
5. a new round maximum or minimum is queued as Write Scratchpad → Read
   Scratchpad (echo + data verify) → Copy Scratchpad → Read Memory;
6. while waiting between rounds, the DS2413 PIOA/PIOB latches alternate every
   500 ms and are read back.

Records are 8-byte, self-validating blocks (magic, version, flags, update
counter, whole degrees, tenths, sensor index, CRC-8) at EEPROM addresses 16..47,
and the sensor's ROM block is written before the record that refers to it — a
power loss cannot leave a record pointing at a ROM that is not there. The
records only move in one direction (the peak grows, the low falls), so after
warm-up writes are rare. Build with `make APP=9_multi`; `PEAK_RESET=1` ignores
the stored records at boot and forces the write path, while `CLEAR_RECORDS=1`
erases the four record/ROM blocks before a fresh capture and then stops.

## Performance

- Time to result (one measurement): 93.75 ms @ 9-bit … ~0.76 s @ 12-bit
  (conversion + protocol overhead; the conversion wait follows the configured
  resolution, see `ds18b20_set_resolution()`)
- Measurement cadence: **application-defined**. The driver measures one cycle per
  `ds18b20_start_measure()` and then parks, so it has no inter-measurement
  interval of its own; the examples pace themselves (5 s, see
  `MEASURE_PERIOD_MS` in `examples/*/main.c` — except `6_statistics`, which runs
  with no time base at all and paces itself with the conversion time — and
  `app_time_init()` in the
  shared app layer)
- Precision: 0.1°C reported (API tenths; the sensor step at 12-bit is
  0.0625°C — coarser steps at lower resolutions)
- Accuracy: ±0.5°C (typical)
- CPU Usage: Minimal; CPU is free to perform other tasks during waits.

### Footprint

The driver keeps all of its state in `.bss`; the flash cost is the driver plus
the application. `make test-ram` (`tests/check_ram_budget.sh`) gates both with
exact ceilings, so any growth has to be a deliberate budget change. The numbers
below are the ceilings enforced there.

Driver-owned `.bss` is the sum of the DS18B20 context, search and transaction
statics in the `5_commands` image (the one example that links every context at
once):

| Family | Driver `.bss` | Total `.bss`, `5_commands` | Total `.bss`, `1_basic` |
| --- | ---: | ---: | ---: |
| STM32F0 | 329 B | 652 B | 432 B |
| STM32F4 | 434 B | 756 B | 536 B |
| STM32G4 | 329 B | 652 B | 432 B |
| STM32L4 | 329 B | 652 B | 432 B |
| STM32H5 | 329 B | 652 B | 432 B |

The 105-byte difference on F4 is the 16-bit `ow_pulse_t` feed table; G4, L4 and
H5 latch 8-bit entries like F0. The F1, F3 and H7 backends are not part of this
gate; F1/F3 use the 8-bit layout and H7 the 16-bit one, so expect their numbers
to land in the same range as F0 and F4 respectively.

Whole-image ceilings on the binding part (STM32F030x6, 16 KB flash / 4 KB RAM):

| Example | `.text` | Total `.bss` |
| --- | ---: | ---: |
| `0_dma_pressure` | 2456 B | 1484 B |
| `6_statistics` | 6336 B | 1788 B |
| `8_ds2431` | 6156 B | 768 B |
| `9_multi` | 12792 B | 1456 B |

`6_statistics` owns the largest `.bss` (1024 B UART ring + ~304 B stats state);
`9_multi` owns the largest `.text` (DS18B20 + DS2431 + DS2413 in one image,
about 78% of the 16 KB flash). The DS2431 and DS2413 drivers are dropped by
`--gc-sections` from images that never call them.

## Configuration

### Part selection: one `OW_MCU` key

A single build knob names the exact STMicroelectronics part by its standard
device macro — the same string the CMSIS device header is named after:

```bash
make OW_MCU=STM32F401xC APP=4_scan_mode   # Makefile
cmake -S . -B build -DOW_MCU=STM32F401xC  # CMake (same chips/*.mk)
```

The macro does two jobs at once, and the split between them is the whole
point of how the project is laid out:

- **Family** — the last part of the macro (`F4`, `F0`, `G0`, ...) picks the
  driver code: the `src/ow_mcu.h` section, the timer and DMA resources, the bus
  pin. One family section per family. This is where the
  driver code differs.
- **Part** — the full macro names a file in `chips/`. The parts of one
  family share an `src/ow_mcu.h` section — the F4 section has no per-part conditional at
  all, because TIM1/DMA2 and `CHSEL=6` map identically across the F4 parts.
  What actually differs is the CMSIS device layer, the linker script
  (memory sizes), the debugger projects and the default clock, so that is
  exactly what `chips/<part>.mk` holds:

  | Variable | Meaning |
  |---|---|
  | `CHIP_DEV_DEF` | CMSIS device macro (`-DSTM32F401xC`) |
  | `CHIP_DEVICE_HDR` | device header fetched by `make download-deps` |
  | `CHIP_STARTUP` | startup assembly fetched by `make download-deps` |
  | `CHIP_LINKER` | linker script — the memory map is the part's own |
  | `CHIP_JFLASH` / `CHIP_JDEBUG` | J-Flash (where available) and SEGGER Ozone projects |
  | `CHIP_SVD` | register view for debuggers (optional; no upstream SVD for H503/H743) |
  | `CHIP_HSE_MHZ` | the board crystal the part file is named after (F3/F4/G4/L4/H5/H7), overridable via `HSE_MHZ` |
  | `CHIP_SYSCLK_MHZ` | the part's default clock, overridable via `SYSCLK_MHZ` |

Both build systems read the same file, so they cannot drift on the device macro,
startup file, linker script or clock.

Parts are named after the CMSIS device macro, lowercased — `STM32F401xC`
becomes `f401xc`, `STM32F103xB` becomes `f103xb` — so the name says which
flash/pin-density code it is. `make test-chips` (part of `make test-clocks`)
checks that every part's repository-side files exist and that the two scalars
are well formed, and that an unknown family or part is **rejected** rather than
silently falling back to the F1 default. That last part matters: an
unrecognised `OW_MCU` used to fall through to the F1 branch and produce a
perfectly valid Blue Pill binary.

Legacy aliases: `OW_TARGET=<family>` and `OW_CHIP=<part>` still select the
same part file as before (e.g. `OW_TARGET=f4 OW_CHIP=f401xc` ≡
`OW_MCU=STM32F401xC`), but they are no longer the documented interface.

Two rules when editing a part file: keep it to plain `NAME = value` assignments
(no includes or conditionals — the CMake build parses it), and do not put a
comment at the end of an assignment line, because make keeps the whitespace in
front of `#` as part of the value and every path in it silently stops existing.

### Timing

Slot timing is fixed at **compile time** via the `ONEWIRE_*` macros in
`inc/ow_config.h` (see also the [API Reference → Timing](#timing)). Each macro is
overridable with `-D`, so a specific board or bus length pins its values without
any runtime state. The reset-pulse bounds are defined in `src/onewire.c`:

```c
/* inc/ow_config.h — defaults */
#define ONEWIRE_ONE_PULSE           5     // µs (short low = write-1)
#define ONEWIRE_ZERO_PULSE         60    // µs (long low = write-0)
#define ONEWIRE_GUARD_BAND          5     // µs (built into slot formula)
#define ONEWIRE_SHORT_PULSE_MAX    10    // µs (pulse <= this reads as bit '1')

/* src/onewire.c */
#define RESET_PULSE_MIN           480U    // µs
#define RESET_PULSE_MAX           540U    // µs
```

Slot formula:

```text
ARR = one_pulse + zero_pulse + guard_band
```

The Makefile presets set all four values at once (`make TIMING=SLOW`):

| Preset     | `one` | `zero` | `guard` | `short≤` | Slot  |
|------------|------:|-------:|--------:|---------:|------:|
| FAST       | 5 µs  | 60 µs  | 3 µs    | 10 µs    | 68 µs |
| STANDARD   | 5 µs  | 60 µs  | 5 µs    | 10 µs    | 70 µs |
| SLOW       | 8 µs  | 90 µs  | 20 µs   | 15 µs    | 118 µs|
| ROBUST     | 10 µs | 110 µs | 30 µs   | 18 µs    | 150 µs|
| CUSTOM     | 1 µs  | 60 µs  | 1 µs    | 15 µs    | 62 µs |

SLOW / ROBUST widen the bit slot (118 µs / 150 µs instead of 70 µs), which trades
**protocol throughput** — every command and every bit read costs proportionally
more time — for timing margin. They do **not** change the conversion wait: that
is fixed by the resolution alone (93.75 ms … 750 ms). They are intended for
long wiring, parasite buses or electrically noisy setups.

On a parasite-powered bus the strong-pullup release must not clip the sensor's
slot sampling, so the guard band needs to be wider than the 5 µs external-power
default. Compile with `-DOW_PARASITE_POWER=1`: when `ONEWIRE_GUARD_BAND` is not
defined by a preset or an explicit `-D`, it defaults to 100 µs. Presets always
pin the guard explicitly, so on long or noisy parasite wiring pass the value
directly instead — e.g. `make TIMING=ROBUST EXT="-DONEWIRE_GUARD_BAND=250"`.

CUSTOM uses the minimum slot timing allowed by the 1-Wire standard
(t_LOW1 = 1 µs, t_LOW0 = 60 µs, t_REC = 1 µs). It is an experimental setting:
a 1 µs read/write pulse is **below the values validated on hardware** (a 2 µs
pulse already broke slot decoding on an F030 at 8 MHz — see also the note in
`inc/ow_config.h`). Use it only for experiments or electrically ideal setups.

**OC3 output-compare preload (`OC3PE`).** The write/capture paths that must
leave the bus idle-HIGH at the terminal update (capture, read-pair, single-slot
write) gate the `TIM_CCMR2` preload bit behind `-DOW_PORT_OC3PE=1` (default) /
`-DOW_PORT_OC3PE=0`. The DMA-fed paths leave it off unconditionally, because
their reload must act immediately — preload would break it. The knob exists so
a bench can sweep the bit without editing code; see `src/ow_port.h` for the
rationale.
