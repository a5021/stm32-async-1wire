#!/bin/sh
# tests/check_version.sh - verify the project version is declared consistently
#
# The version lived in eight places: four macros in inc/ds18b20.h, the CMake
# project(), the two PlatformIO manifests, the Doxygen PROJECT_NUMBER, the
# FetchContent GIT_TAG example in the README, and the release notes body. Nothing
# compared them, which is how v1.8.1 shipped with no [1.8.1] section in
# CHANGELOG.md at all: the tag existed, the release notes did not, and the
# section that should have described it had drifted into [Unreleased].
#
# inc/ds18b20.h is the single source: it is the only place a user compiles
# against, so every other site is compared against the macros there. A bump that
# misses one fails here instead of at the next release.
#
# Matching goes through `tr -d '\r'` and grep -F on purpose. This repo checks out
# with CRLF on Windows (autocrlf) and LF in CI, so a regex anchored with a bare $
# would pass in CI and fail on the machine where the bump was actually made -
# which is the only place this check has to be reliable. Fixed-string matching
# also means the version's dots cannot act as wildcards.
#
# Usage: sh tests/check_version.sh        (make test-version is the usual entry)

set -u

cd "$(dirname "$0")/.." || exit 1

HEADER=inc/ds18b20.h
CHANGELOG=CHANGELOG.md
fail=0

note() { printf '  %s\n' "$1"; }
bad() { printf '  FAIL: %s\n' "$1"; fail=1; }

# fixed-string search, line-ending agnostic
has() { tr -d '\r' < "$1" | grep -qF "$2"; }

# --- the reference version, read out of the four macros -----------------------
maj=$(tr -d '\r' < "$HEADER" | sed -n 's/^#define STM32_ASYNC_1WIRE_VERSION_MAJOR  *\([0-9][0-9]*\).*/\1/p')
min=$(tr -d '\r' < "$HEADER" | sed -n 's/^#define STM32_ASYNC_1WIRE_VERSION_MINOR  *\([0-9][0-9]*\).*/\1/p')
pat=$(tr -d '\r' < "$HEADER" | sed -n 's/^#define STM32_ASYNC_1WIRE_VERSION_PATCH  *\([0-9][0-9]*\).*/\1/p')
str=$(tr -d '\r' < "$HEADER" | sed -n 's/^#define STM32_ASYNC_1WIRE_VERSION_STRING *"\([^"]*\)".*/\1/p')

if [ -z "$maj" ] || [ -z "$min" ] || [ -z "$pat" ] || [ -z "$str" ]; then
    bad "$HEADER: cannot read the four version macros"
    exit 1
fi

want="$maj.$min.$pat"
note "version from $HEADER: $want"

if [ "$str" != "$want" ]; then
    bad "$HEADER: VERSION_STRING is \"$str\" but MAJOR/MINOR/PATCH give \"$want\""
fi

# --- every other declaration site must agree ---------------------------------
check() {
    file=$1
    needle=$2
    what=$3
    if [ ! -f "$file" ]; then
        bad "$file: missing"
        return
    fi
    if has "$file" "$needle"; then
        note "ok   $what ($file)"
    else
        bad "$what: '$needle' not found in $file - did the version bump miss it?"
    fi
}

check CMakeLists.txt     "project(stm32_async_1wire VERSION $want " "CMake project version"
check library.json       "\"version\": \"$want\""                 "PlatformIO library.json"
check library.properties "version=$want"                        "PlatformIO library.properties"
check Doxyfile           "PROJECT_NUMBER         = $want"        "Doxygen PROJECT_NUMBER"
check README.md          "GIT_TAG        v$want"                 "README FetchContent GIT_TAG"

# The Doxygen config carries its own "1.8.17" in the header comment - that is
# Doxygen's version, not the project's. A blanket search-and-replace across the
# repo is exactly the operation that would rewrite it, so it is asserted rather
# than assumed.
if has Doxyfile "# Doxyfile 1.8.17"; then
    note "ok   Doxygen's own version line untouched"
else
    bad "Doxyfile: the '# Doxyfile 1.8.17' header line is gone - a blanket replace probably hit it"
fi

# --- CHANGELOG must describe the version being released ---------------------
# A tag with no section is what went wrong last time, so the section and its
# compare link are part of "the version is consistent".
# A tag with no section is what went wrong last time, so the section and its
# compare link are part of "the version is consistent". The date is required
# rather than optional: a "## [2.0.0] - TBD" placeholder is what a release gets
# tagged with when nobody finishes the notes, and it is the same failure wearing
# a different hat.
if tr -d '\r' < "$CHANGELOG" | grep -qE "^## \[$want\] - [0-9]{4}-[0-9]{2}-[0-9]{2}$"; then
    note "ok   CHANGELOG has a dated [$want] section"
else
    bad "$CHANGELOG: no '## [$want] - YYYY-MM-DD' section - a tag without one ships undocumented"
fi

if has "$CHANGELOG" "[$want]: https://"; then
    note "ok   CHANGELOG has a [$want] compare link"
else
    bad "$CHANGELOG: no '[$want]: https://' compare link"
fi

if has "$CHANGELOG" "[Unreleased]: https://github.com/a5021/stm32-async-1wire/compare/v$want...HEAD"; then
    note "ok   [Unreleased] compares against v$want"
else
    bad "$CHANGELOG: [Unreleased] does not compare against v$want...HEAD"
fi

if [ "$fail" -ne 0 ]; then
    echo "check_version: FAILED"
    exit 1
fi
echo "check_version: OK ($want in every declaration site, documented in $CHANGELOG)"
