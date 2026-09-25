#!/bin/sh
# tests/check_chips.sh - verify the per-part build matrix in chips/*.mk
#
# Each chips/<part>.mk names three files that live in this repository (the
# linker script, the J-Flash project and the SEGGER Ozone project), a CMSIS
# device macro, an SVD and a default clock. A typo in any of them otherwise
# surfaces only at link time - or, worse, as a perfectly valid binary for the
# wrong part, because an unrecognised token used to fall through to the F1
# default.
#
# This lives in a script rather than in the Makefile on purpose: reading three
# values out of a flat file and testing them for existence is what a shell does
# well, and doing it in make needed a parse-time include, an eval'd conditional
# and a sub-make per part. The rejection half does still call make, because the
# behaviour under test is make's own.
#
# Usage: sh tests/check_chips.sh        (make test-chips is the usual entry)

set -u

cd "$(dirname "$0")/.." || exit 1

CHIP_DIR=chips
fail=0

# Every variable a part file must define, with the shape it must have. The
# three *_PATH entries are checked against the filesystem; the other three are
# checked for form.
REQUIRED_PATHS="CHIP_LINKER CHIP_JFLASH CHIP_JDEBUG"
REQUIRED_FORMS="CHIP_DEV_DEF CHIP_SYSCLK_MHZ CHIP_SVD CHIP_STARTUP CHIP_DEVICE_HDR"

parts=$(ls "$CHIP_DIR" 2>/dev/null | sed -n 's/\.mk$//p' | sort)
if [ -z "$parts" ]; then
    echo "  no part files found in $CHIP_DIR/" >&2
    exit 1
fi

for part in $parts; do
    file="$CHIP_DIR/$part.mk"
    # value_of VAR - read one flat "NAME = value" assignment. Part files are
    # restricted to plain assignments: no includes, no conditionals, no
    # continuation lines, so a single pass over the file is enough.
    value_of() {
        sed -n "s/^$1[[:space:]]*=[[:space:]]*//p" "$file" | sed 's/[[:space:]]*#.*$//' | sed 's/[[:space:]]*$//'
    }

    missing_vars=
    for var in $REQUIRED_PATHS $REQUIRED_FORMS; do
        [ -n "$(value_of "$var")" ] || missing_vars="$missing_vars $var"
    done
    if [ -n "$missing_vars" ]; then
        echo "  $part: does not define$missing_vars"
        fail=1
        continue
    fi

    bad=
    for var in $REQUIRED_PATHS; do
        path=$(value_of "$var")
        [ -f "$path" ] || bad="$bad $var -> $path (no such file)"
    done

    case "$(value_of CHIP_DEV_DEF)" in
        -D*) ;;
        *) bad="$bad CHIP_DEV_DEF -> '$(value_of CHIP_DEV_DEF)' is not -D<macro>" ;;
    esac

    case "$(value_of CHIP_SYSCLK_MHZ)" in
        '' | *[!0-9]*) bad="$bad CHIP_SYSCLK_MHZ -> '$(value_of CHIP_SYSCLK_MHZ)' is not a number" ;;
    esac

    if [ -n "$bad" ]; then
        echo "  $part:$bad"
        fail=1
    else
        echo "  $part: ok"
    fi
done

# Rejection half. An unknown token has to fail with a message about the token
# itself; a message about some unrelated prerequisite would make these checks
# pass for the wrong reason (an empty APP trips the APP validation first, for
# instance).
rejects() {
    msg=$(make --no-print-directory APP=1_basic $1 2>&1 >/dev/null)
    case "$msg" in
        *"$2"*) echo "  rejected $1" ;;
        *) echo "  FAIL: $1 was not rejected with '$2'"; fail=1 ;;
    esac
}

rejects OW_TARGET=f401-84 "OW_TARGET='f401-84' is not a known family"
rejects OW_TARGET=f9 "is not a known family"
rejects "OW_TARGET=f4 OW_CHIP=f401" "OW_CHIP='f401' has no chips/f401.mk"
rejects "OW_TARGET=f4 OW_CHIP=f999" "OW_CHIP='f999' has no chips/f999.mk"

if [ "$fail" -ne 0 ]; then
    echo "test-chips: FAILED"
    exit 1
fi

count=$(echo "$parts" | wc -l | tr -d ' ')
echo "test-chips: OK ($count parts: $(echo $parts))"
