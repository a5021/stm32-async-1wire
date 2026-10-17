#!/bin/sh
# tests/check_header_hygiene.sh - the public headers must not leave the port's
# register vocabulary (the T1/PA/RC/D13 single-letter aliases, the BITS() mask
# machinery and the register shortcuts) defined in a consumer's translation
# unit.
#
# Why this exists: onewire.h pulls in the port engine, which is header-only and
# written in that vocabulary. The vocabulary is defined at the top of
# inc/ow_port.h: every macro body and inline body must see it while it is
# parsed, so nothing earlier in the chain can withdraw it, and inc/ow_port.h -
# the last header in the chain that uses it - withdraws the whole vocabulary at
# its end. Nothing tied that block to the definitions, so this check is the tie.
#
# It is not hypothetical. Commit 8dba9d2 moved the backend #include from
# ow_port.h into onewire.h ("single family chain in onewire.h"); that pulled
# the vocabulary into every consumer TU, and the leak shipped in v2.1.0. Every
# header below should stay clean at every family.
#
# The check preprocesses a one-line TU per header/family with -dM and fails if
# any vocabulary name is defined. A marker macro proves the include actually
# took effect, so a broken include path cannot pass vacuously. All five public
# headers are covered, including ow_mcu.h: its family hooks only expand inside
# ow_port.h, so its own TU carries none of the vocabulary either (documented in
# the README "Layer Contracts").
#
# Run: make test-header-hygiene

set -u

# Run from the repo root with relative include paths: the host compiler is the
# native one (MinGW/MSVC on Windows), which does not understand the /cygdrive
# paths a Cygwin/Git-Bash `pwd` would hand it.
cd "$(dirname "$0")/.." || exit 1

CC=${CC:-gcc}
INC=inc
MOCK=tests/mock

# The port's register vocabulary (inc/ow_port.h: the vocabulary block at the
# top); kept in sync by hand with the withdraw block at the end of the same
# file. Longest alternation first for the overlapping machinery names, and the
# terminator class must be '[ (]': gcc -dM prints a function-like macro as
# "#define F(" with no space after the name, an object-like one as "#define A "
# with one.
PATTERN='^#define (RCC_BITS|RCC_AHBENR|RCC_APB2ENR|GPIO_CRH|TIM_CR1|TIM_DIER|TIM_BDTR|TIM_SR|TIM_EGR|TIM_CCMR2|TIM_CCER|DMA_CCR|DMA_LIFCR|DMA_HIFCR|DMA_SxCR|_BITS_CHOOSER2|_BITS_CHOOSER1|_BITS_CHOOSER|_BITS_NARG_|_BITS_NARG|_BITS_ONE|_BITS_8|_BITS_7|_BITS_6|_BITS_5|_BITS_4|_BITS_3|_BITS_2|_BITS_1|BITS|D13|D14|D2|PA|RC|T1)[ (]'

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
            "$CC" -E -dM -x c - -I"$INC" -I"$MOCK" -D"$fam" 2>/dev/null) || {
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
check_header ow_port.h OW_PORT_LONG_STAGE_US
check_header ow_mcu.h OW_MCU_H
check_header ds18b20.h DS18B20_ROM_BYTES
check_header ds2431.h DS2431_FAMILY_CODE

if [ "$fail" -ne 0 ]; then
    echo "  check_header_hygiene: FAIL" >&2
    exit 1
fi
echo "  check_header_hygiene: OK ($checks header x family combinations, no register vocabulary leaked)"
