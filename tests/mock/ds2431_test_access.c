/* Single translation unit that compiles the DS2431 driver together with the
 * shared 1-Wire layer and test accessors. Build ONLY this TU for the driver;
 * do not compile src/ds2431.c or src/onewire.c separately, or their static
 * state would not be shared with the accessors. */
#define HOST_BUILD 1
#include "ds2431_test_access.h"
#include "../src/ds2431.c"
#include "../src/onewire.c"
#include "hw_model.h"
#include <stddef.h>

void ds2431_test_register_buffers(void) {
    /* ctx.u.cap aliases ctx.u.pulse and ctx.u.cmd (one union): the reset and
     * read captures both start at &ctx.u.cap, and the write feed DMA sources
     * from &ctx.u.cmd[1] - the first pulse is applied to CCR3 by software and
     * the CCR3-feed DMA continues from the next element. */
    hw_register_buf((const void*)&ctx.u.cap);
    hw_register_buf((const void*)&ctx.u.cmd[1]);
}

uint16_t ds2431_test_get_cmd_pulse(uint8_t i) {
    return (uint16_t)((const ow_pulse_t*)ctx.u.cmd)[i];
}

void ds2431_test_reset_ctx(void) {
    /* Mirror ds2431_init()'s software state without onewire_init(): the host
     * test/fuzz loop re-runs this between operations, when the port is already
     * initialised. */
    ctx.flags = DS2431_FLAG_FINISHED;
    ctx.phase = DS2431_PH_IDLE;
    ctx.command = 0;
    ctx.len = 0;
    ctx.plen = 0;
    ctx.es = 0;
}

void ds2431_test_set_pending(uint8_t addr_lo, uint8_t addr_hi, uint8_t es) {
    ctx.addr_lo = addr_lo;
    ctx.addr_hi = addr_hi;
    ctx.es = es;
    ctx.flags |= DS2431_FLAG_PENDING;
}

void ds2431_test_set_payload(const uint8_t* data, uint8_t len) {
    if (data == NULL) {
        return;
    }
    if (len > DS2431_SCRATCH_SIZE) {
        len = DS2431_SCRATCH_SIZE;
    }
    for (uint8_t i = 0; i < len; i++) {
        ctx.payload[i] = data[i];
    }
}

uint8_t ds2431_test_is_finished(void) {
    return (uint8_t)((ctx.flags & DS2431_FLAG_FINISHED) != 0u);
}

void ds2431_test_reset_search(void) {
    /* Park the shared 1-Wire search state at FINISHED without onewire_init(),
     * so ds2431_can_start()/ds2431_select() accept new work. Mirrors the
     * ds18b20_test_reset_search() shim. */
    search_ctx.flags |= ONEWIRE_SEARCH_FLAG_FINISHED;
    search_ctx.phase = ONEWIRE_SEARCH_DONE;
    search_ctx.found = 0;
    search_ctx.max = 0;
    search_ctx.sink = NULL;
    search_ctx.id_bit_number = 0;
    search_ctx.last_discrepancy = 0;
    search_ctx.last_zero = 0;
}