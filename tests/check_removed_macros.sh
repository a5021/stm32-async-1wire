#!/bin/sh
# tests/check_removed_macros.sh - the removed macro names must be hard errors
#
# 3.0.0 renamed the public macros (OW_PORT_FAMILY_* -> OW_FAMILY_*,
# OW_PORT_SYSCLK_MHZ -> OW_SYSCLK_MHZ, and the rest) and deliberately shipped
# no compatibility aliases. Before this check, a build that still passed an old
# name compiled cleanly and silently used the renamed knob at its default - the
# exact migration failure the rename was supposed to expose. The guards in
# inc/ow_board.h and inc/ow_config.h turn that into an #error that names the
# replacement.
#
# This script is the tie between the guards and the migration table:
#   1. every old name below is passed to a tiny TU and the compiler must fail;
#   2. stderr must contain the replacement name, so the message is actionable;
#   3. a TU using the new names must still preprocess, so the guards cannot
#      have broken the normal build.
#
# It preprocesses only (-E), no CMSIS download and no ARM toolchain: the
# headers under test are the public ones and the host compiler is enough.
#
# Run: make test-removed-macros

set -u

cd "$(dirname "$0")/.." || exit 1

CC=${CC:-gcc}
INC=inc
TMP=".removed-macros.$$"
if ! mkdir "$TMP"; then
    echo "  check_removed_macros: cannot create $TMP" >&2
    exit 1
fi
trap 'rm -rf "$TMP"' EXIT

TU="$TMP/removed.c"
ERR="$TMP/err.txt"
fail=0
checks=0

# check_removed <old macro> <replacement macro> <header> [extra compiler args...]
check_removed() {
    old=$1
    new=$2
    hdr=$3
    shift 3
    checks=$((checks + 1))
    printf '#include "%s"\n' "$hdr" > "$TU"

    if "$CC" -E -x c "$TU" -I"$INC" -D"$old" "$@" >/dev/null 2>"$ERR"; then
        echo "  $old ($hdr): compiled cleanly - the removed name is still silently accepted" >&2
        fail=1
        return
    fi
    if ! grep -qF "$new" "$ERR"; then
        echo "  $old ($hdr): failed without naming the replacement $new:" >&2
        sed 's/^/    /' "$ERR" >&2
        fail=1
    fi
}

# check_ok <what> <header> [compiler args...]
check_ok() {
    what=$1
    hdr=$2
    shift 2
    checks=$((checks + 1))
    printf '#include "%s"\n' "$hdr" > "$TU"
    if ! "$CC" -E -x c "$TU" -I"$INC" "$@" >/dev/null 2>"$ERR"; then
        echo "  $what: the new names no longer preprocess:" >&2
        sed 's/^/    /' "$ERR" >&2
        fail=1
    fi
}

# --- old family tokens -----------------------------------------------------
check_removed OW_PORT_FAMILY_F0 OW_FAMILY_F0 ow_board.h
check_removed OW_PORT_FAMILY_F1 OW_FAMILY_F1 ow_board.h
check_removed OW_PORT_FAMILY_F3 OW_FAMILY_F3 ow_board.h
check_removed OW_PORT_FAMILY_F4 OW_FAMILY_F4 ow_board.h
check_removed OW_PORT_FAMILY_G0 OW_FAMILY_G0 ow_board.h
check_removed OW_PORT_FAMILY_G4 OW_FAMILY_G4 ow_board.h
check_removed OW_PORT_FAMILY_L4 OW_FAMILY_L4 ow_board.h
check_removed OW_PORT_FAMILY_H5 OW_FAMILY_H5 ow_board.h
check_removed OW_PORT_FAMILY_H7 OW_FAMILY_H7 ow_board.h

# --- old clock / ceiling tokens --------------------------------------------
check_removed OW_PORT_SYSCLK_MHZ OW_SYSCLK_MHZ ow_board.h
check_removed OW_PORT_F4_MAX_SYSCLK_MHZ OW_F4_MAX_SYSCLK_MHZ ow_board.h
check_removed OW_PORT_G4_MAX_SYSCLK_MHZ OW_G4_MAX_SYSCLK_MHZ ow_board.h
check_removed OW_PORT_H5_MAX_SYSCLK_MHZ OW_H5_MAX_SYSCLK_MHZ ow_board.h

# --- old feature flags -----------------------------------------------------
check_removed OW_PORT_LOW_POWER OW_LOW_POWER ow_config.h
check_removed PARASITE_POWER OW_PARASITE_POWER ow_config.h

# --- the new names still build ---------------------------------------------
check_ok "new F0 names" onewire.h -DSTM32F0 -DOW_SYSCLK_MHZ=48 \
    -DOW_LOW_POWER=1 -DOW_PARASITE_POWER=1
check_ok "new F4 ceiling" onewire.h -DSTM32F4 -DOW_F4_MAX_SYSCLK_MHZ=168
check_ok "new H5 ceiling" onewire.h -DSTM32H5 -DOW_H5_MAX_SYSCLK_MHZ=250

if [ "$fail" -ne 0 ]; then
    echo "  check_removed_macros: FAIL" >&2
    exit 1
fi
echo "  check_removed_macros: OK ($checks checks; removed names fail and name their replacement, new names still build)"
