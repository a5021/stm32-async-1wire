#!/bin/sh
# tests/check_mcu_dispatch.sh - cross-check the two MCU dispatch tables
#
# The MCU macro -> (family, -mcpu/-mthumb flags, CMSIS system source) mapping
# is written down twice: once in the Makefile (OW_MCU_TARGET_*, the family
# ifeq chain) for the bare-metal build, and once in cmake/ow_flags.cmake for
# the CMake/FetchContent consumers. The two are edited by hand and nothing
# compared them, so a new part added to one side could silently take the
# other side's core flags - a binary that links but runs on the wrong core.
#
# This script reads both tables and fails if they disagree for any known MCU,
# and separately checks that every chips/<part>.mk names a device macro the
# Makefile knows and the chip the Makefile maps that macro to. It is pure
# text parsing (no toolchain, no cmake), so it runs anywhere check_chips.sh
# does.
#
# Usage: sh tests/check_mcu_dispatch.sh    (make test-mcu-dispatch is the entry)

set -u

cd "$(dirname "$0")/.." || exit 1

MK=Makefile
CM=cmake/ow_flags.cmake
fail=0

for f in "$MK" "$CM"; do
    if [ ! -f "$f" ]; then
        echo "  missing $f" >&2
        exit 1
    fi
done

# ---- Table cross-check ------------------------------------------------------
# First file: parse cmake/ow_flags.cmake into cm_fam/cm_flags/cm_sys keyed by
# every STM32 token in a block's (possibly multi-line) elseif condition.
# Second file: parse the Makefile tables plus the family ifeq chain.
awk -v MK="$MK" -v CM="$CM" '
function flush(   s, m) {
    if (cond == "") return
    s = cond
    while (match(s, /STM32[A-Za-z0-9_]+/)) {
        m = substr(s, RSTART, RLENGTH)
        cm_fam[m] = fam
        cm_flags[m] = flags
        cm_sys[m] = sys
        s = substr(s, RSTART + RLENGTH)
    }
    cond = ""
}
function norm(s) { sub(/[ \t]*$/, "", s); return s }
function err(msg) { printf("  ERROR: %s\n", msg); bad = 1 }

{ sub(/\r$/, "") }

FNR == NR {
    if ($0 ~ /^(elseif|if)\(OW_MCU STREQUAL/) { flush(); cond = $0; next }
    if ($0 ~ /^[ \t]+OW_MCU STREQUAL/)        { cond = cond " " $0; next }
    if ($0 ~ /set\(OW_FAMILY[ \t]/)     { v=$0; sub(/.*set\(OW_FAMILY[ \t]+/,"",v);     sub(/\).*/,"",v); fam=v;   next }
    if ($0 ~ /set\(OW_MCU_FLAGS[ \t]/)  { v=$0; sub(/.*set\(OW_MCU_FLAGS[ \t]+/,"",v);  sub(/\).*/,"",v); flags=v; next }
    if ($0 ~ /set\(OW_SYSTEM_SRC[ \t]/) { v=$0; sub(/.*set\(OW_SYSTEM_SRC[ \t]+/,"",v); sub(/\).*/,"",v); sys=v;   next }
    next
}

{
    if ($0 ~ /^OW_MCU_TARGET_/) {
        v=$0; sub(/^OW_MCU_TARGET_/,"",v); sub(/[ \t]*=.*/,"",v); k=v
        v=$0; sub(/.*=[ \t]*/,"",v); mk_fam[k]=norm(v); next
    }
    if ($0 ~ /^OW_MCU_CHIP_/) {
        v=$0; sub(/^OW_MCU_CHIP_/,"",v); sub(/[ \t]*=.*/,"",v); k=v
        v=$0; sub(/.*=[ \t]*/,"",v); mk_chip[k]=norm(v); next
    }
    if ($0 ~ /^OW_KNOWN_MCUS[ \t]*=/) {
        v=$0; sub(/.*=[ \t]*/,"",v); n=split(v, arr, /[ \t]+/)
        for (i=1; i<=n; i++) if (arr[i] != "") known[arr[i]] = 1
        next
    }
    if ($0 ~ /OW_TARGET\),/ && $0 ~ /ifeq/) {
        f=$0; sub(/.*OW_TARGET\),/,"",f); sub(/\).*/,"",f); curfam=f; next
    }
    if ($0 ~ /^[ \t]*else[ \t]*$/) { curfam="f1"; next }
    if ($0 ~ /^MCU[ \t]*=/) {
        v=$0; sub(/^MCU[ \t]*=[ \t]*/,"",v); fam_flags[curfam]=norm(v); next
    }
    if ($0 ~ /^SRC[ \t]*=/) {
        if (match($0, /system_stm32[A-Za-z0-9_]+\.c/)) fam_sys[curfam]=substr($0, RSTART, RLENGTH)
        next
    }
}

END {
    flush()
    for (m in known) {
        if (!(m in cm_fam)) { err(m ": absent from " CM); continue }
        if (!(m in mk_fam)) { err(m ": absent from Makefile OW_MCU_TARGET_*"); continue }
        fam = mk_fam[m]
        if (cm_fam[m] != fam)
            err(m ": family " cm_fam[m] " (" CM ") != " fam " (" MK ")")
        if (fam_flags[fam] != cm_flags[m])
            err(m ": flags [" fam_flags[fam] "] (" MK " " fam ") != [" cm_flags[m] "] (" CM ")")
        if (fam_sys[fam] != cm_sys[m])
            err(m ": system src " fam_sys[fam] " (" MK " " fam ") != " cm_sys[m] " (" CM ")")
    }
    for (m in cm_fam) {
        if (m ~ /^STM32[FGH][0-9][0-9][0-9]/ && !(m in known))
            err(m ": device macro in " CM " is not in Makefile OW_KNOWN_MCUS")
    }
    exit bad
}
' "$CM" "$MK"
if [ $? -ne 0 ]; then
    fail=1
fi

# ---- chips/<part>.mk consistency -------------------------------------------
# Every part file must name a device macro the Makefile knows, and the chip the
# Makefile maps that macro to must be the part file itself.
known=$(sed -n 's/^OW_KNOWN_MCUS[ \t]*=[ \t]*//p' "$MK" | tr -d '\r')
parts=$(ls chips 2>/dev/null | sed -n 's/\.mk$//p' | sort)
if [ -z "$parts" ]; then
    echo "  no part files found in chips/" >&2
    fail=1
fi

for part in $parts; do
    dev=$(sed -n 's/^CHIP_DEV_DEF[ \t]*=[ \t]*//p' "chips/$part.mk" | sed 's/[ \t]*#.*$//' | sed 's/[ \t]*$//' | tr -d '\r')
    macro=${dev#-D}
    case " $known " in
        *" $macro "*) ;;
        *) echo "  chips/$part.mk: CHIP_DEV_DEF $dev is not in Makefile OW_KNOWN_MCUS"; fail=1; continue ;;
    esac
    mapped=$(sed -n "s/^OW_MCU_CHIP_$macro[ \t]*=[ \t]*//p" "$MK" | sed 's/[ \t]*$//' | tr -d '\r')
    if [ "$mapped" != "$part" ]; then
        echo "  chips/$part.mk: OW_MCU_CHIP_$macro = '$mapped' (expected '$part')"
        fail=1
    else
        echo "  chips/$part.mk: ok ($macro -> $mapped)"
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "test-mcu-dispatch: FAILED"
    exit 1
fi

count=$(echo $known | wc -w | tr -d ' ')
echo "test-mcu-dispatch: OK ($count MCUs cross-checked)"
