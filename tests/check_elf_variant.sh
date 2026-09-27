#!/bin/sh
# Regression guard: does a build produce the firmware it was asked for?
#
# The ELF is named ds18b20_<APP>.elf, so it encodes the app and nothing else,
# while the objects behind it are stamped with the family, part and clock. Build
# one variant, then another, and the second build can find every object it needs
# already older than the ELF left behind by the first: make then relinks nothing
# and `make program` flashes the previous build. Nothing warns, every check
# passes, and the wrong firmware reaches the board.
#
# The test is the sequence B, A, B. The first and third builds are the same
# variant and must produce identical bytes; the middle one is a different family
# and must not. Before the fix the third build returned the middle build's bytes.
set -e
cd "$(dirname "$0")/.."

ELF=build/ds18b20_2_device_search.elf
APP=2_device_search

build_b() { make OW_TARGET=f4 OW_CHIP=f446xx SYSCLK_MHZ=180 "APP=$APP" >/dev/null; }
build_a() { make OW_TARGET=g0 "APP=$APP" >/dev/null; }
hash_of() { sha256sum "$ELF" | cut -c1-16; }

build_b; b1=$(hash_of)
build_a; a=$(hash_of)
build_b; b2=$(hash_of)

echo "  f446@180 first build : $b1"
echo "  g0 build            : $a"
echo "  f446@180 rebuilt    : $b2"

if [ "$b1" != "$b2" ]; then
    echo "  check_elf_variant: FAIL - rebuilding the same variant produced different" >&2
    echo "  bytes ($b1 vs $b2), so the link is not reproducible." >&2
    exit 1
fi
if [ "$b1" = "$a" ]; then
    echo "  check_elf_variant: FAIL - the f446 and g0 builds produced the same bytes," >&2
    echo "  so one of them was not linked at all." >&2
    exit 1
fi
echo "  check_elf_variant: OK (each variant links its own objects)"
