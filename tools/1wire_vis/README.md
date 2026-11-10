# 1-Wire Bus Visualizer (browser prototype)

Open a sigrok `.sr` capture in the browser and see the 1-Wire traffic:
resets/presence, Search ROM triplets with ROM + CRC check, Match/Skip ROM,
Convert T, scratchpad and DS2431 EEPROM commands. Standard and overdrive
timings are supported. No upload — everything runs locally, no dependencies,
works from `file://`.

Open `index.html` in Chrome/Edge/Firefox/Safari, or use the hosted copy at
<https://a5021.github.io/stm32-async-1wire/viewer/> (its built-in selftest
runs at `/viewer/selftest.html`). Then either:

- **Samples → Synthetic demo (1 MHz)** — a capture generated in the page
  (no file, no hardware): single-device Search ROM, Skip ROM + Convert T,
  a 750 ms conversion wait, then Match ROM + Read Scratchpad;
- **Samples → Bundled 9_Multi capture** — the real bench recording shipped
  in `captures/` (also embedded in `bundled_sr.js`, so it works straight
  from `file://` with no server); or
- pick any `.sr` file recorded with `tools/la_capture.py`
  (fx2lafw, DQ on D0–D7, 1–8 MHz), choose the channel and inspect; or
- open a shared link with `?src=<capture>`, optionally with
  `&speed=standard|overdrive`, for example
  `index.html?src=../stress/captures/l5_8mhz_g1.sr&speed=standard`.

`?src=` is how the published stress report opens one frame per test. The path
is resolved against the viewer URL and must stay on the same origin (`file://`
pages may point at local files). Cross-origin, `javascript:` and `data:` values
are rejected, so a shared link can only name data the page is allowed to fetch.
`speed` pins the bus profile: auto-detection is right for real 1-Wire traffic,
but the stress pattern's 60 µs '0' pulses look like overdrive resets, so the
report pins `standard`.

`raw=1` opens the pulse viewer: the selected channel is drawn as a digital
waveform and no 1-Wire decoder runs, so PIO/marker channels show no protocol
tree or CRC marks. The page hides the 1-Wire controls, search/protocol/
transcript panels and footer, and changes its title to **Pulse viewer**. For
example, the 9_multi PIO pins are
`index.html?src=../examples/captures/9_multi.sr&chan=2&raw=1` (PIOA) and
`chan=3&raw=1` (PIOB).

**Bus speed** is next to the channel selector. `Auto` looks at the longest
reset low pulse: standard (≥300 µs) or overdrive (~48–80 µs); explicit
`Standard` / `Overdrive` is available when a capture is ambiguous or when
mixing both speeds (a single profile cannot decode both at once).

## Bundled 9_Multi capture (F030 boot excerpt)

`captures/9_multi_f030_boot.sr` (8 MHz, 32.8 M samples, 4.1 s, 65 KiB) —
STM32F030 + `9_multi`, parasite power, 7× DS18B20 + 1× DS2431. This is the
embedded sample behind *Samples → Bundled 9_Multi capture*; it predates the
DS2413 and therefore has no PIO channels. The published
`../examples/captures/9_multi.sr` is the later F446 run that adds the DS2413
(on channels 2/3 with `raw=1`). The F030 excerpt is the first boot after
reflashing with `PEAK_RESET=1`, which forces both the EEPROM read and the write
chains. It holds:

- T1–T8 and T9–T16: two Search ROM scans, 8 passes each; both enumerate all
  8 devices;
- T17–T20: DS2431 Read Memory of `0x10` / `0x18` / `0x20` / `0x28`;
- T21–T28: broadcast Convert T (`CC 44`) and the 7 DS18B20 scratchpad reads;
- T29–T44: four DS2431 write chains — ROM block `0x20`, peak record `0x10`,
  ROM block `0x28`, low record `0x18`. Each chain is
  `0F` Write Scratchpad → `AA` Read Scratchpad verify → `55` Copy →
  `F0` read-back; the read-back frames carry CRC-16 checks.

The excerpt starts with a 10 µs idle-high lead-in, then the first reset. The
full 30 s, 8 MHz, single-channel capture lives in `bench_logs/` (git-ignored).

`captures/9_multi_f030_boot.txt` is the same 44 transactions as text (same
shape as `tools/la_decode.py` output).

## What it shows

- **Overview** — whole-capture activity envelope with reset ticks (red = no
  presence, green = presence) and a light blue tint for the Detail window when
  zoomed. Click centres Detail on the clicked time at the current zoom; drag a
  range to zoom/pan Detail to that exact interval.
- **Detail** — adaptive relative-time ruler (zero at capture start, or at a
  click-set zero; µs for sub-second spans; hover reports absolute capture time
  and pulse width), and
  the nearest slot's bit/role/phase with its low and slot widths; color can
  encode bit value or bus ownership. Role mode distinguishes
  master→slave, slave→master, Search ROM slave ID/complement and the
  master's Search-ROM direction slot. Presence and reset have measured spans;
  RESET (red) and PRESENCE (green) are annotated separately on the byte row,
  shortening to `R`/`P` as the pulses narrow and disappearing at overview
  zoom. Byte boundaries get thin vertical dividers while the bytes remain wide
  enough to read. A plain click sets a relative-time zero (marked on the plot,
  ruler re-anchors to it); a second click clears it. The selected transaction
  line also reports the measured widths.
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
- **Theme** — the header has a light/dark toggle; the choice is
  remembered in `localStorage` when available.
- **Transactions/transcript** — click a transaction to inspect; copy the
  sigrok-style transcript. Family codes include `0x28` DS18B20, `0x2D` DS2431,
  and `0x3A` DS2413.

### Self-test

`selftest.html` is a browser page with 103 golden checks (no Node needed). Open
it directly from `file://`; it runs automatically and prints PASS/FAIL per
check plus a summary. `python tools/1wire_vis/run_selftest.py` (or
`make test-viewer`) runs the same page in headless Chromium and exits non-zero
with the failed checks in the log; CI runs it on every pull request, so a
decoder change cannot ship without the golden vectors passing. Install the
pinned dependency first:

```bash
python -m pip install -r tools/1wire_vis/requirements-selftest.txt
python -m playwright install chromium
```

It covers:

- bundle integrity (`.sr` size, sample count, FNV-1a of all 32.8 M samples);
- 44 transactions, 7384 slots, 8 unique ROMs, 7× DS18B20 + 1× DS2431;
- two 8-pass Search ROM scans, both enumerating the same 8 devices;
- CRC-8 and DS2431 inverted CRC-16 vectors, plus checks against real frames;
- DS2431 Copy Scratchpad roles (TA1/TA2/ES master→slave, `AA` slave→master);
- positive final-slot spans at 1 MHz and 2 MHz (median-pitch estimate);
- overdrive decode at 1 MHz and 2 MHz, auto-detection and channel D3;
- broadcast Skip ROM labels/roles for DS2431 and the DS18B20 Convert T;
- Alarm Search, DS2413 PIO read/write, truncated/empty/no-presence inputs;
- pure-JS inflate on fixed/dynamic/stored DEFLATE blocks, a multi-entry
  `.sr` whose waveform crosses the `logic-1-1` / `logic-1-2` boundary, and
  the full bundled 32.8 M-sample capture with `DecompressionStream` disabled;
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
- Role annotation follows the ROM/function-command state: master→slave
  command/address/data slots, slave→master reply slots, and the 3-slot Search
  ROM ID/complement/direction triplets. Broadcast Skip ROM commands are labelled
  with their family (the frame itself does not encode it), and unknown or
  truncated traffic stays marked unknown rather than being guessed.

`file://` note: everything is static. Clipboard access may be restricted on
`file://` in some browsers — if copy fails, select the transcript text manually.
For sharing, serve the directory (`python -m http.server`) or host it on
gh-pages.
