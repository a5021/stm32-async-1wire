#ifndef DS18B20_TEST_ACCESS_H
#define DS18B20_TEST_ACCESS_H
/* Access to driver internals: implemented in ds18b20_test_access.c which
 * #includes src/ds18b20.c, so the driver is compiled ONLY through that TU
 * (it must not also be compiled separately). */
#include "ds18b20.h"
#include <stdint.h>

/* State machine / scratchpad accessors (used by the legacy unit tests). */
ds18b20_state_t ds18b20_test_get_state(void);
void ds18b20_test_set_state(ds18b20_state_t s);
void ds18b20_test_reset_ctx(void);
void ds18b20_test_set_edge(uint8_t i, uint16_t v);
uint16_t ds18b20_test_get_edge(uint8_t i);
void ds18b20_test_set_pulse(uint8_t i, uint8_t v);
uint8_t ds18b20_test_get_scratchpad(uint8_t i);
void ds18b20_test_set_scratchpad(uint8_t i, uint8_t v);
void ds18b20_test_decode_scratchpad(void);
uint8_t ds18b20_test_get_address_mode(void);
void ds18b20_test_set_address_mode(uint8_t m);

/* Internal decode / encode / addressing wrappers. */
int16_t ds18b20_test_decode_temperature(void);
unsigned ds18b20_test_check_presence(void);
uint8_t ds18b20_test_check_scratchpad_crc(void);
void ds18b20_test_encode_byte_pulses(uint8_t* out, uint8_t byte);
void ds18b20_test_build_addr_prefix(void);
void ds18b20_test_build_addr_cmd(uint8_t cmd_byte);
void ds18b20_test_arm_capture(volatile void* dst, uint16_t count, uint16_t width);
void ds18b20_test_get_selected_rom(uint8_t* rom_out);
void ds18b20_test_set_selected_rom(const uint8_t* rom_in);
uint8_t ds18b20_test_get_addr_cmd(uint8_t i);
void ds18b20_test_set_addr_cmd(uint8_t i, uint8_t v);

/* Bus-operation wrappers for the hardware-release tests. */
void test_bus_send_command_n(const uint8_t* cmd, uint16_t slots);
void test_bus_reset(void);
void test_bus_read_pair(void);
void test_bus_write_then_read(uint8_t bit);
void test_bus_write_bit(uint8_t bit);
void test_bus_read_data(void);
void test_bus_wait_conversion(void);
void test_bus_start_cycle_pause(void);
uint8_t test_ds18b20_bus_done(void);
uint8_t test_bus_present(void);

/* Merged-search capture buffer access (search_edge3). */
uint16_t test_search_edge(uint8_t i);
void ds18b20_test_set_search_edge3(uint8_t i, uint16_t v);

/* Idle-HIGH gap (µs) injected between search slots (0 = disabled). */
void ds18b20_test_set_gap_us(uint16_t us);

/* Reset the search context to "no search running" (finished, DONE phase). */
void ds18b20_test_reset_search(void);

/* Device-table / scan-mode accessors (simultaneous multi-device conversion). */
void ds18b20_test_set_device(uint8_t index, const uint8_t* rom);
void ds18b20_test_set_device_count(uint8_t n);
uint8_t ds18b20_test_get_device_count(void);
uint8_t ds18b20_test_get_scan_mode(void);
void ds18b20_test_set_scan_mode(uint8_t m);
uint8_t ds18b20_test_get_scan_index(void);

/* Resolution-change accessors (resolution state machine in src/ds18b20.c). */
void ds18b20_test_set_resolution(uint8_t r); /* set ctx.resolution directly */
uint8_t ds18b20_test_get_res_pulse(uint8_t i); /* pre-built res_ctx.pulses[i] */

/* Reset the resolution context to "no change running" (finished, DONE phase). */
void ds18b20_test_reset_resolution(void);

/* Command-transaction accessors (txn engine in src/ds18b20.c). */
void ds18b20_test_reset_txn(void); /* finished + DONE phase + ok cleared */
uint8_t ds18b20_test_get_txn_pulse(uint8_t i); /* pre-built txn_ctx.pulses[i] */
uint8_t ds18b20_test_get_txn_slots(void); /* txn_ctx.slots */
uint8_t ds18b20_test_get_txn_ok(void); /* txn_ctx.ok */
uint8_t ds18b20_test_get_txn_finished(void); /* txn_ctx.finished */

/* Register the driver's internal DMA buffers with the hardware model. */
void ds18b20_test_register_buffers(void);

#endif /* DS18B20_TEST_ACCESS_H */
