#include "onewire.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * libFuzzer harness for onewire_encode_byte().
 *
 * Properties:
 * 1. Every output element is either ow_one_pulse_us or ow_zero_pulse_us
 * 2. Roundtrip: decode(encode(byte)) == byte
 * 3. Deterministic
 */

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;

    uint8_t byte = data[0];
    uint8_t out[8];

    /* Property 1: no crash/UB */
    onewire_encode_byte(out, byte);

    /* Property 2: each output is one of two valid pulse values */
    for (int i = 0; i < 8; i++) {
        if (out[i] != ow_one_pulse_us && out[i] != ow_zero_pulse_us) abort();
    }

    /* Property 3: roundtrip decode→encode */
    uint8_t decoded[1];
    onewire_decode_pulses(decoded, out, 1);
    if (decoded[0] != byte) abort();

    /* Property 4: deterministic */
    uint8_t out2[8];
    onewire_encode_byte(out2, byte);
    if (memcmp(out, out2, 8) != 0) abort();

    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int main(void) {
    uint8_t out[8];

    /* Encode 0x00: all bits 0 → all pulses should be ow_zero_pulse_us */
    onewire_encode_byte(out, 0x00);
    for (int i = 0; i < 8; i++) {
        if (out[i] != ow_zero_pulse_us) abort();
    }

    /* Encode 0xFF: all bits 1 → all pulses should be ow_one_pulse_us */
    onewire_encode_byte(out, 0xFF);
    for (int i = 0; i < 8; i++) {
        if (out[i] != ow_one_pulse_us) abort();
    }

    /* Roundtrip all 256 byte values */
    for (int b = 0; b < 256; b++) {
        onewire_encode_byte(out, (uint8_t)b);
        uint8_t decoded[1];
        onewire_decode_pulses(decoded, out, 1);
        if (decoded[0] != (uint8_t)b) abort();
    }

    return 0;
}
#endif
