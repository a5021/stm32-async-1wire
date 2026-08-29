#include "onewire.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * libFuzzer harness for onewire_pair_bits().
 *
 * Properties:
 * 1. id_bit and cmp_bit are each 0 or 1
 * 2. Deterministic
 * 3. id_bit depends only on edge[0], cmp_bit only on edge[1]
 */

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 4) return 0;

    uint16_t edge[2];
    memcpy(edge, data, 4);

    uint8_t id_bit = 0xFF, cmp_bit = 0xFF;
    onewire_pair_bits(edge, &id_bit, &cmp_bit);

    /* Property 1: outputs are 0 or 1 */
    if (id_bit > 1 || cmp_bit > 1) abort();

    /* Property 2: deterministic */
    uint8_t id2 = 0xFF, cmp2 = 0xFF;
    onewire_pair_bits(edge, &id2, &cmp2);
    if (id_bit != id2 || cmp_bit != cmp2) abort();

    /* Property 3: id_bit matches bit_from_pulse(edge[0]) */
    if (id_bit != onewire_bit_from_pulse(edge[0])) abort();
    if (cmp_bit != onewire_bit_from_pulse(edge[1])) abort();

    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int main(void) {
    /* Short pulse → bit=1, long pulse → bit=0 */
    uint16_t edge_short_short[] = {3, 3};
    uint8_t id_bit, cmp_bit;
    onewire_pair_bits(edge_short_short, &id_bit, &cmp_bit);
    if (id_bit != 1 || cmp_bit != 1) abort();

    uint16_t edge_long_long[] = {60, 60};
    onewire_pair_bits(edge_long_long, &id_bit, &cmp_bit);
    if (id_bit != 0 || cmp_bit != 0) abort();

    uint16_t edge_mixed[] = {3, 60};
    onewire_pair_bits(edge_mixed, &id_bit, &cmp_bit);
    if (id_bit != 1 || cmp_bit != 0) abort();

    return 0;
}
#endif
