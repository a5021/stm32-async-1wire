/* Consumer main() for the PlatformIO fixture (tests/integration/platformio).
 *
 * Deliberately the smallest thing a real project would write: include the
 * driver, initialise it, and poll. It is a translation unit and nothing more, so
 * a failure here is a failure of the library manifest - the include directory,
 * the source filter or the port include flags in library.json - rather than of
 * anything this file does.
 *
 * Compiled two ways, which is the point of keeping it free of platform headers:
 *
 *   - by PlatformIO, with the flags from the fixture's platformio.ini, against
 *     the real CMSIS device headers the library ships for the chosen part;
 *   - by tests/check_library_manifest.sh, with host gcc against the test mocks,
 *     using the include directories and flags read out of library.json itself.
 *
 * The second is what CI runs, because the first needs the ststm32 platform and
 * the STM32Cube framework downloaded, which costs minutes and hundreds of
 * megabytes for a check that the manifest is consistent.
 */

#include "ds18b20.h"

int main(void) {
    ds18b20_init();
    for (;;) {
        ds18b20_poll();
    }
}
