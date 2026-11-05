# Migrating from earlier releases

[Back to the README](../README.md)

## Upgrading: the renamed macros

The public macros lost the `PORT_` infix, because the port layer is private to
the library and a public macro named after it invited consumers to depend on
something they cannot see. There are no compatibility aliases — the old names
are gone, deliberately. 3.0.1 makes every one of them a hard `#error` that
names the replacement, so a missed migration fails at build time instead of
silently compiling with the renamed knob at its default.

| before | after | what it is |
| --- | --- | --- |
| `OW_PORT_FAMILY_F0` … `OW_PORT_FAMILY_H7` | `OW_FAMILY_F0` … `OW_FAMILY_H7` | the family selection token (`inc/ow_board.h`) |
| `OW_PORT_SYSCLK_MHZ` | `OW_SYSCLK_MHZ` | the system clock the build compiles in |
| `OW_PORT_LOW_POWER` | `OW_LOW_POWER` | the opt-in WFE sleep path |
| `OW_PORT_F4_MAX_SYSCLK_MHZ` | `OW_F4_MAX_SYSCLK_MHZ` | the F4 part ceiling |
| `OW_PORT_G4_MAX_SYSCLK_MHZ` | `OW_G4_MAX_SYSCLK_MHZ` | the G4 part ceiling |
| `OW_PORT_H5_MAX_SYSCLK_MHZ` | `OW_H5_MAX_SYSCLK_MHZ` | the H5 part ceiling |
| `PARASITE_POWER` | `OW_PARASITE_POWER` | the older parasite-power flag |

What did **not** change: the header names (`onewire.h`, `ow_board.h`,
`ow_config.h`, `ow_stats.h`, the driver headers), `ow_pulse_t` and every
`ONEWIRE_*` / `OW_STATS_*` / `DS18B20_*` / `DS2431_*` name, the CMake cache
knobs (`OW_MCU`, `OW_SYSCLK_MHZ`, …), the Makefile knobs (`SYSCLK_MHZ`,
`HSE_MHZ`, `OW_MCU`, `EXT`), and `-Iinc` as the only include path a consumer
needs. The engine's own vocabulary (`OW_PORT_*`, `ow_port_*`) is unchanged and
still private.

## What else changed in 3.0

The macro rename is the mechanical part. Four more changes can break a build or
affect code that used the bus layer directly.

### The engine is private, and `ow_board.h` is new

`inc/ow_port.h` and `inc/ow_mcu.h` no longer exist. The TIM1+DMA engine is
`src/ow_port.h`, the per-family register facts are `src/ow_mcu.h`, and only
`src/onewire.c` reaches them. `onewire.h` now includes `ow_config.h` and the new
public `inc/ow_board.h`, which carries the `OW_FAMILY_*` token, `ow_pulse_t` and
the board clock defaults.

If you included `ow_port.h`/`ow_mcu.h` or called `ow_port_*` directly, move to
the `onewire_*` API. The installed package ships the public headers only
(`onewire.h`, `ow_board.h`, `ow_config.h`, `ow_stats.h` and the driver
headers); `-Iinc` still suffices.

### `onewire_start_timer()` and `onewire_kick()` are gone

These were the driver's calls into the engine, not bus primitives, and are now
private as `ow_glue_start_timer()` and `ow_glue_kick()` in `src/ow_glue.h`.
Application code must not schedule a timed wait or kick the state machine; the
DS18B20/DS2431 drivers own that sequence.

### `onewire_present()` needs the captured edge count

Presence is no longer encoded as a zeroed second capture slot. The reset
geometry is one hardware fact, so the function takes the number of edges the
reset actually captured:

```c
uint8_t onewire_reset_captured(void);
uint8_t onewire_present(const volatile uint16_t *pulses, uint8_t captured);
```

Call `onewire_reset_captured()` after `onewire_bus_done()` reports the reset
complete and before the next capture is armed. A reset that captured only the
master-release edge reports `captured == 1` and cannot produce a device. The
bundled drivers and the search engine already follow this contract.

### Both driver inits may run before the first bus operation

The 2.x headers said a second `onewire_init()` and mixing DS18B20 with DS2431 in
one image were unsupported. That was stricter than the code: `ds18b20_init()`
and `ds2431_init()` may both run before any bus operation, and each
`onewire_init()` re-applies the same idle configuration. A second init is still
**not** a reset and must not be called during activity; only bus *ownership* is
exclusive, one operation at a time. `examples/9_multi` relies on this.
