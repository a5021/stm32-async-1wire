/* Minimal downstream consumer of the installed library.
 *
 * Both integration fixtures (tests/integration/cmake/installed and
 * .../fetchcontent) build this one file, so "the consumer compiles" has a
 * single definition rather than one per fixture.
 *
 * It deliberately references both layers a real application needs:
 *
 *   ds18b20.h   the driver API;
 *   ow_port.h   the port layer, which is what a consumer needs in order to
 *               configure the bus at all.
 *
 * ow_port.h is self-checking, which is why this file needs no assertions of
 * its own. It ends in `#error` unless one of the OW_PORT_TARGET_* macros
 * arrived (inc/ow_port.h), it compares against OW_PORT_SYSCLK_MHZ, and it
 * _Static_asserts the pulse-timing sum. All three reach a consumer only
 * through the exported target's INTERFACE_COMPILE_DEFINITIONS, so a build that
 * compiles this file has proved that the package handed over its include
 * paths, its public -D definitions and its device macro.
 *
 * Built as a static library, not an executable: this checks headers, defines
 * and target resolution, none of which need a vector table or a linker script.
 * Linking the library into firmware is covered by the Makefile and CMake builds.
 */

#include "ds18b20.h"
#include "ow_port.h"

uint8_t ow_consumer_probe(void);

uint8_t ow_consumer_probe(void) {
    ds18b20_init();
    ds18b20_start_measure();
    return (uint8_t)ds18b20_device_count();
}
