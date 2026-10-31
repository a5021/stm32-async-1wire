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
# A third tier pins the heavyweight F030 examples whole-image: F030 is the
# binding part (16KB flash / 4KB RAM) and these three are its largest images,
# so they are the first to break on innocent growth. Both text (flash proxy;
# .data is 8-12 B on every image here) and total .bss are exact ceilings:
#   app            text    bss
#   6_statistics   6264    1784
#   8_ds2431       6156    768
#   9_multi        11756   1296
#
# Budgets (measured 2026-10-07, toolchain 15.2, defaults, no EXT flags —
# parasite/power flags only retune pulse values, not sizes).
# History: f0 352/1012/460 and f4 465/1124/572 before the flash search-command
# tables (search_ctx 44->32 on F0, 52->32 on F4); f0 340/1000/448 and f4
# 445/1104/552 before the flag-byte packing (txn 32->24, ctx 86->84, res
# 5->4; search counters give 0: sink-pointer alignment absorbs them).
#   family  libsum(5_commands)  total(5_commands)  total(1_basic)
#   f0             329                 652              432
#   f4             434                 756              536
#   g4             329                 652              432
#   h5             329                 652              432
# (g4 measured 2026-10-08: same uint8_t pulse tables as f0, so the library
# lands byte-identical; only the part Statics differ, none driver-owned.)
# (h5 measured 2026-10-10: same uint8_t pulse tables again - GPDMA converts
# widths in hardware, so no F4-style halfword tables - hence the same 329.)
#
# 2026-10-14: ONEWIRE_PARAM_CHECK replaced assert(0) in the onewire.c reject
# paths (fail-soft everywhere, trap only under the test harness). The 5_commands
# image used to link __assert_func -> fprintf -> __sf (332 B .bss on every
# family), so total(5_commands) drops 984->652 (f0/g4/h5) and 1088->756 (f4);
# libsum and total(1_basic) do not move (that image never kept the trap).
# H5 1_basic text drops 76 B for the same reason.
#
# 2026-10-11: src/ds2431.c (the DS2431 EEPROM driver) joined the firmware links
# (Makefile SRC / CMake core library). Neither tier image (1_basic,
# 5_commands) calls it, so --gc-sections drops its context and none of the
# three measured numbers moves; the driver is present in the image but
# byte-inert. examples/8_ds2431 does call it, but this script never builds
# that example. The DS2431 context static is also named `ctx` (file-local, so
# no link clash with the DS18B20 one): if a tier image ever pulls the DS2431
# driver in, the `ctx` allowlist entry sums both, which is the intended
# behaviour but worth knowing when reading a grown number.
#
# 2026-10-31: third tier for the heavyweight F030 examples (see above).
# 6_statistics owns the largest .bss (1024 B UART ring + 300 B stats state),
# 9_multi the largest text (multi-device + DS2431 in one image, ~72% of the
# 16KB flash). 8_ds2431 is the only tier image that actually links the
# DS2431 engine (see 2026-10-11), so its text moves with that driver. 8_ds2431
# text also falls when the engine shrinks (6156 -> 6152 with the shared
# prefix/byte encoders); only growth needs a conscious bump, shrinkage
# just passes.
#
# 2026-10-31 (same day): ROM-prefix encoder factored out of the five command
# builds (DS18B20 addr/txn/resolution/skip plus DS2431) into one always-inline
# ow_encode_prefix(), and the two payload loops into ow_encode_bytes();
# 0xCC literals and per-driver 0x55 spellings now resolve to
# ONEWIRE_SKIP_ROM/ONEWIRE_MATCH_ROM. Both helpers take a cursor they advance
# themselves, so no call site multiplies by the entry width; F030 images end
# byte-identical to baseline except 8_ds2431 text 6156 -> 6152.
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

# .text of $1 (an .elf path); prints bytes. Flash proxy for the tier-3
# images (.data is a constant 8-12 B there and lives in flash too, but
# gating text alone keeps the ceiling readable).
textsize() {
    "$SZ" -B "$1" | awk 'NR==2 {print $1}'
}

check() {
    mcu="$1"
    libmax="$2"
    total5max="$3"
    total1max="$4"
    note "OW_MCU=$mcu"
    make -s OW_MCU="$mcu" APP=1_basic > /dev/null
    make -s OW_MCU="$mcu" APP=5_commands > /dev/null
    lib=$(libsum build/ds18b20_5_commands.elf)
    tot5=$(totalbss build/ds18b20_5_commands.elf)
    tot1=$(totalbss build/ds18b20_1_basic.elf)
    note "libsum(5_commands)=$lib (budget $libmax)"
    note "total(5_commands)=$tot5 (budget $total5max)"
    note "total(1_basic)=$tot1 (budget $total1max)"
    if [ "$lib" -gt "$libmax" ]; then
        bad "$mcu library .bss $lib exceeds budget $libmax — update LIBSYMS/budgets consciously or shrink RAM"
    fi
    if [ "$tot5" -gt "$total5max" ]; then
        bad "$mcu 5_commands total .bss $tot5 exceeds budget $total5max"
    fi
    if [ "$tot1" -gt "$total1max" ]; then
        bad "$mcu 1_basic total .bss $tot1 exceeds budget $total1max"
    fi
}

# Whole-image ceiling for one heavyweight example on the binding part
# (F030: 16KB flash / 4KB RAM). Unlike check() above this gates the app
# image itself, not the library inside it: app growth and library growth
# both count, which is what makes it a backstop for the examples that
# would break first.
check_app() {
    mcu="$1"
    app="$2"
    textmax="$3"
    bssmax="$4"
    note "OW_MCU=$mcu APP=$app"
    make -s OW_MCU="$mcu" APP="$app" > /dev/null
    text=$(textsize build/ds18b20_"$app".elf)
    tot=$(totalbss build/ds18b20_"$app".elf)
    note "text($app)=$text (budget $textmax)"
    note "total($app)=$tot (budget $bssmax)"
    if [ "$text" -gt "$textmax" ]; then
        bad "$mcu $app text $text exceeds budget $textmax"
    fi
    if [ "$tot" -gt "$bssmax" ]; then
        bad "$mcu $app total .bss $tot exceeds budget $bssmax"
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

check STM32F030x6 329 652 432
check STM32F407xx 434 756 536
check STM32G474xx 329 652 432
check STM32H503xx 329 652 432

check_app STM32F030x6 6_statistics 6264 1784
check_app STM32F030x6 8_ds2431 6156 768
check_app STM32F030x6 9_multi 11752 1296

if [ "$fail" -ne 0 ]; then
    exit 1
fi
note "RAM budget OK"
