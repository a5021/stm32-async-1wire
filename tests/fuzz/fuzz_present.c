#include "onewire.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * libFuzzer harness for onewire_present().
 *
 * Properties:
 * 1. Output is always 0 or 1 (never crashes)
 * 2. Deterministic: same input → same output
 * 3. Matches manual range-check against timing constants AND the captured
 *    edge count: an underrun reset cannot report a device just because a
 *    stale pulses[1] happens to sit inside the presence window.
 */

#define RESET_PULSE_MIN 480U
#define RESET_PULSE_MAX 540U
#define PRESENCE_PULSE_MIN 555U /* 480+15+60 */
#define PRESENCE_PULSE_MAX 840U /* 540+60+240 */

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 5) return 0;

    uint16_t pulses[2];
    memcpy(pulses, data, 4);
    uint8_t captured = data[4];

    uint8_t result = onewire_present(pulses, captured);

    /* Property 1: output is 0 or 1 */
    if (result > 1) abort();

    /* Property 2: deterministic */
    uint8_t result2 = onewire_present(pulses, captured);
    if (result != result2) abort();

    /* Property 3: matches manual range check */
    uint16_t reset = pulses[0];
    uint16_t presence = pulses[1];
    uint8_t expected = (captured == 2) && (reset >= RESET_PULSE_MIN) &&
                               (reset <= RESET_PULSE_MAX) &&
                               (presence >= PRESENCE_PULSE_MIN) &&
                               (presence <= PRESENCE_PULSE_MAX)
                           ? 1u
                           : 0u;
    if (result != expected) abort();

    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int main(void) {
    /* Valid presence: reset=500, presence=700, both edges captured */
    uint16_t valid[] = {500, 700};
    if (onewire_present(valid, 2) != 1) abort();

    /* Invalid reset */
    uint16_t bad_reset[] = {100, 700};
    if (onewire_present(bad_reset, 2) != 0) abort();

    /* Invalid presence */
    uint16_t bad_presence[] = {500, 100};
    if (onewire_present(bad_presence, 2) != 0) abort();

    /* Underrun: only the master-release edge was captured, so a pulses[1]
     * inside the presence window is stale memory, not an answer. */
    uint16_t no_presence[] = {500, 700};
    if (onewire_present(no_presence, 1) != 0) abort();
    if (onewire_present(no_presence, 0) != 0) abort();

    return 0;
}
#endif
