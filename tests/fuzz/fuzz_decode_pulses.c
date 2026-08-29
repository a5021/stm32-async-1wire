#include "onewire.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * libFuzzer harness for onewire_decode_pulses().
 *
 * Properties:
 * 1. No out-of-bounds write (dst must hold nbytes, pulse must hold nbytes*8)
 * 2. Each decoded byte matches per-bit onewire_bit_from_pulse decode
 * 3. Roundtrip: encode(decode(pulses)) == original pulses (for each bit)
 */

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;

    /* First byte = number of output bytes (0..32) */
    uint8_t nbytes = data[0];
    if (nbytes == 0 || nbytes > 32) return 0;

    /* Need at least nbytes * 8 pulse values */
    size_t pulse_count = (size_t)nbytes * 8;
    if (pulse_count + 1 > size) return 0;

    const uint8_t* pulses = data + 1;
    uint8_t dst[32];

    /* Property 1: no crash/UB */
    memset(dst, 0xAA, sizeof(dst));
    onewire_decode_pulses(dst, pulses, nbytes);

    /* Property 2: each byte matches per-bit decode */
    for (uint8_t b = 0; b < nbytes; b++) {
        uint8_t expected = 0;
        for (int bit = 0; bit < 8; bit++) {
            uint8_t pulse_val = pulses[b * 8 + bit];
            expected |= (uint8_t)(onewire_bit_from_pulse(pulse_val) << bit);
        }
        if (dst[b] != expected) abort();
    }

    /* Property 3: roundtrip encode→decode for each decoded byte */
    if (nbytes <= 8) {
        for (uint8_t b = 0; b < nbytes; b++) {
            uint8_t enc[8];
            onewire_encode_byte(enc, dst[b]);
            uint8_t decoded[1];
            onewire_decode_pulses(decoded, enc, 1);
            if (decoded[0] != dst[b]) abort();
        }
    }

    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int main(void) {
    /* All-short pulses (all 1-bits) */
    uint8_t short_pulses[8];
    memset(short_pulses, 3, 8); /* 3 <= ONEWIRE_SHORT_PULSE_MAX(10) → bit=1 */
    uint8_t dst[1];
    onewire_decode_pulses(dst, short_pulses, 1);
    if (dst[0] != 0xFF) abort();

    /* All-long pulses (all 0-bits) */
    uint8_t long_pulses[8];
    memset(long_pulses, 60, 8); /* 60 > 10 → bit=0 */
    onewire_decode_pulses(dst, long_pulses, 1);
    if (dst[0] != 0x00) abort();

    return 0;
}
#endif
