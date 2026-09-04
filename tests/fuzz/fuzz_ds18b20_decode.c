/*
 * libFuzzer harness for DS18B20 decode functions.
 *
 * Compiled as single-TU: #include the source to access static inlines.
 * Properties:
 * - res_config_byte: known mapping 9→0x1F, 10→0x3F, 11→0x5F, 12→0x7F
 * - resolution_to_wait: (RCR+1)*ARR = expected conversion time in µs
 * - decode_temperature: correct conversion from raw int16 to tenths-C
 * - check_scratchpad_crc: == onewire_crc8(scratchpad, 8)
 */

#include "ds18b20.h"
#include "onewire.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Access ds18b20 internals via test accessor */
#include "../mock/ds18b20_test_access.c"
#include "../mock/ds18b20_test_access.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 4) return 0;

    uint8_t action = data[0] & 3;

    switch (action) {
    case 0: {
        /* Fuzz res_config_byte */
        uint8_t res = data[1];
        uint8_t result = res_config_byte(res);

        /* For valid resolutions 9-12, check known values */
        if (res == 9 && result != 0x1F) abort();
        if (res == 10 && result != 0x3F) abort();
        if (res == 11 && result != 0x5F) abort();
        if (res == 12 && result != 0x7F) abort();

        /* Bits 4:0 are always 0x1F */
        if ((result & 0x1F) != 0x1F) abort();
        break;
    }
    case 1: {
        /* Fuzz resolution_to_wait */
        uint8_t res = data[1];
        uint16_t arr = 0;
        uint8_t rcr = 0;
        resolution_to_wait(res, &arr, &rcr);

        /* ARR and RCR must be non-zero for valid resolutions */
        if (res >= 9 && res <= 12) {
            if (arr == 0) abort();
            /* (RCR+1)*ARR should equal expected conversion time */
            uint32_t total_us = (uint32_t)(rcr + 1) * arr;
            if (res == 9 && total_us != 93750) abort();
            if (res == 10 && total_us != 187500) abort();
            if (res == 11 && total_us != 375000) abort();
            if (res == 12 && total_us != 750000) abort();
        }
        break;
    }
    case 2: {
        /* Fuzz decode_temperature with arbitrary scratchpad bytes */
        /* Set up scratchpad[0] and scratchpad[1] from fuzzer data */
        ds18b20_test_set_scratchpad(0, data[1]); /* LSB */
        ds18b20_test_set_scratchpad(1, data[2]); /* MSB */

        int16_t temp = decode_temperature();

        /* Verify: raw = (MSB<<8 | LSB), tenths = raw*10/16 with rounding */
        int16_t raw = (int16_t)((data[2] << 8) | data[1]);
        int32_t expected = ((int32_t)raw * 10 + ((raw < 0) ? -8 : 8)) / 16;
        if (temp != (int16_t)expected) abort();
        break;
    }
    case 3: {
        /* Fuzz check_scratchpad_crc */
        uint8_t len = (size >= 9) ? 8 : (uint8_t)(size - 1);
        for (uint8_t i = 0; i < len; i++) {
            ds18b20_test_set_scratchpad(i, data[1 + i]);
        }
        for (uint8_t i = len; i < 9; i++) {
            ds18b20_test_set_scratchpad(i, 0);
        }

        uint8_t crc = check_scratchpad_crc();
        /* Reconstruct scratchpad for verification */
        uint8_t scratchpad[9] = {0};
        for (uint8_t i = 0; i < len; i++) {
            scratchpad[i] = data[1 + i];
        }
        uint8_t expected_crc = onewire_crc8(scratchpad, 8);
        if (crc != expected_crc) abort();
        break;
    }
    }

    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int main(void) {
    ds18b20_test_register_buffers();
    ds18b20_test_reset_ctx();

    /* res_config_byte known values */
    if (res_config_byte(9) != 0x1F) abort();
    if (res_config_byte(10) != 0x3F) abort();
    if (res_config_byte(11) != 0x5F) abort();
    if (res_config_byte(12) != 0x7F) abort();

    /* resolution_to_wait: verify (RCR+1)*ARR */
    uint16_t arr;
    uint8_t rcr;
    resolution_to_wait(9, &arr, &rcr);
    if ((uint32_t)(rcr + 1) * arr != 93750) abort();
    resolution_to_wait(12, &arr, &rcr);
    if ((uint32_t)(rcr + 1) * arr != 750000) abort();

    /* decode_temperature: 0x0000 → 0 */
    ds18b20_test_set_scratchpad(0, 0x00);
    ds18b20_test_set_scratchpad(1, 0x00);
    if (decode_temperature() != 0) abort();

    /* decode_temperature: 0x0010 → 10 (1.0°C) */
    ds18b20_test_set_scratchpad(0, 0x10);
    ds18b20_test_set_scratchpad(1, 0x00);
    if (decode_temperature() != 10) abort();

    return 0;
}
#endif
