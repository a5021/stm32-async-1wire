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
