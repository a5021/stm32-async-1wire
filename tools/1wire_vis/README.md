# 1-Wire Bus Visualizer (browser prototype)

Open a sigrok `.sr` capture in the browser and see the 1-Wire traffic:
resets/presence, Search ROM triplets with ROM + CRC check, Match/Skip ROM,
Convert T, scratchpad and DS2431 EEPROM commands. Standard and overdrive
timings are supported. No upload — everything runs locally, no dependencies,
works from `file://`.

Open `index.html` in Chrome/Edge/Firefox/Safari, then either:

- **Load demo waveform** — a synthetic 1 MHz capture (Match ROM +
  Read Scratchpad, Skip ROM + Convert T, single-device Search ROM),
  so the page works with no hardware;
- **Load bundled F030 capture** — the real bench recording shipped in
  `captures/` (also embedded in `bundled_sr.js`, so it works straight
  from `file://` with no server); or
- pick any `.sr` file recorded with `tools/la_capture.py`
  (fx2lafw, DQ on D0–D7, 1–2 MHz), choose the channel and inspect.

**Bus speed** is next to the channel selector. `Auto` looks at the longest
reset low pulse: standard (≥300 µs) or overdrive (~48–80 µs); explicit
`Standard` / `Overdrive` is available when a capture is ambiguous or when
mixing both speeds (a single profile cannot decode both at once).

## Bundled capture

`captures/9_multi_f030_boot.sr` (1 MHz, 4.0 M samples, 11 KiB) —
STM32F030 + `9_multi`, parasite power, 7× DS18B20 + 1× DS2431.
Recorded from the first bus edge after an SWD reset, so it holds:

- T1–T7: Match ROM + Read Scratchpad reads of the 7 DS18B20 sensors;
- T8–T15: a peak write chain — ROM block `0x20` (scratchpad /
  verify `ES=0x07` / copy `0xAA` / read-back) then record block `0x10`
  (`#6 27.3 °C`, sensor 3); the read-back frames carry CRC-16 checks;
- T16–T23 and T24–T31: two consecutive Search ROM scans, 8 passes each;
  both scans enumerate all 8 devices (the DS2431 included);
- T32–T35: four DS2431 Read Memory transactions at `0x10` / `0x18` /
  `0x20` / `0x28` (record blocks);
- T36: broadcast (Skip ROM) Convert T (`CC 44`).

`captures/9_multi_f030_boot.txt` is the same 36 transactions as text
(same shape as `tools/la_decode.py` output); the UART reference for
this run reported `peak: #6 27.3 C, sensor 3`, matching T8/T12/T15.

## What it shows

- **Overview** — whole-capture envelope with reset ticks (red = reset,
  green = presence), selected transaction and the detail viewport
  frame. Drag a range to zoom/pan Detail to that exact capture interval.
- **Detail** — adaptive absolute-time ruler; hover reports level, measured
  pulse width, and the nearest slot's bit/role/phase with its low and slot
  widths; color can encode bit value or bus ownership. Role mode
  distinguishes master writes, slave reads, Search ROM responses and the
  master's Search-ROM direction slot. Presence and reset have measured spans;
  every visible reset gets a caption: full text when it fits, compact
  `RESET+`/`RESET-` tags at medium density, and red/green pins plus an
  aggregate count at overview density.
- **CRC verdict** — DS18B20 scratchpad CRC-8 and DS2431 Read Scratchpad
  inverted CRC-16 bytes are highlighted green/red; the expanded transaction
  tree reports the algorithm and verdict.
- **Timing cursors** — Alt-click A and Shift-click B (both snap to nearby
  edges); read the delta in microseconds. Esc clears both.
- **Protocol tree** — Search ROM passes are grouped into scans (a repeated ROM
  starts a new scan); repeated Match-ROM transactions to one device form a
  chain. Expand a transaction to see ROM command/address, function command,
  slot ownership, payload/reply and CRC status.
- **Search ROM results** — unique ROMs with family name and triplet/ROM CRC
  status; click one to jump to the pass that found it (its group expands and
  the row is scrolled into view).
- **Navigation** — ←/→ previous/next reset, `F` fit transaction, `0` fit
  capture, `+`/`-` zoom; mouse wheel zooms around the pointer and drag pans.
  On touch screens one finger pans and two fingers pinch-zoom around the
  midpoint.
- **Transactions/transcript** — click a transaction to inspect; copy the
  sigrok-style transcript. Family codes include `0x28` DS18B20, `0x2D` DS2431,
  and `0x3A` DS2413.

## Self-test

`selftest.html` is a browser page with 82 golden checks (no Node needed). Open
it directly from `file://`; it runs automatically and prints PASS/FAIL per
check plus a summary. It covers:

- bundle integrity (`.sr` size, sample count, FNV-1a of all 4.0 M samples);
- 36 transactions, 6152 slots, 8 unique ROMs, 7× DS18B20 + 1× DS2431;
- two 8-pass Search ROM scans, both enumerating the same 8 devices;
- CRC-8 and DS2431 inverted CRC-16 vectors, plus checks against real frames;
- DS2431 Copy Scratchpad roles (TA1/TA2/ES master-write, `AA` slave-read);
- positive final-slot spans at 1 MHz and 2 MHz (median-pitch estimate);
- overdrive decode at 1 MHz and 2 MHz, auto-detection and channel D3;
- broadcast Skip ROM labels/roles for DS2431 and the DS18B20 Convert T;
- Alarm Search, DS2413 PIO read/write, truncated/empty/no-presence inputs;
- pure-JS inflate on fixed/dynamic/stored DEFLATE blocks, a multi-entry
  `.sr` whose waveform crosses the `logic-1-1` / `logic-1-2` boundary, and
  the full bundled 4.0 M-sample capture with `DecompressionStream` disabled;
- raw-note truncation, all-FF Search ROM filler guard, and the demo waveform.

## Decoder

`owvis.js` holds the whole pipeline with no DOM in it:

- `.sr` parsing through the zip central directory (raw deflate via the
  native `DecompressionStream`, with a pure-JS inflate fallback for older
  browsers), samplerate/unitsize from `metadata`;
- slot splitter ported from the validated Python reference
  (bit-exact vs `sigrok onewire_link` on the bench `probe.sr`:
  falling edge starts a slot, low ≥ 300 µs is Reset, otherwise the bit
  is sampled 20 µs after the edge, 55 µs advance; presence = ≥ 30 µs
  low within 200 µs after release; all thresholds scale with samplerate);
- overdrive profile: reset ≥ 24 µs, bit sampled 3 µs after the edge,
  5 µs advance, 4 µs presence low within 40 µs. `Auto` chooses the profile
  from the longest low pulse;
- Search ROM triplet → ROM + Dallas CRC-8 check.
- One slot span per decoded slot: end = next slot start, capped at 500 µs so
  a host idle gap is never shaded as a slot. The final slot of a transaction
  uses the median start-to-start pitch of the preceding slots.
- Role annotation follows the ROM/function-command state: master command and
  address/data slots, slave reply slots, and the 3-slot Search ROM
  ID/complement/direction triplets. Broadcast Skip ROM commands are labelled
  with their family (the frame itself does not encode it), and unknown or
  truncated traffic stays marked unknown rather than being guessed.

`file://` note: everything is static. Clipboard access may be restricted on
`file://` in some browsers — if copy fails, select the transcript text manually.
For sharing, serve the directory (`python -m http.server`) or host it on
gh-pages.
