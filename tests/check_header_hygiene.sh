#!/bin/sh
# tests/check_header_hygiene.sh - the public headers must not leave the port's
# register vocabulary (the T1/PA/RC/D13 single-letter aliases, the OW_BITFIELDS()
# mask machinery and the register shortcuts) defined in a consumer's translation
# unit.
#
# Why this exists: the TIM1+DMA engine is header-only and written in that
# vocabulary, defined at the top of src/ow_port.h. Every macro body and inline
# body must see it while it is parsed, so nothing earlier in the chain can
# withdraw it, and src/ow_port.h - the last header in the chain that uses it -
# withdraws the whole vocabulary at its end. Nothing tied that block to the
# definitions, so this check is the tie.
#
# The engine is library-internal: src/ow_port.h is reached only by
# src/onewire.c, and the public headers - onewire.h, ow_board.h, ds18b20.h,
# ds2431.h - must never carry a single one of these names. The engine headers
# are checked too: ow_port.h's own TU must show the vocabulary fully withdrawn
# by its end, and ow_mcu.h's family hooks only expand inside ow_port.h, so its
# TU carries none either.
#
# It is not hypothetical. Commit 8dba9d2 moved the backend #include from
# ow_port.h into onewire.h ("single family chain in onewire.h"); that pulled
# the vocabulary into every consumer TU, and the leak shipped in v2.1.0. Every
# header below should stay clean at every family.
#
# The check preprocesses a one-line TU per header/family with -dM and fails if
# any vocabulary name is defined. A marker macro proves the include actually
# took effect, so a broken include path cannot pass vacuously. All six headers
# are covered - the four public ones, plus the two engine headers under src/
# (reached with -Isrc).
#
# Run: make test-header-hygiene

set -u

# Run from the repo root with relative include paths: the host compiler is the
# native one (MinGW/MSVC on Windows), which does not understand the /cygdrive
# paths a Cygwin/Git-Bash `pwd` would hand it.
cd "$(dirname "$0")/.." || exit 1

CC=${CC:-gcc}
INC=inc
SRC=src
MOCK=tests/mock

# The port's register vocabulary (src/ow_port.h: the vocabulary block at the
# top); kept in sync by hand with the withdraw block at the end of the same
# file. Longest alternation first for the overlapping machinery names, and the
# terminator class must be '[ (]': gcc -dM prints a function-like macro as
# "#define F(" with no space after the name, an object-like one as "#define A "
# with one.
PATTERN='^#define (RCC_BITS|RCC_AHBENR|RCC_APB2ENR|GPIO_CRH|TIM_CR1|TIM_DIER|TIM_BDTR|TIM_SR|TIM_EGR|TIM_CCMR2|TIM_CCER|DMA_CCR|DMA_LIFCR|DMA_HIFCR|DMA_SxCR|OW_BITFIELDS|OW_BF_CHOOSER2|OW_BF_CHOOSER1|OW_BF_CHOOSER|OW_BF_NARG_|OW_BF_NARG|OW_BF_ONE|OW_BF_8|OW_BF_7|OW_BF_6|OW_BF_5|OW_BF_4|OW_BF_3|OW_BF_2|OW_BF_1|D13|D14|D2|PA|RC|T1)[ (]'

FAMS="STM32F0 STM32F1 STM32F3 STM32F4 STM32G0 STM32G4 STM32H5"
fail=0
checks=0

# check_header <header> <marker macro the header is known to define>
check_header() {
    hdr=$1
    marker=$2
    for fam in $FAMS; do
        checks=$((checks + 1))
        out=$(printf '#include "%s"\n' "$hdr" |
            "$CC" -E -dM -x c - -I"$INC" -I"$SRC" -I"$MOCK" -D"$fam" 2>/dev/null) || {
            echo "  $hdr ($fam): preprocess failed - is $CC usable?" >&2
            fail=1
            continue
        }
        # ([ (]|$) accepts a bare "#define MARKER" as well as a valued one and
        # still rejects markers the line merely starts with.
        if ! printf '%s\n' "$out" | grep -qE "^#define ${marker}([ (]|$)"; then
            echo "  $hdr ($fam): $marker not defined - the include did not take effect" >&2
            fail=1
            continue
        fi
        leaked=$(printf '%s\n' "$out" | grep -E "$PATTERN")
        if [ -n "$leaked" ]; then
            echo "  $hdr ($fam): leaked the port's register vocabulary:" >&2
            printf '%s\n' "$leaked" | sed 's/^/    /' >&2
            fail=1
        fi
    done
}

check_header onewire.h ONEWIRE_ROM_BYTES
check_header ow_board.h OW_BOARD_H
check_header ow_port.h OW_PORT_LONG_STAGE_US
check_header ow_mcu.h OW_MCU_H
check_header ow_glue.h OW_GLUE_H
check_header ds18b20.h DS18B20_ROM_BYTES
check_header ds2431.h DS2431_FAMILY_CODE

# ---------------------------------------------------------------------------
# The public surface must speak in OW_*, never in the engine's vocabulary.
# ---------------------------------------------------------------------------
# The public headers are OW_FAMILY_*, OW_SYSCLK_MHZ, OW_LOW_POWER,
# OW_*_MAX_SYSCLK_MHZ - the board facts a consumer legitimately needs. The
# engine's own vocabulary (OW_PORT_*, ow_port_*) names its registers, DMA
# channels and register shortcuts, and it is private: the engine headers are
# not installed, so a consumer cannot even open what such a name would point
# at. A public header that mentions it invites a dependency that breaks the
# moment the engine is refactored.
#
# This is the tie for the 3.0.0 rename: the public macros were renamed in one
# sweep, and this stops a new OW_PORT_* from creeping back into inc/.
check_public_names() {
    for hdr in "$INC"/*.h; do
        hits=$(grep -nE 'OW_PORT_[A-Z0-9_]*|ow_port_[a-z0-9_]*' "$hdr" 2>/dev/null)
        if [ -n "$hits" ]; then
            echo "  $hdr: names the private port vocabulary:" >&2
            printf '%s\n' "$hits" | sed 's/^/    /' >&2
            fail=1
        fi
        pulled=$(grep -nE '#[[:space:]]*include[[:space:]]+"(ow_port|ow_mcu|ow_compiler|ow_glue)\.h"' \
                 "$hdr" 2>/dev/null)
        if [ -n "$pulled" ]; then
            echo "  $hdr: includes a private header:" >&2
            printf '%s\n' "$pulled" | sed 's/^/    /' >&2
            fail=1
        fi
    done
}

check_public_names

if [ "$fail" -ne 0 ]; then
    echo "  check_header_hygiene: FAIL" >&2
    exit 1
fi
echo "  check_header_hygiene: OK ($checks header x family combinations, no register vocabulary leaked; the public headers name no private symbol)"
