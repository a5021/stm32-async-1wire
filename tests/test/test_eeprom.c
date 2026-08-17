/* ============================================================
 *  test_eeprom.c - Copy/Recall EEPROM and Read Power Supply
 *  Transaction Tests
 *
 *  Covers the non-blocking command pairs:
 *   - ds18b20_copy_scratchpad()/poll() (Copy Scratchpad 0x48):
 *     command build, the t_COPY 10ms hold-off wait, feed release
 *   - ds18b20_recall_eeprom()/poll() (Recall EEPROM 0xB8): same
 *     shape with the t_RECALL hold-off
 *   - ds18b20_read_power_supply()/poll() (Read Power Supply 0xB4):
 *     external vs parasite power bit decoding
 *   - presence-abort for all three
 * ============================================================ */

#include "ds18b20.h"
#include "ds18b20_test_access.h"
#include "hw_model.h"
#include "stm32f1xx.h"
#include "unity.h"
#include <string.h>

#define ONE 5u
#define ZERO 60u

/*-------------------------------------------------------------
 *  Shared helpers
 * -----------------------------------------------------------*/

static uint16_t ep_capture_present(uint32_t idx) {
    return idx == 0 ? 510u : 700u;
}

static uint16_t ep_capture_absent(uint32_t idx) {
    (void)idx;
    return 100u;
}

static uint8_t ep_read_pulses[9 * 8];
static uint16_t ep_read_capture(uint32_t idx) { return ep_read_pulses[idx]; }

static void set_read_bits(const uint8_t* bits, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        ep_read_pulses[i] = (bits[i] ? ONE : ZERO);
    }
}

/* Encode len bytes (LSB-first) into the per-bit pulse buffer. */
static void set_read_bytes(const uint8_t* data, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        for (uint8_t b = 0; b < DS18B20_BITS_PER_BYTE; b++) {
            ep_read_pulses[i * 8 + b] = ((data[i] >> b) & 1u) ? ONE : ZERO;
        }
    }
}

/* Build a scratchpad frame with the given TH/TL/CFG and a valid CRC. */
static void make_scratchpad(uint8_t* sd, uint8_t th, uint8_t tl, uint8_t cfg) {
    sd[0] = 0x64; /* temp LSB */
    sd[1] = 0x01; /* temp MSB */
    sd[2] = th;
    sd[3] = tl;
    sd[4] = cfg;
    sd[5] = 0xFF;
    sd[6] = 0x08;
    sd[7] = 0x10;
    sd[8] = ds18b20_crc8(sd, 8);
}

static void run_current_op(void) {
    if (mock_tim1.CR1 & TIM_CR1_CEN) {
        if ((mock_dma1_ch3.CCR & DMA_CCR_EN) && mock_dma1_ch3.CNDTR > 2) {
            hw_set_capture_source(ep_read_capture);
        }
        TEST_ASSERT_TRUE(hw_run_until_uif(256));
    }
}

static void drive_txn(uint8_t (*poll)(void)) {
    uint16_t guard = 0;
    for (;;) {
        if (poll()) {
            break;
        }
        run_current_op();
        if (++guard > 500) {
            break;
        }
    }
    TEST_ASSERT_TRUE(guard <= 500);
}

/*-------------------------------------------------------------
 *  1. Copy Scratchpad
 * -----------------------------------------------------------*/

void test_copy_scratchpad_pulses_built(void) {
    ds18b20_test_set_address_mode(0); /* Skip ROM */
    ds18b20_copy_scratchpad();
    /* 0xCC + 0x48 */
    static const uint8_t k_bytes[] = {0xCC, 0x48};
    for (uint8_t i = 0; i < sizeof(k_bytes); i++) {
        for (uint8_t b = 0; b < DS18B20_BITS_PER_BYTE; b++) {
            uint16_t want = ((k_bytes[i] >> b) & 1u) ? ONE : ZERO;
            TEST_ASSERT_EQUAL_UINT16(want, ds18b20_test_get_txn_pulse((uint8_t)(i * 8 + b)));
        }
    }
    TEST_ASSERT_EQUAL_UINT8(16, ds18b20_test_get_txn_slots());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_txn_pulse(16));
}

void test_copy_scratchpad_match_rom_built(void) {
    uint8_t rom[DS18B20_ROM_BYTES] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    ds18b20_select(rom);
    ds18b20_copy_scratchpad();
    /* 0x55 + ROM + 0x48 */
    static const uint8_t k_bytes[] = {0x55, 0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x48};
    TEST_ASSERT_EQUAL_UINT8(80, ds18b20_test_get_txn_slots());
    for (uint8_t i = 0; i < 10; i++) {
        uint8_t want_byte = k_bytes[i];
        for (uint8_t b = 0; b < DS18B20_BITS_PER_BYTE; b++) {
            uint16_t want = ((want_byte >> b) & 1u) ? ONE : ZERO;
            TEST_ASSERT_EQUAL_UINT16(want, ds18b20_test_get_txn_pulse((uint8_t)(i * 8 + b)));
        }
    }
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_txn_pulse(80));
}

void test_copy_scratchpad_waits_10ms(void) {
    hw_set_capture_source(ep_capture_present);
    ds18b20_copy_scratchpad();
    drive_txn(ds18b20_copy_scratchpad_poll);

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_last_command_ok());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_txn_finished());
    /* The last scheduled op was the t_COPY hold-off timer: 10ms @ 1µs/tick. */
    TEST_ASSERT_EQUAL_UINT32(10000, mock_tim1.ARR);
    TEST_ASSERT_EQUAL_UINT32(0, mock_tim1.RCR);
}

void test_copy_scratchpad_feed_release(void) {
    hw_set_capture_source(ep_capture_present);
    ds18b20_test_set_address_mode(0);
    ds18b20_copy_scratchpad();
    TEST_ASSERT_FALSE(ds18b20_copy_scratchpad_poll());
    run_current_op();
    TEST_ASSERT_FALSE(ds18b20_copy_scratchpad_poll());
    run_current_op();
    const hw_ccr1_feed_log_t* log = hw_ccr1_feed_log();
    TEST_ASSERT_EQUAL_UINT8(16, log->count);
    TEST_ASSERT_EQUAL_UINT16(0, log->values[15]);
    TEST_ASSERT_EQUAL_UINT16(0, hw_effective_ccr1());
    TEST_ASSERT_FALSE(mock_tim1.CR1 & TIM_CR1_CEN);
    /* finish (write UIF consumed -> DONE, then the timer handover) */
    drive_txn(ds18b20_copy_scratchpad_poll);
}

void test_copy_scratchpad_no_presence_aborts(void) {
    hw_set_capture_source(ep_capture_absent);
    ds18b20_copy_scratchpad();
    drive_txn(ds18b20_copy_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_last_command_ok());
    TEST_ASSERT_EQUAL_UINT8(0, hw_ccr1_feed_log()->count);
}

/*-------------------------------------------------------------
 *  2. Recall EEPROM
 * -----------------------------------------------------------*/

void test_recall_eeprom_pulses_built(void) {
    ds18b20_test_set_address_mode(0); /* Skip ROM */
    ds18b20_recall_eeprom();
    /* 0xCC + 0xB8 */
    static const uint8_t k_bytes[] = {0xCC, 0xB8};
    for (uint8_t i = 0; i < sizeof(k_bytes); i++) {
        for (uint8_t b = 0; b < DS18B20_BITS_PER_BYTE; b++) {
            uint16_t want = ((k_bytes[i] >> b) & 1u) ? ONE : ZERO;
            TEST_ASSERT_EQUAL_UINT16(want, ds18b20_test_get_txn_pulse((uint8_t)(i * 8 + b)));
        }
    }
    TEST_ASSERT_EQUAL_UINT8(16, ds18b20_test_get_txn_slots());
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_txn_pulse(16));
}

void test_recall_eeprom_waits_10ms(void) {
    hw_set_capture_source(ep_capture_present);
    ds18b20_recall_eeprom();
    drive_txn(ds18b20_recall_eeprom_poll);

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_last_command_ok());
    /* t_RECALL hold-off timer: 10ms @ 1µs/tick. */
    TEST_ASSERT_EQUAL_UINT32(10000, mock_tim1.ARR);
    TEST_ASSERT_EQUAL_UINT32(0, mock_tim1.RCR);
}

void test_recall_eeprom_no_presence_aborts(void) {
    hw_set_capture_source(ep_capture_absent);
    ds18b20_recall_eeprom();
    drive_txn(ds18b20_recall_eeprom_poll);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_last_command_ok());
}

/*-------------------------------------------------------------
 *  2b. End-to-end persistence: write -> copy -> recall -> read
 * -----------------------------------------------------------*/

void test_eeprom_persistence_e2e(void) {
    /* Harness note: hw_model does not simulate the DS18B20 EEPROM, so this
     * verifies the full command chaining (Write Scratchpad -> Copy Scratchpad
     * -> Recall EEPROM -> Read Scratchpad) and the scratchpad data-path
     * coherence. True non-volatile retention across a power cycle is covered by
     * hardware validation (demo4). */
    hw_set_capture_source(ep_capture_present);
    ds18b20_test_set_address_mode(0); /* broadcast Skip ROM */

    /* 1. Write TH/TL + CFG into the scratchpad. */
    ds18b20_set_alarm_thresholds(0x19, 0x0F);
    drive_txn(ds18b20_set_alarm_thresholds_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_last_command_ok());

    /* 2. Persist the scratchpad to the EEPROM (Copy Scratchpad, t_COPY). */
    ds18b20_copy_scratchpad();
    drive_txn(ds18b20_copy_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_last_command_ok());

    /* 3. Recall the EEPROM copy back into the scratchpad (t_RECALL). */
    ds18b20_recall_eeprom();
    drive_txn(ds18b20_recall_eeprom_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_last_command_ok());

    /* 4. Read the scratchpad back; feed the expected TH/TL/CFG so the driver
     *    decodes a valid frame and we confirm the data path returns them. */
    uint8_t want[9];
    uint8_t got[9];
    make_scratchpad(want, 0x19, 0x0F, 0x7F); /* 12-bit resolution */
    set_read_bytes(want, 9);
    hw_set_capture_source(ep_capture_present);
    ds18b20_read_scratchpad(got);
    drive_txn(ds18b20_read_scratchpad_poll);
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_last_command_ok());
    TEST_ASSERT_EQUAL_HEX8(0x19, got[2]);
    TEST_ASSERT_EQUAL_HEX8(0x0F, got[3]);
    TEST_ASSERT_EQUAL_UINT8(12, ds18b20_get_resolution()); /* CFG 0x7F -> 12 bit */
}

/*-------------------------------------------------------------
 *  2c. A command must not start while a scan session owns the bus
 * -----------------------------------------------------------*/

void test_command_ignored_during_scan(void) {
    /* A scan session keeps scan_mode == 1 for its whole duration. A command
     * transaction started then would clobber the in-flight scan cycle, so
     * txn_can_start() must reject it (no pulses built, never started). */
    ds18b20_test_set_scan_mode(1);
    ds18b20_set_alarm_thresholds(0x4B, 0x46);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_test_get_txn_slots());
    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_test_get_txn_finished());

    /* Sanity: the identical command starts cleanly once scan mode is cleared. */
    ds18b20_test_reset_txn();
    ds18b20_test_set_scan_mode(0);
    ds18b20_set_alarm_thresholds(0x4B, 0x46);
    TEST_ASSERT_NOT_EQUAL(0, ds18b20_test_get_txn_slots());
}

/*-------------------------------------------------------------
 *  3. Read Power Supply
 * -----------------------------------------------------------*/

void test_power_supply_external(void) {
    uint8_t bits[8] = {1, 1, 1, 1, 1, 1, 1, 1}; /* slot 0 short = external */
    set_read_bits(bits, 8);
    uint8_t is_parasite = 0x55;
    hw_set_capture_source(ep_capture_present);

    ds18b20_read_power_supply(&is_parasite);
    drive_txn(ds18b20_read_power_supply_poll);

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_last_command_ok());
    TEST_ASSERT_EQUAL_UINT8(0, is_parasite);
}

void test_power_supply_parasite(void) {
    uint8_t bits[8] = {0, 1, 1, 1, 1, 1, 1, 1}; /* slot 0 long = parasite */
    set_read_bits(bits, 8);
    uint8_t is_parasite = 0x55;
    hw_set_capture_source(ep_capture_present);

    ds18b20_read_power_supply(&is_parasite);
    drive_txn(ds18b20_read_power_supply_poll);

    TEST_ASSERT_EQUAL_UINT8(1, ds18b20_last_command_ok());
    TEST_ASSERT_EQUAL_UINT8(1, is_parasite);
}

void test_power_supply_command_built(void) {
    ds18b20_test_set_address_mode(0); /* Skip ROM */
    uint8_t is_parasite;
    ds18b20_read_power_supply(&is_parasite);
    /* 0xCC + 0xB4, single read byte */
    static const uint8_t k_bytes[] = {0xCC, 0xB4};
    for (uint8_t i = 0; i < sizeof(k_bytes); i++) {
        for (uint8_t b = 0; b < DS18B20_BITS_PER_BYTE; b++) {
            uint16_t want = ((k_bytes[i] >> b) & 1u) ? ONE : ZERO;
            TEST_ASSERT_EQUAL_UINT16(want, ds18b20_test_get_txn_pulse((uint8_t)(i * 8 + b)));
        }
    }
    TEST_ASSERT_EQUAL_UINT8(16, ds18b20_test_get_txn_slots());
}

void test_power_supply_no_presence_aborts(void) {
    hw_set_capture_source(ep_capture_absent);
    uint8_t is_parasite = 0x55;
    ds18b20_read_power_supply(&is_parasite);
    drive_txn(ds18b20_read_power_supply_poll);
    TEST_ASSERT_EQUAL_UINT8(0, ds18b20_last_command_ok());
    TEST_ASSERT_EQUAL_HEX8(0x55, is_parasite); /* untouched */
}

/*-------------------------------------------------------------
 *  Run all EEPROM / power-supply tests
 * -----------------------------------------------------------*/
void run_test_eeprom(void) {
    TEST_RUN(test_copy_scratchpad_pulses_built);
    TEST_RUN(test_copy_scratchpad_match_rom_built);
    TEST_RUN(test_copy_scratchpad_waits_10ms);
    TEST_RUN(test_copy_scratchpad_feed_release);
    TEST_RUN(test_copy_scratchpad_no_presence_aborts);
    TEST_RUN(test_recall_eeprom_pulses_built);
    TEST_RUN(test_recall_eeprom_waits_10ms);
    TEST_RUN(test_recall_eeprom_no_presence_aborts);
    TEST_RUN(test_eeprom_persistence_e2e);
    TEST_RUN(test_command_ignored_during_scan);
    TEST_RUN(test_power_supply_external);
    TEST_RUN(test_power_supply_parasite);
    TEST_RUN(test_power_supply_command_built);
    TEST_RUN(test_power_supply_no_presence_aborts);
}
