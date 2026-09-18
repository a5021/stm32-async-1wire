# STM32F4 (STM32F401) backend — hardware notes

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
