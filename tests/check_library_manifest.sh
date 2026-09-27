#!/bin/sh
# PlatformIO has no install tree to assert on: library.json is the whole
# contract, and nothing was reading it. This reads the manifest and proves two
# things about it.
#
#   1. It is self-consistent: every file it names exists, every source under
#      build.srcDir is either listed in build.srcFilter or deliberately excluded,
#      and every header it advertises is one the driver actually includes.
#   2. It is sufficient: a consumer translation unit compiles with the include
#      directories and flags the manifest itself declares, and no others.
#
# The consumer is tests/integration/platformio/src/main.c - the same file the
# PlatformIO fixture builds, so this is a check of that fixture's premise rather
# than a separate one. It is compiled with host gcc against the test mocks,
# because the point is the manifest's include set, not the device headers; CI
# already compiles the library for real on ARM in the build and cmake jobs.
#
# What this does NOT prove: that PlatformIO itself accepts the manifest. That
# needs `pio run`, which downloads the ststm32 platform and the STM32Cube
# framework. See the comment in tests/integration/platformio/platformio.ini.
set -e
cd "$(dirname "$0")/.."

fail() { echo "  check_library_manifest: FAIL - $*" >&2; exit 1; }

command -v python3 >/dev/null || fail "python3 is required to read library.json"
command -v gcc >/dev/null || fail "gcc is required to compile the consumer"

# mktemp, not build/: this script must work on a fresh checkout, where build/
# does not exist yet - the whole point of the repo-side checks is that they run
# before anything has been built.
INCS_FILE=$(mktemp)
trap 'rm -f "$INCS_FILE"' EXIT

python3 - <<'PY' > "$INCS_FILE"
import json, sys, os
m = json.load(open("library.json", encoding="utf-8"))
b = m["build"]

dirs = [b["includeDir"]]
flags = []
for f in b.get("flags", []):
    if f.startswith("-I"):
        dirs.append(f[2:])
    else:
        flags.append(f)

missing = [d for d in ([b["includeDir"]] + dirs[1:]) if not os.path.isdir(d)]
if missing:
    sys.stderr.write("include dir(s) named by library.json do not exist: %s\n" % missing)
    sys.exit(1)

hdr_missing = [h for h in m["headers"] if not os.path.isfile(os.path.join(b["includeDir"], h))]
if hdr_missing:
    sys.stderr.write("header(s) advertised by library.json are absent from %s: %s\n"
                     % (b["includeDir"], hdr_missing))
    sys.exit(1)

srcdir = b["srcDir"]

# src/ holds sources that are deliberately NOT in the manifest, so "everything
# under srcDir must be listed" would be the wrong check - it would demand the
# amalgamated driver parts, which ds18b20.c #includes and which must never be
# compiled as separate TUs.
#
#   ds18b20_{measure,resolution,search,txn}.c  the four functional parts of the
#       amalgamated driver TU. CMakeLists.txt marks them HEADER_FILE_ONLY for
#       this reason; listing them here would compile them twice.
#   syscall.c  weak newlib-nano retarget stubs. Its own header says the library
#       target does not carry it: CMake links --specs=nosys.specs, which ships
#       equivalent weak stubs, and the linker collapses duplicate weak symbols.
INTENTIONALLY_UNLISTED = {
    "ds18b20_measure.c", "ds18b20_resolution.c",
    "ds18b20_search.c", "ds18b20_txn.c", "syscall.c",
}

all_c = sorted(f for f in os.listdir(srcdir) if f.endswith(".c"))
listed = {f[2:-1] if f.startswith("+<") else f for f in b["srcFilter"]}

listed_missing = [f for f in sorted(listed) if not os.path.isfile(os.path.join(srcdir, f))]
if listed_missing:
    sys.stderr.write("srcFilter names source(s) that do not exist: %s\n" % listed_missing)
    sys.exit(1)

# Anything new under src/ that is neither listed nor on the exclusion list is
# the drift this exists to catch: a PlatformIO consumer would link a library
# quietly missing it.
unlisted = [f for f in all_c if f not in listed and f not in INTENTIONALLY_UNLISTED]
if unlisted:
    sys.stderr.write(
        "source file(s) under %s/ are neither in library.json build.srcFilter nor\n"
        "known exclusions: %s\n"
        "Add them to srcFilter (or to the exclusion list here, with a reason) -\n"
        "a PlatformIO consumer links a library that quietly lacks them today.\n"
        % (srcdir, unlisted))
    sys.exit(1)

print(" ".join("-I" + d for d in dirs))
PY

INCS=$(cat "$INCS_FILE")
[ -n "$INCS" ] || fail "could not derive an include set from library.json"

# The consumer must compile with the manifest's own include set and nothing
# else. tests/mock supplies the device header the manifest's -Iport/stm32f1
# would otherwise point at, exactly as the host test suite does.
OBJ="${INCS_FILE}.o"
gcc -std=c11 -Wall -Wextra -Werror -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
    -DOW_PORT_TARGET_F1 -DHOST_BUILD -DDS18B20_TEST_HARNESS \
    $INCS -Itests/mock \
    -c tests/integration/platformio/src/main.c -o "$OBJ" \
    || fail "the consumer did not compile against the include set library.json declares"

echo "  check_library_manifest: OK (manifest is self-consistent and its include set builds a consumer)"
