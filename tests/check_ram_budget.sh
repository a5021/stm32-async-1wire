#!/bin/sh
# tests/check_ram_budget.sh - gate the library .bss budget of the firmware
#
# The phase-union work bought back ~200-430 bytes of .bss (F0 1_basic 676->460,
# F4 1004->572). Without a gate the next innocent commit eats it back silently:
# nothing else watches RAM. This script is that gate.
#
# Two tiers, both exact ceilings (no slack — any growth must be conscious):
#   1. library-symbol sum: nm sizes of the driver-owned .bss statics in the
#      5_commands image (the only example linking every context at once:
#      measure + search + transactions + resolution; 1_basic has part of them
#      garbage-collected by LTO and would not notice their growth).
#   2. total .bss of the image: backstop against new statics nobody added to
#      the allowlist below, and against libc/startup drift (toolchain is
#      pinned, so the number is deterministic).
# Plus the 1_basic total, the headline number from the bench reports.
#
# Budgets (measured 2026-10-07, toolchain 15.2, defaults, no EXT flags —
# parasite/power flags only retune pulse values, not sizes).
# History: f0 352/1012/460 and f4 465/1124/572 before the flash search-command
# tables (search_ctx 44->32 on F0, 52->32 on F4); f0 340/1000/448 and f4
# 445/1104/552 before the flag-byte packing (txn 32->24, ctx 86->84, res
# 5->4; search counters give 0: sink-pointer alignment absorbs them).
#   family  libsum(5_commands)  total(5_commands)  total(1_basic)
#   f0             329                 984              432
#   f4             434                1088              536
#   g4             329                 984              432
#   h5             329                 984              432
# (g4 measured 2026-10-08: same uint8_t pulse tables as f0, so the library
# lands byte-identical; only the part Statics differ, none driver-owned.)
# (h5 measured 2026-10-10: same uint8_t pulse tables again - GPDMA converts
# widths in hardware, so no F4-style halfword tables - hence the same 329.)
#
# Recalibration procedure (ceilings are exact by design — no slack, so any
# growth must be conscious; a red gate is a question, not a failure):
#   - Intentional RAM change: update the numbers above and the matching
#     _Static_assert sizes in src/ (ctx in ds18b20.c, txn_ctx beside it,
#     res_ctx in ds18b20_resolution.c, search_ctx in onewire.c), and extend
#     the History note with what moved and why.
#   - Toolchain upgrade (pinned at 15.2.rel1 in ci.yml/build.yml/release.yml):
#     run `make test-ram` locally with the new toolchain, record all three
#     tiers per family, and update budgets + asserts + this note together —
#     layout and padding shift between compiler versions, so a red gate right
#     after an upgrade is expected recalibration, not a regression.
#
# Matching goes through fixed strings on purpose (see check_version.sh: this
# repo checks out CRLF on Windows and LF in CI).
#
# Usage: sh tests/check_ram_budget.sh        (make test-ram is the usual entry)
# Env: GCC_PATH — same as make (directory holding arm-none-eabi-*); empty
# means the tools are on PATH (CI puts them there).

set -u

cd "$(dirname "$0")/.." || exit 1

if [ -n "${GCC_PATH:-}" ]; then
    TC="$GCC_PATH/"
else
    TC=""
fi
NM="${TC}arm-none-eabi-nm"
SZ="${TC}arm-none-eabi-size"

fail=0

note() { printf '  %s\n' "$1"; }
bad() { printf '  FAIL: %s\n' "$1"; fail=1; }

# Driver-owned .bss statics. Add new driver state here when it is introduced —
# tier 2 (total .bss) catches what this list misses, but only tier 1 tells
# which symbol grew. Deliberately NOT here: app statics (uart_tx_*, rom,
# scratchpad, *_ms, search_running, ...), libc/startup (__sf, errno, heap,
# object.0 from crtbegin.o, ...).
LIBSYMS="phase_pulses ctx dev_roms dev_count detect_buf search_ctx search_user_sink txn_ctx res_ctx search_pulse3 search_pair_pulse"

# Sum of nm sizes of the allowlisted symbols in $1 (an .elf path); prints bytes.
libsum() {
    tmp="build/.ramsum.tmp"
    # tr -d '\r': binutils on Windows emit CRLF, and a stray CR inside $name
    # would silently break the allowlist match below (same trap as in
    # check_version.sh).
    "$NM" --print-size "$1" | tr -d '\r' > "$tmp"
    sum=0
    while read -r addr size type name rest; do
        case " $LIBSYMS " in
            *" $name "*)
                case "$type" in
                    b|B) sum=$((sum + 0x$size)) ;;
                esac
                ;;
        esac
    done < "$tmp"
    rm -f "$tmp"
    printf '%s' "$sum"
}

# Total .bss of $1 (an .elf path); prints bytes.
totalbss() {
    "$SZ" -B "$1" | awk 'NR==2 {print $3}'
}

check() {
    target="$1"
    libmax="$2"
    total5max="$3"
    total1max="$4"
    note "OW_TARGET=$target"
    make -s OW_TARGET="$target" APP=1_basic > /dev/null
    make -s OW_TARGET="$target" APP=5_commands > /dev/null
    lib=$(libsum build/ds18b20_5_commands.elf)
    tot5=$(totalbss build/ds18b20_5_commands.elf)
    tot1=$(totalbss build/ds18b20_1_basic.elf)
    note "libsum(5_commands)=$lib (budget $libmax)"
    note "total(5_commands)=$tot5 (budget $total5max)"
    note "total(1_basic)=$tot1 (budget $total1max)"
    if [ "$lib" -gt "$libmax" ]; then
        bad "$target library .bss $lib exceeds budget $libmax — update LIBSYMS/budgets consciously or shrink RAM"
    fi
    if [ "$tot5" -gt "$total5max" ]; then
        bad "$target 5_commands total .bss $tot5 exceeds budget $total5max"
    fi
    if [ "$tot1" -gt "$total1max" ]; then
        bad "$target 1_basic total .bss $tot1 exceeds budget $total1max"
    fi
}

for t in "$NM" "$SZ"; do
    if ! command -v "$t" > /dev/null 2>&1; then
        bad "tool not found: $t (pass GCC_PATH=<dir> as with make)"
    fi
done
if [ "$fail" -ne 0 ]; then
    exit 1
fi

check f0 329 984 432
check f4 434 1088 536
check g4 329 984 432
check h5 329 984 432

if [ "$fail" -ne 0 ]; then
    exit 1
fi
note "RAM budget OK"
