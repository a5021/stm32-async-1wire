/* Single translation unit that compiles the driver together with the shared
 * 1-Wire layer and test accessors. Build ONLY this TU for the driver; do not
 * compile src/ds18b20.c or src/onewire.c separately, or their static state
 * would not be shared with the accessors. */
#define HOST_BUILD 1
#include "ds18b20_test_access.h"
#include "../src/ds18b20.c"
#include "../src/onewire.c"
#include "hw_model.h"
#include <stddef.h>

void ds18b20_test_register_buffers(void) {
    hw_register_buf((const void*)&ctx.edge);
    hw_register_buf((const void*)(uintptr_t)search_edge3);
    hw_register_buf((const void*)(uintptr_t)search_pair_edge);
    hw_register_buf((const void*)((uintptr_t)conv_cmd + 1u)); /* &conv_cmd[1] */
    hw_register_buf((const void*)((uintptr_t)read_cmd + 1u)); /* &read_cmd[1] */
    hw_register_buf((const void*)((uintptr_t)ctx.addr_cmd + 1u)); /* &addr_cmd[1] */
    hw_register_buf((const void*)((uintptr_t)search_ctx.pulses + 1u)); /* &pulses[1] */
    hw_register_buf((const void*)(uintptr_t)search_read_pulse);
    hw_register_buf((const void*)((uintptr_t)res_ctx.pulses + 1u)); /* &res_ctx.pulses[1] */
}

ds18b20_state_t ds18b20_test_get_state(void) { return ctx.current_state; }
void ds18b20_test_set_state(ds18b20_state_t s) { ctx.current_state = s; }

void ds18b20_test_reset_ctx(void) {
    ctx.fill_union = (uint64_t)-1; /* 0xFF fill, same as ds18b20_poll() */
    ctx.current_state = DS18B20_ST_IDLE;
    ctx.address_mode = 0;
    ctx.scan_mode = 0;
    ctx.scan_index = 0;
    ctx.resolution = DS18B20_RES_DEFAULT; /* 12 bit, DS18B20 power-on default */
}

void ds18b20_test_set_resolution(uint8_t r) { ctx.resolution = r; }

uint8_t ds18b20_test_get_res_pulse(uint8_t i) { return res_ctx.pulses[i]; }

void ds18b20_test_reset_resolution(void) {
    res_ctx.phase = DS18B20_RES_DONE;
    res_ctx.pending_res = DS18B20_RES_DEFAULT;
    res_ctx.applied = 0;
    res_ctx.finished = 1;
}

void ds18b20_test_set_edge(uint8_t i, uint16_t v) { ctx.edge[i] = v; }
uint16_t ds18b20_test_get_edge(uint8_t i) { return ctx.edge[i]; }
void ds18b20_test_set_pulse(uint8_t i, uint8_t v) { ctx.pulse[i] = v; }
uint8_t ds18b20_test_get_scratchpad(uint8_t i) { return ctx.scratchpad[i]; }
void ds18b20_test_set_scratchpad(uint8_t i, uint8_t v) { ctx.scratchpad[i] = v; }

void ds18b20_test_decode_scratchpad(void) { decode_scratchpad(); }

uint8_t ds18b20_test_get_address_mode(void) { return ctx.address_mode; }
void ds18b20_test_set_address_mode(uint8_t m) { ctx.address_mode = m; }

int16_t ds18b20_test_decode_temperature(void) { return decode_temperature(); }
unsigned ds18b20_test_check_presence(void) { return onewire_present(ctx.edge); }
uint8_t ds18b20_test_check_scratchpad_crc(void) { return check_scratchpad_crc(); }
void ds18b20_test_encode_byte_pulses(uint8_t* out, uint8_t byte) { onewire_encode_byte(out, byte); }
void ds18b20_test_build_addr_prefix(void) { build_addr_prefix(); }
void ds18b20_test_build_addr_cmd(uint8_t cmd_byte) { build_addr_cmd(cmd_byte); }
void ds18b20_test_arm_capture(volatile void* dst, uint16_t count, uint16_t width) { arm_capture(dst, count, width); }
void ds18b20_test_get_selected_rom(uint8_t* rom_out) {
    for (int i = 0; i < DS18B20_ROM_BYTES; i++) {
        rom_out[i] = ctx.selected_rom[i];
    }
}
void ds18b20_test_set_selected_rom(const uint8_t* rom_in) {
    for (int i = 0; i < DS18B20_ROM_BYTES; i++) {
        ctx.selected_rom[i] = rom_in[i];
    }
}
uint8_t ds18b20_test_get_addr_cmd(uint8_t i) { return ctx.addr_cmd[i]; }
void ds18b20_test_set_addr_cmd(uint8_t i, uint8_t v) { ctx.addr_cmd[i] = v; }

void test_bus_send_command_n(const uint8_t* cmd, uint16_t slots) { onewire_write_slots(cmd, slots); }
void test_bus_reset(void) { onewire_reset(ctx.edge); }
void test_bus_read_pair(void) { onewire_read_pair(ctx.edge); }
void test_bus_write_then_read(uint8_t bit) { onewire_write_then_read(bit); }
void test_bus_write_bit(uint8_t bit) { onewire_write_bit(bit); }
void test_bus_read_data(void) { onewire_read_data(ctx.pulse, DS18B20_SCRATCHPAD_LEN); }
void test_bus_wait_conversion(void) { wait_conversion(); }
void test_bus_start_cycle_pause(void) { start_cycle_pause(); }
uint8_t test_ds18b20_bus_done(void) { return onewire_bus_done(); }
uint8_t test_bus_present(void) { return onewire_present(ctx.edge); }
uint16_t test_search_edge(uint8_t i) { return search_edge3[i]; }
void ds18b20_test_set_search_edge3(uint8_t i, uint16_t v) { search_edge3[i] = v; }

void ds18b20_test_reset_search(void) {
    search_ctx.finished = 1;
    search_ctx.phase = ONEWIRE_SEARCH_DONE;
    search_ctx.found = 0;
    search_ctx.max = 0;
    search_ctx.sink = NULL;
    search_ctx.id_bit_number = 0;
    search_ctx.last_discrepancy = 0;
    search_ctx.last_zero = 0;
    dev_count = 0;
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        search_ctx.rom[i] = 0;
    }
}

void ds18b20_test_set_device(uint8_t index, const uint8_t* rom) {
    if (index >= DS18B20_MAX_DEVICES) {
        return;
    }
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        dev_roms[index][i] = rom[i];
    }
}

void ds18b20_test_set_device_count(uint8_t n) { dev_count = n; }
uint8_t ds18b20_test_get_device_count(void) { return dev_count; }
uint8_t ds18b20_test_get_scan_mode(void) { return ctx.scan_mode; }
void ds18b20_test_set_scan_mode(uint8_t m) { ctx.scan_mode = m; }
uint8_t ds18b20_test_get_scan_index(void) { return ctx.scan_index; }

void ds18b20_test_set_gap_us(uint16_t us) { onewire_test_set_gap_us(us); }
