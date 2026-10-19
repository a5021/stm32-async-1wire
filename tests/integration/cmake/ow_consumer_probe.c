/* Minimal downstream consumer of the installed library.
 *
 * Both integration fixtures (tests/integration/cmake/installed and
 * .../fetchcontent) build this one file, so "the consumer compiles" has a
 * single definition rather than one per fixture.
 *
 * It references the public headers only:
 *
 *   ds18b20.h   the driver API;
 *   onewire.h   the bus API, which pulls in ow_board.h (family dispatch,
 *               ow_pulse_t and the board clock defaults).
 *
 * It doubles as the encapsulation probe: the TIM1+DMA engine and the
 * register-level per-family facts (src/ow_port.h, src/ow_mcu.h) are
 * library-internal, so a consumer that compiles this file has proved the
 * installed package carries the whole public API and nothing it would have to
 * reach behind.
 *
 * The checks below have to hold for a consumer that received the package: a
 * CMSIS device/family macro arrived (inc/ow_board.h ends in #error
 * otherwise), the public -D definitions arrived (OW_PORT_SYSCLK_MHZ), and the
 * pulse tunables are within the feed budget. All of these reach a consumer
 * only through the exported target's INTERFACE_COMPILE_DEFINITIONS, so a build
 * that compiles this file has proved that the package handed over its include
 * paths, its public -D definitions and its device macro.
 *
 * Built as a static library, not an executable: this checks headers, defines
 * and target resolution, none of which need a vector table or a linker script.
 * Linking the library into firmware is covered by the Makefile and CMake builds.
 */

#include "ds18b20.h"
#include "onewire.h"

#ifndef OW_PORT_SYSCLK_MHZ
#error "ow_consumer_probe: OW_PORT_SYSCLK_MHZ did not reach the consumer; the package lost its public compile definitions"
#endif

_Static_assert((ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND) < 256u,
               "the slot word does not fit one feed entry (ow_config.h tunables)");

uint8_t ow_consumer_probe(void);

uint8_t ow_consumer_probe(void) {
    ds18b20_init();
    ds18b20_start_measure();
    return (uint8_t)ds18b20_device_count();
}
