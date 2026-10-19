/**
 * @file ds2431.h
 * @brief Non-blocking DS2431 1 kbit EEPROM driver for STM32
 *
 * This driver implements a strictly non-blocking interface for the DS2431
 * EEPROM using hardware timers and DMA on STM32 microcontrollers, on top of
 * the public 1-Wire layer primitives (onewire_reset() / onewire_write_slots()
 * / onewire_read_data()). It is a self-contained second driver: it does not
 * touch the DS18B20 driver or the 1-Wire layer internals, and is compiled in
 * the same way ("include-only parts" pulled into one umbrella translation
 * unit).
 *
 * Key features:
 * - Pure bare-metal, register-level programming
 * - No interrupts; no software delays or busy-waits in the measurement path
 * - Non-blocking state machine architecture (start/poll pairs)
 * - Read Memory (0xF0) in passes of up to 8 bytes per bus transaction
 * - Full write cycle: Write Scratchpad (0x0F) -> Read Scratchpad (0xAA,
 *   TA/ES echo and written-data read-back verified) -> Copy Scratchpad (0x55)
 * - Parasite-power support: the strong pull-up is engaged for the t_PROG
 *   programming window of Copy Scratchpad (ds2431_set_parasite(1))
 * - Optional Match ROM (0x55 + 8-byte ROM) addressing from a bus search;
 *   Skip ROM (broadcast) is the default
 *
 * Device model (DS2431 datasheet): 128 bytes of EEPROM = 4 pages x 32 bytes,
 * written through an 8-byte scratchpad in page-aligned 8-byte blocks. The
 * scratchpad holds the block being written plus the latched target address
 * and the end-offset status byte ES = (address + length - 1) & 0x07. The Read
 * Scratchpad reply ends with an inverted CRC-16 over the command, the target
 * address (TA1/TA2), the E/S byte and the 8 scratchpad data bytes; the driver
 * reads and verifies it (onewire_crc16()) and additionally compares the
 * TA1/TA2/ES echo against what the last Write Scratchpad latched. Because the
 * device computes that CRC over what it actually stored, the read-back data
 * bytes are also compared with the payload that was sent: a byte corrupted on
 * the way to the device during Write Scratchpad passes the CRC and the echo,
 * so only this comparison catches it before Copy Scratchpad commits it.
 *
 * Usage:
 * 1. Call ds2431_init() once at startup (instead of ds18b20_init(): the two
 *    drivers share the TIM1/DMA/GPIO bus, so only one of them may be used
 *    on a bus at a time)
 * 2. (Optional) Run the non-blocking device search (onewire_search_start()
 *    with family code DS2431_FAMILY_CODE) to get a ROM address, then call
 *    ds2431_select() so subsequent operations are addressed to that device
 * 3. Read back memory: ds2431_read_memory() + ds2431_read_memory_poll()
 * 4. Program a page: ds2431_write_scratchpad() (8-byte block) + poll,
 *    then ds2431_read_scratchpad() + poll (verify the TA/ES echo and the
 *    written data via ds2431_last_command_ok()), then
 *    ds2431_copy_scratchpad() + poll
 * 5. Partial block writes are read-modify-write at the application level:
 *    read the 8-byte block, modify the bytes of interest, write the whole
 *    block back
 */

#ifndef DS2431_H
#define DS2431_H

#include "ow_config.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup DS2431_Exported_Constants DS2431 Exported Constants
 * @{
 */

/**
 * @brief DS2431 1-Wire family code (LSB of the 64-bit ROM address)
 * @note Other 1-Wire devices on the bus use different family codes and are
 *       skipped by a family-filtered device search.
 */
#define DS2431_FAMILY_CODE 0x2D

/**
 * @brief Standard 8 bits per byte
 */
#define DS2431_BITS_PER_BYTE 8

/**
 * @brief Number of bytes in a device ROM address
 */
#define DS2431_ROM_BYTES 8

/**
 * @brief DS2431 EEPROM size in bytes (1 kbit)
 */
#define DS2431_EEPROM_SIZE 128

/**
 * @brief DS2431 EEPROM page size in bytes (4 pages of 32 bytes)
 */
#define DS2431_PAGE_SIZE 32

/**
 * @brief Number of EEPROM pages
 */
#define DS2431_PAGE_COUNT (DS2431_EEPROM_SIZE / DS2431_PAGE_SIZE)

/**
 * @brief DS2431 scratchpad size in bytes (one EEPROM block)
 * @note The EEPROM is programmed in 8-byte blocks, so write cycles must
 *       start at an 8-byte-aligned address and write up to 8 bytes.
 */
#define DS2431_SCRATCH_SIZE 8

/**
 * @brief Maximum length of the Read Scratchpad reply frame in bytes
 * @note 3-byte header (TA1, TA2, ES) + up to 8 scratchpad bytes. The device
 *       returns (E[2:0] - T[2:0] + 1) data bytes, so the reply is only
 *       DS2431_SCRATCH_FRAME long for a full 8-byte window; ds2431_read_scratchpad()
 *       zero-fills the unused tail and the caller reads the real length from
 *       the E/S byte.
 */
#define DS2431_SCRATCH_FRAME (DS2431_SCRATCH_SIZE + 3)

/**
 * @brief Maximum wire length of the Read Scratchpad reply in bytes, CRC included
 * @note The (up to 11-byte) frame (DS2431_SCRATCH_FRAME) followed by the two
 *       inverted CRC-16 bytes the device transmits (datasheet Read Scratchpad
 *       [AAh]). The driver always clocks this many slots - the device drives
 *       logic 1 once it has finished transmitting - and verifies the CRC over
 *       the (E[2:0] - T[2:0] + 1) bytes that were actually sent; the valid
 *       frame is returned to the caller.
 */
#define DS2431_SCRATCH_FRAME_CRC (DS2431_SCRATCH_FRAME + 2)

/**
 * @brief DS2431 Read Memory command
 * @note Reads memory starting at the address given by the TA1/TA2 argument;
 *       the read must stay within EEPROM_SIZE and the driver caps one read
 *       pass at DS2431_READ_PASS bytes (the proven Linux driver chunk size).
 */
#define DS2431_READ_MEMORY 0xF0

/**
 * @brief DS2431 Write Scratchpad command
 * @note Writes up to 8 bytes into the volatile scratchpad and latches the
 *       target address with the end-offset status byte ES. Does not touch
 *       the EEPROM; it takes effect on Copy Scratchpad (DS2431_COPY_SCRATCHPAD).
 */
#define DS2431_WRITE_SCRATCHPAD 0x0F

/**
 * @brief DS2431 Read Scratchpad command
 * @note Returns the frame TA1, TA2, ES followed by (E[2:0] - T[2:0] + 1)
 *       scratchpad data bytes. On the bus the reply continues with an inverted
 *       CRC-16 over the command (0xAA), TA1, TA2, ES and those data bytes; the
 *       driver clocks the maximum window (DS2431_SCRATCH_FRAME_CRC) and
 *       verifies the CRC with onewire_crc16() over the bytes the device
 *       actually transmitted. The TA/ES echo is additionally compared against
 *       what the last Write Scratchpad latched, the PF flag (0x20, set when the
 *       device received fewer bytes than the scratchpad holds) is rejected, and
 *       the read-back data bytes are compared with the payload that was sent,
 *       so a write corrupted in flight is not reported as successful.
 */
#define DS2431_READ_SCRATCHPAD 0xAA

/**
 * @brief DS2431 Copy Scratchpad command
 * @note Copies the scratchpad into the matching EEPROM block. The EEPROM
 *       programming (t_PROG, 10ms max) writes one block in about 11ms; the
 *       driver waits DS2431_TPROG_US, engages the strong pull-up for the whole
 *       window in parasite mode, then reads the device's completion pattern:
 *       0xAA means the block was programmed, logic 1 means the device refused
 *       the copy (PF set, invalid address or copy-protected page) and the
 *       driver reports failure.
 * @note Takes the same byte value (0x55) as the 1-Wire Match ROM ROM-level
 *       command; the two never collide because the ROM-level command phase
 *       completes before a device-level command is issued.
 */
#define DS2431_COPY_SCRATCHPAD 0x55

/**
 * @brief DS2431 Match ROM command (ROM-level addressing prefix)
 * @note Shares its byte value with DS2431_COPY_SCRATCHPAD; see there.
 */
#define DS2431_MATCH_ROM 0x55

/**
 * @brief DS2431 Skip ROM command (ROM-level broadcast prefix)
 */
#define DS2431_SKIP_ROM 0xCC

/**
 * @brief Copy Scratchpad EEPROM programming hold-off in microseconds
 * @note DS2431 datasheet t_PROG: 10ms max; the driver waits 11ms (the value
 *       proven by the Linux driver) to absorb the maximum with headroom.
 */
#define DS2431_TPROG_US 11000

/**
 * @brief Maximum EEPROM bytes read by one Read Memory bus transaction
 * @note 1-Wire reads are captured one bit slot per pulse; capping the pass
 *       keeps the capture buffer small (kernel-proven 8-byte chunk). Large
 *       reads are looped by the application in DS2431_READ_PASS-byte passes.
 */
#define DS2431_READ_PASS 8

/**
 * @}
 */

/**
 * @defgroup DS2431_Exported_Functions DS2431 Exported Functions
 * @{
 */

/**
 * @brief Initialize DS2431 driver hardware and peripherals
 * @pre Call exactly once after MCU reset, after the system clock is configured
 *      and before any 1-Wire bus operation.
 * @note Calls onewire_init(), which takes exclusive ownership of the shared
 *       TIM1/DMA/GPIO resources for the lifetime of the driver (until reset).
 *       The DS18B20 driver and the DS2431 driver share that bus ownership, so
 *       call ds2431_init() INSTEAD of ds18b20_init(); mixing the two drivers
 *       on one bus is unsupported.
 * @warning A second call is unsupported and does not fully reset the timer,
 *          DMA, low-power or DS2431 software state.
 */
void ds2431_init(void);

/**
 * @brief Select which DS2431 device to address by its ROM address
 * @param[in] rom Pointer to the 8-byte ROM address (LSB first), or NULL to
 *                return to Skip ROM (broadcast) addressing
 * @note With a non-NULL address the driver sends Match ROM (0x55) plus the
 *       device address before every command, so only that device responds.
 *       The address should come from a family-filtered device search
 *       (onewire_search_start(sink, max, DS2431_FAMILY_CODE)).
 * @note Applies only while the driver is idle. A select() clears the latched
 *       write-cycle state (pending TA/ES): address the same device for the
 *       whole write_scratchpad -> read_scratchpad -> copy_scratchpad cycle.
 */
void ds2431_select(const uint8_t* rom);

/**
 * @brief Declare the bus as parasite-powered
 * @param[in] parasite 1 = devices are powered over the data line, 0 =
 *                     external VDD supply (default)
 * @note Drives the DS2431 EEPROM programming hold-off (t_PROG) through the
 *       strong pull-up (bus pin switched to push-pull HIGH), then releases
 *       the line back to the passive pull-up. Call once after ds2431_init()
 *       or between operations; applies to all subsequent Copy Scratchpad
 *       runs.
 */
void ds2431_set_parasite(uint8_t parasite);

/**
 * @brief Current parasite-power configuration of the driver
 * @return 1 when the strong pull-up will be engaged during the t_PROG
 *         programming window, 0 for external VDD supply
 */
uint8_t ds2431_parasite_mode(void);

/**
 * @brief Result of the last completed DS2431 operation
 * @return 1 when the last ds2431_*_poll() finished an operation that found a
 *         device present (and, for reads, verified its data - for Copy
 *         Scratchpad, the device's 0xAA completion pattern), 0 when it
 *         aborted (e.g. no device present, a scratchpad echo mismatch, a
 *         refused copy) or nothing ran yet
 */
uint8_t ds2431_last_command_ok(void);

/**
 * @brief Read up to DS2431_READ_PASS bytes of EEPROM (non-blocking)
 * @param[in,out] dst Buffer for the read data; written on success
 * @param[in] addr Start address in EEPROM (0 .. DS2431_EEPROM_SIZE - 1)
 * @param[in] len Bytes to read, 1 .. DS2431_READ_PASS; addr + len must not
 *                exceed DS2431_EEPROM_SIZE
 * @note Issues a Read Memory (0xF0) command from addr. Result validity:
 *       check ds2431_last_command_ok() after the poll reports 1. Out-of-range
 *       requests are ignored. Large regions are read in repeated passes.
 */
void ds2431_read_memory(uint8_t* dst, uint16_t addr, uint8_t len);

/**
 * @brief Advance the non-blocking Read Memory operation
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds2431_read_memory_poll(void);

/**
 * @brief Write up to 8 bytes into the scratchpad (non-blocking)
 * @param[in] addr Block-aligned target address in EEPROM
 * @param[in] data Data to write to the scratchpad
 * @param[in] len Bytes to write, 1 .. DS2431_SCRATCH_SIZE; addr + len must not
 *                exceed DS2431_EEPROM_SIZE
 * @return 1 if the Write Scratchpad was scheduled, 0 if the request was
 *         rejected (NULL data, len out of range, addr + len beyond the EEPROM,
 *         addr not 8-byte aligned, or an operation is already in flight / a
 *         search is running)
 * @note The EEPROM is programmed in 8-byte blocks: addr must be 8-byte
 *       aligned (a misaligned addr is rejected) and len should be the full
 *       block. A shorter len leaves the remaining scratchpad bytes stale, sets
 *       the device's PF flag, and Copy Scratchpad then refuses the block; the
 *       driver reports that as a failure (ds2431_read_scratchpad() and
 *       ds2431_copy_scratchpad() both clear the success flag). The write only
 *       fills the volatile scratchpad; run ds2431_read_scratchpad() to verify
 *       and ds2431_copy_scratchpad() to program the EEPROM.
 */
uint8_t ds2431_write_scratchpad(uint16_t addr, const uint8_t* data, uint8_t len);

/**
 * @brief Advance the non-blocking Write Scratchpad operation
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds2431_write_scratchpad_poll(void);

/**
 * @brief Read the scratchpad frame (non-blocking)
 * @param[in,out] buf Buffer for TA1, TA2, ES and the scratchpad bytes; written
 *                    on success. The caller reads the real data length from the
 *                    E/S byte (buf[2] & 0x07); bytes beyond the reply window are
 *                    zeroed, so the buffer is always DS2431_SCRATCH_FRAME long.
 * @note The device returns (E[2:0] - T[2:0] + 1) data bytes: the driver clocks
 *       the maximum reply and verifies the inverted CRC-16 over what was
 *       actually transmitted. When a Write Scratchpad completed since the last
 *       reset/select, the driver verifies the echoed TA1/TA2/ES against the
 *       latched values, rejects a set PF flag (incomplete write) and compares
 *       the read-back data with the payload that was sent; it clears the
 *       success flag on any mismatch (ds2431_last_command_ok() reports 0).
 *       Without a pending write there is nothing to compare against, so the
 *       frame is returned with the flag set from presence.
 */
void ds2431_read_scratchpad(uint8_t* buf);

/**
 * @brief Advance the non-blocking scratchpad frame read
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds2431_read_scratchpad_poll(void);

/**
 * @brief Copy the scratchpad into the EEPROM (non-blocking)
 * @note Programs the EEPROM block selected by the last Write Scratchpad
 *       (target address + ES). The driver waits the t_PROG hold-off (11ms),
 *       holds the strong pull-up for the whole window in parasite mode, then
 *       reads the device's completion pattern and sets the success flag only
 *       for 0xAA (logic 1 = the device refused the copy: PF set, invalid
 *       address or copy-protected page). Ignored unless a Write Scratchpad
 *       completed since the last reset/select.
 */
void ds2431_copy_scratchpad(void);

/**
 * @brief Advance the non-blocking Copy Scratchpad operation
 * @return 1 when finished (successfully or aborted), 0 while running
 */
uint8_t ds2431_copy_scratchpad_poll(void);

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif // DS2431_H