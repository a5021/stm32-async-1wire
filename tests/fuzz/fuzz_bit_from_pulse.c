#include "onewire.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * libFuzzer harness for onewire_bit_from_pulse().
 *
 * Properties:
 * 1. Output is always 0 or 1
 * 2. Monotonically non-increasing (shorter pulses → 1, longer → 0)
 * 3. Boundary: dur == ow_short_pulse_max_us → 1, dur == ow_short_pulse_max_us+1 → 0
 * 4. Deterministic
 */

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;

    uint16_t dur;
    memcpy(&dur, data, 2);

    uint8_t result = onewire_bit_from_pulse(dur);

    /* Property 1: output is 0 or 1 */
    if (result > 1) abort();

    /* Property 2: deterministic */
    uint8_t result2 = onewire_bit_from_pulse(dur);
    if (result != result2) abort();

    /* Property 3: monotonicity check with neighbor */
    if (dur > 0) {
        uint8_t prev = onewire_bit_from_pulse((uint16_t)(dur - 1));
        /* If prev==0 then result must also be 0 (monotonically non-increasing) */
        if (prev == 0 && result == 1) abort();
    }

    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int main(void) {
    /* Below threshold → 1 */
    if (onewire_bit_from_pulse(0) != 1) abort();
    if (onewire_bit_from_pulse(5) != 1) abort();
    if (onewire_bit_from_pulse(10) != 1) abort(); /* == ONEWIRE_SHORT_PULSE_MAX */

    /* Above threshold → 0 */
    if (onewire_bit_from_pulse(11) != 0) abort();
    if (onewire_bit_from_pulse(60) != 0) abort();
    if (onewire_bit_from_pulse(0xFFFF) != 0) abort();

    return 0;
}
#endif
