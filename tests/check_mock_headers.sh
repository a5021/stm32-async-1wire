#!/bin/sh
# tests/check_mock_headers.sh - verify the host mocks' register macros against
# the real CMSIS headers.
#
# The host suite reasons about peripherals through the stand-ins in
# tests/mock/stm32f<family>xx.h. If a stand-in's macro value disagrees with the
# silicon header, a test asserting "the bus pin is in alternate-function mode"
# passes no matter what the driver does - it is checking the wrong bits. That is
# not hypothetical: the MODER bit field for pin 10 in the f0, g0 and f4 mocks
# held pin 11's bits, so on three of the four backends the bus-pin setup was
# unverifiable on the host and the whole suite stayed green throughout.
#
# The comparison is by name, which survives the family differences: the F0/F4
# CMSIS headers spell it GPIO_MODER_MODER10 and the G0 header spells it
# GPIO_MODER_MODE10, and each mock uses its own header's spelling.
#
# CMSIS carries a resolved value in a trailing /*!< 0x... */ comment whether it
# wrote the number directly (0x00000400U) or as an expression ((0x2UL << POS)).
# That comment is the authority; both sides are normalised to a bare hex number
# first, so 0x10 and 0x0010 compare equal. Names the real header defines only as
# an alias to a _Msk or _Pos macro carry no such comment; those are counted and
# reported so the check cannot quietly become vacuous.
#
# Run: make check-mocks

set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
MOCK_DIR="$REPO/tests/mock"
CMSIS_DIR="$REPO/CMSIS/device"
fail=0

if [ ! -d "$CMSIS_DIR" ]; then
    echo "  CMSIS/device not present - run 'make download-deps' first" >&2
    exit 1
fi

REAL_TBL=$(mktemp)
MOCK_TBL=$(mktemp)
trap 'rm -f "$REAL_TBL" "$MOCK_TBL"' EXIT

# normalise every "name<TAB>0x...." row to a bare hex number
normalise() {
    awk '{
        v = $2
        gsub(/\r/, "", v)
        gsub(/[uUlL]/, "", v)
        v = tolower(v)
        sub(/^0x/, "", v)
        sub(/^0+/, "", v)
        if (v == "") v = "0"
        print tolower($1) "\t" v
    }'
}

compare() {
    fam=$1; mock=$2; real=$3
    if [ ! -f "$mock" ]; then echo "  $fam: missing $mock" >&2; fail=1; return; fi
    if [ ! -f "$real" ]; then echo "  $fam: missing $real" >&2; fail=1; return; fi

    # real header: name -> the value from its /*!< 0x... */ comment
    sed -n 's/^[[:blank:]]*#[[:blank:]]*define[[:blank:]]*\([A-Za-z_][A-Za-z0-9_]*\)[[:blank:]].*\/\*!<[[:blank:]]*\(0[xX][0-9A-Fa-f][0-9A-Fa-f]*\).*\r*$/\1 \2/p' \
        "$real" | normalise | sort -u > "$REAL_TBL"

    # mock header: name -> its literal value
    sed -n 's/^[[:blank:]]*#[[:blank:]]*define[[:blank:]]*\([A-Za-z_][A-Za-z0-9_]*\)[[:blank:]]*\(0[xX][0-9A-Fa-f][0-9A-Fa-f]*\)[uUlL]*[[:blank:]]*\r*$/\1 \2/p' \
        "$mock" | normalise | sort -u > "$MOCK_TBL"

    total=$(wc -l < "$MOCK_TBL" | tr -d ' ')
    compared=0
    skipped=0
    bad=""

    while read -r name mval; do
        [ -n "$name" ] || continue
        rval=$(awk -v n="$name" -F'\t' '$1 == n { print $2; exit }' "$REAL_TBL")
        if [ -z "$rval" ]; then
            skipped=$((skipped + 1))
            continue
        fi
        compared=$((compared + 1))
        if [ "$mval" != "$rval" ]; then
            bad="$bad $name(mock=0x$mval real=0x$rval)"
        fi
    done < "$MOCK_TBL"

    if [ "$compared" -eq 0 ]; then
        echo "  $fam: nothing compared - the check would be vacuous" >&2
        fail=1
        return
    fi
    if [ -n "$bad" ]; then
        echo "  $fam: MOCK/CMSIS MISMATCH:$bad" >&2
        fail=1
    else
        echo "  $fam: ok ($compared compared, $skipped of $total mock macros have no comparable CMSIS value)"
    fi
}

compare f1 "$MOCK_DIR/stm32f1xx.h" "$CMSIS_DIR/stm32f103xb.h"
compare f0 "$MOCK_DIR/stm32f0xx.h" "$CMSIS_DIR/stm32f030x6.h"
compare g0 "$MOCK_DIR/stm32g0xx.h" "$CMSIS_DIR/stm32g031xx.h"
compare f4 "$MOCK_DIR/stm32f4xx.h" "$CMSIS_DIR/stm32f407xx.h"

if [ "$fail" -ne 0 ]; then
    echo "  check_mock_headers: FAIL" >&2
    exit 1
fi
echo "  check_mock_headers: OK"
