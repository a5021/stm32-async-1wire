/**
 * @file ow_glue.h
 * @brief Library-internal bridge between the driver sources and the engine.
 *
 * Two operations have always belonged to the DS18B20/DS2431 drivers, not to a
 * consumer: scheduling a hardware-timed wait (a DS18B20 conversion wait, an
 * EEPROM hold-off window) and kicking the owner's state machine so it
 * advances. They are engine calls, but they are made by the driver, and the
 * two are separate translation units - so the contract between them needs a
 * header.
 *
 * It is not a public header. The engine headers are not installed, and a
 * consumer has no business scheduling a bus wait with a value in timer
 * registers: the wait belongs to the driver that knows the device's timing.
 * That header used to expose the two calls in inc/onewire.h, where they
 * read as part of the bus API; they were never meant to be called from
 * outside the library.
 *
 * Library-internal (src/): reached only by the driver sources and by
 * src/onewire.c, which defines the calls.
 */

#ifndef OW_GLUE_H
#define OW_GLUE_H

#include <stdint.h>

/**
 * @brief Start a hardware-timed wait with the shared one-pulse timer
 * @param[in] arr Auto-reload value (one timer period in µs)
 * @param[in] rcr Repetition counter (number of periods - 1)
 * @note The timer update event fires after (RCR + 1) · ARR microseconds; used
 *       for DS18B20 conversion waits and EEPROM hold-off windows.
 */
void ow_glue_start_timer(uint16_t arr, uint8_t rcr);

/**
 * @brief Raise the timer update flag so the caller's state machine advances
 * @note No hardware timing is involved: the operation this call unblocks is
 *       scheduled by the owner itself. Used by the DS18B20 driver to start one
 *       measurement cycle (ds18b20_start_measure()). Call it only while no
 *       operation is in flight.
 */
void ow_glue_kick(void);

#endif /* OW_GLUE_H */
