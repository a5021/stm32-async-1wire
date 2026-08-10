/* Single translation unit that compiles the driver together with test
 * accessors. Build ONLY this TU for the driver; do not compile src/ds18b20.c
 * separately, or its static state would not be shared with the accessors. */
#define HOST_BUILD 1
#include "ds18b20_test_access.h"
#include "hw_model.h"
#include "../src/ds18b20.c"

void ds18b20_test_register_buffers(void) {
    hw_register_buf((const void*)&ctx.edge);
    hw_register_buf((const void*)(uintptr_t)search_edge3);
    hw_register_buf((const void*)((uintptr_t)conv_cmd + 1u));   /* &conv_cmd[1] */
    hw_register_buf((const void*)((uintptr_t)read_cmd + 1u));   /* &read_cmd[1] */
    hw_register_buf((const void*)((uintptr_t)ctx.addr_cmd + 1u)); /* &addr_cmd[1] */
    hw_register_buf((const void*)((uintptr_t)search_ctx.pulses + 1u)); /* &pulses[1] */
    hw_register_buf((const void*)(uintptr_t)search_read_pulse);
}

uint8_t ds18b20_test_get_state(void) { return ctx.current_state; }
void    ds18b20_test_set_state(uint8_t s) { ctx.current_state = s; }

void ds18b20_test_reset_ctx(void) {
    ctx.fill_union = (uint64_t)-1; /* 0xFF fill, same as ds18b20_poll() */
    ctx.current_state = DS18B20_ST_IDLE;
    ctx.address_mode = 0;
}

void    ds18b20_test_set_edge(uint8_t i, uint16_t v) { ctx.edge[i] = v; }
void    ds18b20_test_set_pulse(uint8_t i, uint8_t v) { ctx.pulse[i] = v; }
uint8_t ds18b20_test_get_scratchpad(uint8_t i) { return ctx.scratchpad[i]; }
void    ds18b20_test_set_scratchpad(uint8_t i, uint8_t v) { ctx.scratchpad[i] = v; }

void ds18b20_test_decode_scratchpad(void) { decode_scratchpad(); }

uint8_t ds18b20_test_get_address_mode(void) { return ctx.address_mode; }
void    ds18b20_test_set_address_mode(uint8_t m) { ctx.address_mode = m; }

void    test_bus_send_command_n(const uint8_t* cmd, uint16_t slots) { send_command_n(cmd, slots); }
void    test_bus_reset(void) { ds18b20_bus_reset(); }
void    test_bus_read_pair(void) { ds18b20_bus_read_pair(); }
void    test_bus_write_then_read(uint8_t bit) { ds18b20_bus_write_then_read(bit); }
void    test_bus_write_bit(uint8_t bit) { ds18b20_bus_write_bit(bit); }
void    test_bus_read_data(void) { read_data(); }
uint8_t test_ds18b20_bus_done(void) { return ds18b20_bus_done(); }
uint8_t test_bus_present(void) { return ds18b20_bus_present(); }
uint16_t test_search_edge(uint8_t i) { return search_edge3[i]; }
