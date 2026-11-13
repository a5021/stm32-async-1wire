#include "onewire.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * libFuzzer harness for onewire_crc16().
 *
 * Properties:
 * 1. Matches the bit-wise reflected 0xA001 definition for every input.
 * 2. Deterministic: same input -> same output.
 * 3. Appending the CRC little-endian leaves a zero residue, which is the
 *    property the DS2431 scratchpad frame check relies on.
 * 4. No crash/UB on any input (up to 255 bytes, uint8_t len).
 *
 * The unit test in test/test_ds2431.c compares the table against the same
 * definition, so a wrong entry is caught there too; this harness keeps the
 * coverage moving over multi-byte inputs, and - unlike fuzz_ds2431_txn - the
 * oracle here does not call the helper it is checking.
 */

static uint16_t crc16_reference(const uint8_t* data, uint8_t len) {
    uint16_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8u; bit++) {
            if (crc & 1u) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u); /* ONEWIRE_CRC16_POLY */
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 255) return 0;

    uint8_t len = (uint8_t)size;

    /* Property 1 and 2. */
    uint16_t crc = onewire_crc16(data, len);
    if (crc != crc16_reference(data, len)) abort();
    if (crc != onewire_crc16(data, len)) abort();

    /* Property 3: two more bytes must still fit the uint8_t length. */
    if (len <= 253u) {
        uint8_t buf[256];
        memcpy(buf, data, size);
        buf[size] = (uint8_t)(crc & 0xFFu);
        buf[size + 1] = (uint8_t)(crc >> 8);
        if (onewire_crc16(buf, (uint8_t)(len + 2u)) != 0) abort();
    }

    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int main(void) {
    /* CRC-16/ARC check value: Maxim 1-Wire CRC-16 of "123456789" = 0xBB3D. */
    const uint8_t test[] = "123456789";
    uint16_t crc = onewire_crc16(test, 9);
    if (crc != 0xBB3D) abort();
    if (crc != crc16_reference(test, 9)) abort();

    /* Empty input returns the initial value (0). */
    if (onewire_crc16(test, 0) != 0) abort();

    /* Append-CRC residue. */
    uint8_t buf[11];
    memcpy(buf, test, 9);
    buf[9] = (uint8_t)(crc & 0xFFu);
    buf[10] = (uint8_t)(crc >> 8);
    if (onewire_crc16(buf, 11) != 0) abort();

    return 0;
}
#endif
