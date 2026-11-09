/* Single translation unit that compiles the DS2413 driver together with the
 * shared 1-Wire layer and test accessors. Build ONLY this TU for the driver;
 * do not compile src/ds2413.c or src/onewire.c separately, or their static
 * state would not be shared with the accessors. */
#define HOST_BUILD 1
#include "ds2413_test_access.h"
#include "../src/ds2413.c"
#include "../src/onewire.c"
#include "hw_model.h"
#include <stddef.h>

void ds2413_test_register_buffers(void) {
    /* ctx.u.cap aliases ctx.u.cmd and ctx.u.pulse (one union): the reset and
     * reply captures both start at &ctx.u.cap, and the write feed DMA sources
     * from &ctx.u.cmd[1] (slot 0 is applied to CCR3 by software). */
    hw_register_buf((const void*)&ctx.u.cap);
    hw_register_buf((const void*)&ctx.u.cmd[1]);
}

uint16_t ds2413_test_get_cmd_pulse(uint8_t i) {
    return (uint16_t)((const ow_pulse_t*)ctx.u.cmd)[i];
}

void ds2413_test_reset_ctx(void) {
    ctx.flags = DS2413_FLAG_FINISHED;
    ctx.phase = DS2413_PH_IDLE;
    ctx.command = 0;
    ctx.data = 0;
    ctx.last_pio = 0;
}

uint8_t ds2413_test_is_finished(void) {
    return (uint8_t)((ctx.flags & DS2413_FLAG_FINISHED) != 0u);
}

void ds2413_test_reset_search(void) {
    search_ctx.flags |= ONEWIRE_SEARCH_FLAG_FINISHED;
    search_ctx.phase = ONEWIRE_SEARCH_DONE;
    search_ctx.found = 0;
    search_ctx.max = 0;
    search_ctx.sink = NULL;
    search_ctx.id_bit_number = 0;
    search_ctx.last_discrepancy = 0;
    search_ctx.last_zero = 0;
}
