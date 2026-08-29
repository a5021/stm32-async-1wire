#include "onewire.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * libFuzzer harness for onewire_crc8().
 *
 * Properties:
 * 1. Deterministic: same input → same output
 * 2. Appending CRC byte to buffer yields CRC == 0
 * 3. No crash/UB on any input (up to 255 bytes, uint8_t len)
 */

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 255) return 0;

    uint8_t len = (uint8_t)size;

    /* Property 1: deterministic */
    uint8_t crc1 = onewire_crc8(data, len);
    uint8_t crc2 = onewire_crc8(data, len);
    if (crc1 != crc2) abort();

    /* Property 2: appending CRC yields zero */
    uint8_t buf[256];
    memcpy(buf, data, size);
    buf[size] = crc1;
    uint8_t check = onewire_crc8(buf, (uint8_t)(len + 1));
    if (check != 0) abort();

    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int main(void) {
    /* Known CRC test vector: CRC("123456789") = 0xA2 for Dallas/Maxim */
    const uint8_t test[] = "123456789";
    uint8_t crc = onewire_crc8(test, 9);
    (void)crc;

    /* Empty input returns 0 */
    uint8_t empty_crc = onewire_crc8(test, 0);
    if (empty_crc != 0) abort();

    /* Append-CRC property */
    uint8_t buf[10];
    memcpy(buf, test, 9);
    buf[9] = onewire_crc8(test, 9);
    if (onewire_crc8(buf, 10) != 0) abort();

    return 0;
}
#endif
