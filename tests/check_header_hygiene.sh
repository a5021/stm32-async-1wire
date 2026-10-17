#!/bin/sh
# tests/check_header_hygiene.sh - the public headers must not leave the ow_bits
# register aliases (T1, PA, RC, D13, ...) defined in a consumer's translation
# unit.
#
# Why this exists: onewire.h pulls in the port engine, which is header-only and
# written in ~38 single-letter register shortcuts defined in inc/ow_bits.h. The
# engine's inline bodies must see those aliases while they are parsed, so
# ow_bits.h itself cannot #undef them - a TU that includes onewire.h would get
# its inline bodies half-parsed first. inc/ow_port.h, the last header in the
# chain that uses them, withdraws the aliases at its end. Nothing tied that
# block to the definitions, so this check is the tie.
#
# It is not hypothetical. Commit 8dba9d2 moved the backend #include from
# ow_port.h into onewire.h ("single family chain in onewire.h"); that made
# onewire.h transitively pull ow_bits.h, and the leak shipped in v2.1.0. Every
# header below should stay clean at every family.
#
# The check preprocesses a one-line TU per header/family with -dM and fails if
# any alias is defined. A marker macro proves the include actually took effect,
# so a broken include path cannot pass vacuously. Direct inclusion of ow_mcu.h
# is deliberately out of scope: it is an internal header, not a consumer entry
# (documented in the README "Layer Contracts").
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

# The aliases ow_bits.h defines (inc/ow_bits.h:8-56); kept in sync by hand with
# the #undef block at the end of inc/ow_port.h.
PATTERN='^#define (D11|D12|D13|D14|D15|D16|D17|D2|A1|A2|A|BK|C1|DB|EX|FL|PA|PB|PC|PD|I1|I2|IW|O|PW|RC|RT|S1|S2|T1|T2|T3|T4|U|U1|U2|U3|W) '

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
        if ! printf '%s\n' "$out" | grep -q "^#define $marker "; then
            echo "  $hdr ($fam): $marker not defined - the include did not take effect" >&2
            fail=1
            continue
        fi
        leaked=$(printf '%s\n' "$out" | grep -E "$PATTERN")
        if [ -n "$leaked" ]; then
            echo "  $hdr ($fam): leaked ow_bits aliases:" >&2
            printf '%s\n' "$leaked" | sed 's/^/    /' >&2
            fail=1
        fi
    done
}

check_header onewire.h ONEWIRE_ROM_BYTES
check_header ow_port.h OW_PORT_LONG_STAGE_US
check_header ds18b20.h DS18B20_ROM_BYTES
check_header ds2431.h DS2431_FAMILY_CODE

if [ "$fail" -ne 0 ]; then
    echo "  check_header_hygiene: FAIL" >&2
    exit 1
fi
echo "  check_header_hygiene: OK ($checks header x family combinations, no ow_bits aliases leaked)"
