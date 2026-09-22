# STM32F4 (STM32F407) backend — hardware notes

Bring-up notes for `ow_port_f4.h`: the peripheral topology that works, and the
alternatives that were tested on real hardware and rejected. The operating
invariants live in the code comments and in the "Required Timer Capabilities"
section of `README.md`; this file keeps the experiments.

## DMA direct mode and the 16-bit feed

The F4 DMA streams are used in **direct mode** (FIFO disabled). In direct mode
the memory transfer width is forced to equal the peripheral width (`PSIZE`),
and packing/unpacking between different source/destination widths is available
only in FIFO mode (AN4031 §1.1.9). `CCR3` is a 16-bit (halfword) register, so:

- the feed source must be a **halfword** buffer — the family-typed `ow_pulse_t`
  (`uint16_t` on F4) feeds `CCR3` zero-copy from the shared command buffer
  (`&ow_cmd_buf[1]`: the first slot is latched directly) with `MSIZE_0` set;
- the capture path moves matching 16-bit halfwords (`MSIZE_0` = `PSIZE_0`).

### Rejected: 8-bit feed (`MSIZE=8`) under a halfword PSIZE

Tried feeding the byte command buffer directly with `MSIZE=8`. The stream
silently reads the bytes as halfwords, so consecutive slot durations are
recombined: for a `0xF0` Search ROM the durations `60, 60, 60, 60, 5, 5, 5, 5`
come out as `60|60<<8 = 15420`, `60|5<<8`, `5|5<<8`, … Every recombined value
is far larger than the slot `ARR`, so the line is held LOW across several
slots.

On the logic analyzer (8 MHz) the `0xF0` command's trailing bits merged into a
single ~213 µs LOW (3 × 71 µs slots). Search then returned all-zero ROMs with
CRC failures. With the halfword feed the same command reads back clean
(`60, 60, 60, 60, 5, 5, 5, 5`).

### Rejected: equal-width byte feed (`PSIZE=MSIZE=8`) into CCR3

Storing 8-bit DMA writes straight into the 16-bit `CCR3` was also tested and
rejected: the timer does not latch them, and search again returns all-zero ROMs
with CRC failures. The source must therefore be widened to halfwords, which is
why the feed is zero-copy from the internal `ow_pulse_t` command buffer with no
staging copy.

## Forced-update (`URS`) race

`ow_port_update_event()` forces a timer update (`EGR=UG`) to reload the
`ARR`/`RCR`/`CCR` preloads and give the freshly scheduled operation a clean
completion flag. On the F4 the UG-raised `UIF` appears a few timer cycles
*after* the `EGR` write, so clearing `SR` immediately can race the set and drop
the clear. The stale `UIF` then makes the next `ow_port_bus_done()` report the
operation complete before it has started — seen on hardware as a conversion
wait that returns immediately and every sensor reporting its 85.0 °C
power-on-reset value.

The fix gates the forced update with `URS` (so it raises no `UIF` at all),
clears any leftover flag while the clear cannot race, then restores `URS`. The
real completion (counter overflow) still sets `UIF`.

## CHSEL is a per-stream mux index

DMA2 streams select their request source with their own `CHSEL` field
(RM0368 §9.3.3, Table 29). Both the feed and the capture stream use `CHSEL=6`
(`DMA2_Stream2` → `TIM1_CH2`, `DMA2_Stream4` → `TIM1_CH4`); this is not a
shared physical request line, and neither stream consumes the other's request.

## Capture-chain latency scales with the timer kernel clock

Raw pulse dumps (`-DOW_DEBUG_PULSE_DUMP=1`, one-shot on the first scratchpad
read of `6_statistics`, 7 sensors, parasite power) across `SYSCLK_MHZ` builds.
The values are CCR4 input-capture readings on a 1 µs tick; both slot levels
shift together:

| SYSCLK | IC4F filter | CCR4 `'1'` | CCR4 `'0'` | Δ vs 168 | LA physical `'1'` | LA physical read-`'0'` |
|---|---|---|---|---|---|---|
| 168 MHz (HSE+PLL) | fDTS/8, N=6 | 5 | 28 | 0 | 5.0–6.2 µs | 29.4–30.6 µs |
| 16 MHz (raw HSI) | fCK, N=8 (default) | 7 | 30 | +2 | 5.0–6.2 µs | 29.4–30.6 µs |
| 16 MHz | fCK, N=2 (`IC4F_0`) | 7 | 30 | +2 | — | — |
| 16 MHz | fCK, N=4 (`IC4F_1`) | 7 | 30 | +2 | — | — |
| 8 MHz (raw HSE) | fCK, N=8 | 9 | 32 | +4 | 5.0–6.2 µs | 29.4–30.6 µs |

Logic-analyzer column: fx2lafw @ 16 MS/s on PA10 (D0), 0.26 s windows merged
per clock, low-width histogram (write-`0` lows measure 60.0 µs and resets
≈ 481–485 µs at every clock, as they must).

- The master low is hardware-timed in µs ticks (CCR3 compare), so the physical
  waveform is identical across builds — the offset lives in the capture path
  (edge → latched CCR4), not in the emitted pulse. The LA shows no clock
  dependence while CCR4 moves 5 → 7 → 9.
- The offset is independent of the input filter depth (N=2 and N=8 measure
  identically): the IC4F configuration is not the cause.
- The offset tracks the timer **kernel** clock (before the prescaler): 32 timer
  cycles → 32/168 ≈ 0 µs, 32/16 = 2 µs, 32/8 = 4 µs. Same law as the F030@8
  datapoint in the CHANGELOG ("a 5 µs pulse measures ~9 µs").
- The slow slave release (read-`'0'`) explains why CCR4 `'0'` reads 28 at
  168 MHz while the analyzer (higher logic threshold, later crossing on the RC
  front) shows ≈ 30: the capture latches earlier on the slow edge than the
  analyzer's threshold, then the same +0/+2/+4 latency applies on top
  (29.6 − 1.6 + {0,2,4} = {28, 30, 32}).
- Decode margin: `ONEWIRE_SHORT_PULSE_MAX = 10` still clears `'1' = 9` at
  8 MHz, but with only 1 µs to spare — anything slower needs a re-check of the
  decode window.

## Board connector hazards (STM32F4DISCOVERY MB997C)

The F4DISCOVERY's USB OTG FS mini-AB connector shares pads with the driver's
pins — keep it unplugged while the driver owns the bus (same class of hazard
as the G031's USB-C on PA11/PA12):

| Pad | Driver use | OTG FS use | Failure mode with a cable plugged |
|---|---|---|---|
| PA10 | 1-Wire bus (TIM1_CH3) | OTG_FS_ID | An A-cable grounds ID → holds the bus LOW; every reset/presence fails |
| PA11 | LA marker (GPIO) | OTG_FS_D− | Probe/cable contention on the marker; USB traffic would corrupt it |
| PA12 | — (free) | OTG_FS_D+ | Only matters if USB is initialised or PA12 is repurposed |

Never enable the USB FS peripheral while the driver runs: its pin
initialisation would reconfigure PA10/PA11 away from TIM1_CH3 / the marker.
