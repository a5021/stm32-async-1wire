/**
 * @file ds2413.c
 * @brief DS2413 dual-channel addressable switch - placeholder
 *
 * Intentionally empty: the DS2413 driver is not implemented yet. The header
 * (inc/ds2413.h) defines only the family code so a bus search can identify the
 * device. This translation unit is not part of the build; when a real driver
 * lands it should follow the shape of ds2431.c / ds2431_txn.c and call
 * onewire_init() exactly once like the other drivers.
 */

#include "ds2413.h"

/* Keep this a valid (non-empty) translation unit under -Wpedantic. */
typedef uint8_t ds2413_placeholder_t;
