# STM32F1 backend (STM32F103CB) — hardware notes

Bench notes for the F1 backend (the `src/ow_mcu.h` F1 section): what was
measured on real hardware, and how. The operating invariants live in the code
comments and in the "Required Timer Capabilities" section of `README.md`;
this file keeps the experiments.

Everything below was measured on the **STM32F103CB** ("Blue Pill") reference
bench: 8MHz crystal, 72MHz SYSCLK, 7 DS18B20 on one bus **in parasite
power**, console on a CP210x at 115200. The bus is shared with a second
DS2431 (the EEPROM driver uses the same bus).

## The wire geometry, measured with a logic analyzer

`tests/hil/la_capture.py` captures the 1-Wire line (PA10) with a Saleae
Logic (`sigrok-cli`, channel D0) and checks the measured shape against the
configuration in `inc/ow_config.h`:

```bash
python tests/hil/la_capture.py --parasite     # the parasite build: guard 100 us
python tests/hil/la_capture.py               # the default build: guard 5 us
```

Measured (4 MHz sample rate, the edge quantisation is a quarter of a
microsecond, so the readings are 1 us above the nominal value):

| | parasite bus | default bus | configured |
| --- | --- | --- | --- |
| bit slot | 166.0 us | 71.0 us | ONE 5 + ZERO 60 + guard 100 / 5 |
| '1' pulse (master pulls low) | 5.0 us | 5.5 us | `ONEWIRE_ONE_PULSE` = 5 |
| '0' pulse (master pulls low) | 60.0 us | 60.5 us | `ONEWIRE_ZERO_PULSE` = 60 |
| reset pulse | 481.5 us | 482.5 us | `OW_PORT_RESET_PULSE_DURATION` = 480 |
| presence pulse (slave answers) | 128.5 us | 128.5 us | device-side |

The guard band is what moves between the two columns: the parasite build
widens it to 100 us so the line is released cleanly for a device that is
drawing its power from it.

**What this is for.** The port's schedule is what a consumer's bus has to
live with, so a change to it has to be visible on the wire and not only in a
register. The capture above was taken with the same firmware before and after
the 3.0.0 work (the encapsulation of the engine, the `OW_PORT_*` → `OW_*`
rename, the driver stating a wait in microseconds instead of timer
registers): the measured pulse widths and slot periods were **identical**
before and after, and the firmware images differ. The wait decomposition
changed - the conversion wait is now a different arrangement of the same
total - and that is not visible on the wire, because the line is idle for the
whole wait either way.

## The bench as it stands

| | |
| --- | --- |
| sensors | 7 DS18B20 + 2 DS2431 on one bus, parasite power |
| console | CP210x, 115200, on the same board |
| debugger | ST-Link (V2), 3.25 V at the target |

The HIL harness (`make test-hil`) drives the examples on this bench:
`smoke` (7 devices, 13 readings), `lowpower` (the WFE path), `ds2431` (the
EEPROM cycle, read-back compared), `scan` and `stats`. All of them pass
here; see the "Hardware-in-the-Loop" section of `README.md` for the harness.

**Noise.** The capture showed a *run* of bytes replaced by others in the
middle of a line - not single-bit errors, which is what a marginal wire would
give - so the corruption is more likely in the capture path (the CP210x bridge
or the analyzer's USB transfer) than on the bus. The UART capture in the
harness tore a line once as well. The harness now tolerates a mangled line
instead of failing the run, but the cause is not established, and the bench
wiring (a twisted pair and a common ground for the analyzer's reference) is
the first thing to try before chasing it in the code.
