# CMSIS dependency pins - the single source of truth for BOTH build systems.
#
# The Makefile includes this file; the CMake build parses it (CMake cannot
# include() a makefile, so - like config/optim.mk and the chips/<part>.mk
# files - it is kept to plain "NAME = value" assignments with no includes,
# conditionals, multi-line values or trailing comments). A trailing comment
# would leave the whitespace in front of the '#' inside the value and
# silently point the download at a 404, which is why the format rule is the
# same one the part files follow.
#
# Every value is a full 40-hex commit SHA, never a branch: raw URLs take the
# SHA as a path component and git takes it as GIT_TAG, so a fresh checkout
# downloads byte-identical files years later. Floating branches bit once:
# system_stm32f3xx.c drifted upstream (VTOR setup) and moved firmware images
# without any repository change.
#
# Pinned 2026-10-14 (branch tips that day; verify with
# git ls-remote <url> HEAD before bumping).
#
# To update: re-run the ls-remote commands, replace the SHAs below, delete
# CMSIS/, re-download every family, and run the full verification
# (host suites, test-ram, the 80-image byte gate) - a CMSIS change is a
# codegen change until the gate says otherwise. Record the new date above.
CMSIS_CORE_REF = 2b7495b8535bdcb306dac29b9ded4cfb679d7e5c
CMSIS_DEVICE_REF_F0 = 1ed05aa865aab1db2db2287077080aa44344fafa
CMSIS_DEVICE_REF_F1 = c8e9a4a4f16b6d2cb2a2083cbe5161025280fb22
CMSIS_DEVICE_REF_F3 = 402c039a0a6d73c31303a05e6a1d6b8b59474fa1
CMSIS_DEVICE_REF_F4 = 9192c7b9df75a142f2027ab266601fe061fc00b3
CMSIS_DEVICE_REF_G0 = 7c77b7cbcee497dfbb5cc08f9562a80515e78013
CMSIS_DEVICE_REF_G4 = 626ee412334a5ed2e5b320af5a8d77d69f03a558
CMSIS_DEVICE_REF_H5 = 884b8dc78e41cbfca008363342b17f4a9e8641f7
CMSIS_SVD_REF = c65f8551e57c770344d229dcaa0bf838fa29aff4
