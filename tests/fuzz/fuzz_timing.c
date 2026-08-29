#include "onewire.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * libFuzzer harness for onewire_set_timing_profile() /
 * onewire_get_timing_profile() / ow_set_parasite_guard().
 *
 * Properties:
 * 1. Out-of-range profile leaves all timing globals unchanged
 * 2. Each valid profile writes expected values from timing_profiles table
 * 3. Round-trip: get(set(p)) == p
 * 4. Parasite guard selects correct field from timing_profiles
 */

static const onewire_timing_t profiles[ONEWIRE_TIMING_COUNT] = {
    [ONEWIRE_TIMING_FAST]     = {5, 60, 3, 50, 10},
    [ONEWIRE_TIMING_STANDARD] = {5, 60, 5, 100, 10},
    [ONEWIRE_TIMING_SLOW]     = {8, 90, 20, 200, 15},
    [ONEWIRE_TIMING_ROBUST]   = {10, 110, 30, 250, 18},
};

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;

    uint8_t raw_profile = data[0];
    uint8_t parasite = data[1] & 1;

    /* Property 1: out-of-range profile → globals unchanged */
    if (raw_profile >= ONEWIRE_TIMING_COUNT) {
        uint8_t saved_one = ow_one_pulse_us;
        uint8_t saved_zero = ow_zero_pulse_us;
        uint8_t saved_guard = ow_guard_band_us;
        uint8_t saved_short = ow_short_pulse_max_us;

        onewire_set_timing_profile((onewire_timing_profile_t)raw_profile);

        if (ow_one_pulse_us != saved_one) abort();
        if (ow_zero_pulse_us != saved_zero) abort();
        if (ow_guard_band_us != saved_guard) abort();
        if (ow_short_pulse_max_us != saved_short) abort();

        return 0;
    }

    onewire_timing_profile_t profile = (onewire_timing_profile_t)raw_profile;

    /* Property 2: valid profile writes expected values */
    onewire_set_timing_profile(profile);
    if (ow_one_pulse_us != profiles[profile].one_pulse) abort();
    if (ow_zero_pulse_us != profiles[profile].zero_pulse) abort();
    if (ow_short_pulse_max_us != profiles[profile].short_pulse_max) abort();

    /* Property 3: round-trip get(set(p)) == p */
    if (onewire_get_timing_profile() != profile) abort();

    /* Property 4: parasite guard */
    ow_set_parasite_guard(parasite);
    uint8_t expected_guard = parasite ? profiles[profile].parasite_guard_band
                                      : profiles[profile].guard_band;
    if (ow_guard_band_us != expected_guard) abort();

    /* Idempotent */
    ow_set_parasite_guard(parasite);
    if (ow_guard_band_us != expected_guard) abort();

    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int main(void) {
    /* Set each valid profile and verify */
    for (int p = 0; p < ONEWIRE_TIMING_COUNT; p++) {
        onewire_set_timing_profile((onewire_timing_profile_t)p);
        if (onewire_get_timing_profile() != p) abort();
        if (ow_one_pulse_us != profiles[p].one_pulse) abort();
        if (ow_zero_pulse_us != profiles[p].zero_pulse) abort();
    }

    /* Out-of-range doesn't change anything */
    uint8_t saved = ow_one_pulse_us;
    onewire_set_timing_profile((onewire_timing_profile_t)255);
    if (ow_one_pulse_us != saved) abort();

    return 0;
}
#endif
