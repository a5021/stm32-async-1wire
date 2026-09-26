# Optimisation profiles - the single source of truth for BOTH build systems.
#
# The Makefile includes this file; the CMake build parses it (CMake cannot
# include() a makefile, so - like the chips/<part>.mk files - it is kept to
# plain "NAME = value" assignments with no includes, conditionals, multi-line
# values or trailing comments). A trailing comment would leave the whitespace in
# front of the '#' inside the value and silently turn the flags into garbage,
# which is why the format rule is the same one the part files follow.
#
# It lives in config/ rather than chips/ because chips/*.mk is globbed as the
# list of MCU parts: adding a non-part file there would make `optim` look like
# a selectable OW_CHIP to both tests/check_chips.sh and the CMake part check.

# -Os         : Optimize for code size. This driver is polled on a millisecond
#               cadence, so compact code matters more than raw speed. Saves
#               ~65% flash vs the old -O3 + --param max-inline-insns-auto=480
#               (which ballooned main() to ~9 KB by forcing massive inlining).
# -flto       : Link Time Optimization - cross-file optimization during linking
# -g0         : No debug information (reduces binary size, incompatible with
#               debugging)
# This is what all the hardware numbers in README.md were taken with.
OPT_RELEASE = -Os -flto -g0

# -Og -g3 -gdwarf : debuggable, still sane code. Note this profile carries no
# -flto: stepping through optimised-but-inlined code is not debugging.
OPT_DEBUG = -Og -g3 -gdwarf

# Families that must be built without -flto. Cortex-M0 / Cortex-M0+ (F0 / G0)
# trip a GCC 14 LTO link failure ("invalid constant after fixup" in the
# thin-LTO partitioner) when the code shape shifts; dropping LTO there affects
# size only, not correctness. Cortex-M3 (F1) and Cortex-M4 (F4) keep LTO.
# Adding a family here is the only edit either build system needs.
OPT_NO_LTO = f0 g0
