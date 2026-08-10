#include "ds18b20.h"
#include "macro.h"
#include "stm32f1xx.h"
#ifndef HOST_BUILD
#include "app.h"
// Search diagnostics. WARNING: keep this DISABLED on real hardware. Each
// per-operation print is a blocking UART write of ~100-200µs between the merged
// search slots; the DS18B20's slot timer keeps running during that gap and
// samples the bus state, so the write-1 direction bits get misread as 0 and the
// device search drops sensors (observed: 5 real devices found with DIAG off,
// but only 2 of them survive with DIAG on). If diagnostics are needed, buffer
// the values in RAM and dump them after the search completes.
// #define DS18B20_SEARCH_DIAG
#endif

/**
 * @defgroup DS18B20_Private_Types DS18B20 Private Types
 * @{
 */

/**
 * @defgroup DS18B20_Private_Constants DS18B20 Private Constants
 * @{
 */

/** @brief Timer configuration for 1µs resolution (PSC = SYSCLK / 1MHz - 1) */
#ifdef HSI_8MHZ
#define TIM_PRESCALER 7 // 8MHz / 8 = 1MHz → 1µs/tick
#else
#define TIM_PRESCALER 71 // 72MHz / 72 = 1MHz → 1µs/tick
#endif
/** @brief Minimum reset pulse duration in microseconds */
#define RESET_PULSE_MIN 480U
/** @brief Maximum reset pulse duration in microseconds */
#define RESET_PULSE_MAX 540U
/** @brief Minimum presence pulse positive width in microseconds */
#define POSITIVE_WIDTH_MIN 15U
/** @brief Maximum presence pulse positive width in microseconds */
#define POSITIVE_WIDTH_MAX 60U
/** @brief Minimum presence pulse negative width in microseconds */
#define NEGATIVE_WIDTH_MIN 60U
/** @brief Maximum presence pulse negative width in microseconds */
#define NEGATIVE_WIDTH_MAX 240U
/** @brief Calculated minimum presence pulse timing */
#define PRESENCE_PULSE_MIN (RESET_PULSE_MIN + POSITIVE_WIDTH_MIN + NEGATIVE_WIDTH_MIN)
/** @brief Calculated maximum presence pulse timing */
#define PRESENCE_PULSE_MAX (RESET_PULSE_MAX + POSITIVE_WIDTH_MAX + NEGATIVE_WIDTH_MAX)
/** @brief Duration to drive bus low during reset in microseconds */
#define RESET_PULSE_DURATION RESET_PULSE_MIN
/** @brief Total reset timeslot timeout in microseconds */
#define RESET_TIMEOUT (RESET_PULSE_MIN * 2)
/** @brief CRC8 polynomial for DS18B20 scratchpad validation (Dallas/Maxim algorithm) */
#define DS18B20_CRC8_POLY 0x8C
/** @brief Number of bytes to include in CRC calculation */
#define DS18B20_CRC8_BYTES 8
/** @brief Size of edge capture buffer for presence detection */
#define CAPTURE_BUF_SIZE 2
/** @brief Duration of '1' bit pulse in microseconds */
#define ONE_PULSE 5
/** @brief Duration of '0' bit pulse in microseconds */
#define ZERO_PULSE 60
/** @brief Guard band between slots to prevent overlap due to bus rise time and DMA latency */
#define GUARD_BAND 5
/** @brief Number of bits in a byte */
#define DS18B20_BITS_PER_BYTE 8
/** @brief Total length of DS18B20 scratchpad in bytes */
#define DS18B20_SCRATCHPAD_LEN 9
/** @brief Total number of bits in DS18B20 scratchpad */
#define DS18B20_SCRATCHPAD_BITS (DS18B20_SCRATCHPAD_LEN * DS18B20_BITS_PER_BYTE)
/** @brief Total number of bits in a device ROM address */
#define DS18B20_ROM_BITS (DS18B20_ROM_BYTES * DS18B20_BITS_PER_BYTE)
/** @brief DS18B20 1-Wire command codes (internal protocol details) */
#define DS18B20_SEARCH_ROM 0xF0 /**< Search ROM */
#define DS18B20_MATCH_ROM 0x55 /**< Match ROM */
#define DS18B20_READ_SCRATCHPAD 0xBE /**< Read Scratchpad */
#define DS18B20_CONVERT_T 0x44 /**< Convert T */
/** @brief Total slots for Match ROM + 8-byte ROM + command */
#define DS18B20_MATCH_SLOTS ((DS18B20_ROM_BYTES + 2) * DS18B20_BITS_PER_BYTE)
/** @brief Slots for the invariant Match ROM + 8-byte ROM prefix (built on select) */
#define DS18B20_PREFIX_SLOTS ((DS18B20_ROM_BYTES + 1) * DS18B20_BITS_PER_BYTE)
/** @brief Threshold to distinguish short/long pulses (10µs) */
#define SHORT_PULSE_MAX 10U
/** @brief Number of DMA transfers for command transmission (2 bytes × 8 bits) */
#define DS18B20_DMA_TRANSFERS (2 * DS18B20_BITS_PER_BYTE)
/** @brief DMA channel control bits for 16-bit capture: MINC | PSIZE_0 | EN */
#define DS18B20_DMA_CCR_CAPTURE (DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_EN)
/** @brief Timer configuration for wait and pause (ARR, RCR) — 62500 ticks @ 1µs = 62.5ms per period */
#define PAUSE_750MS 62500, 11 /**< 750ms delay for temperature conversion (62.5ms × 12) */
#define PAUSE_5S 62500, 79 /**< 5s pause between measurement cycles (62.5ms × 80) */

/**
 * @brief Convert byte bit to pulse duration (ONE_PULSE µs for '1', ZERO_PULSE µs for '0')
 * @param B Byte value
 * @param N Bit position (0-7)
 * @return Pulse duration in microseconds
 */
#define B2P(B, N) (((B) & (1 << (N))) ? ONE_PULSE : ZERO_PULSE)

/**
 * @brief Convert entire byte to sequence of pulse durations for transmission
 * @param B Byte value to convert
 */
#define BYTE_TO_PULSES(B)                       \
    B2P(B, 0), B2P(B, 1), B2P(B, 2), B2P(B, 3), \
        B2P(B, 4), B2P(B, 5), B2P(B, 6), B2P(B, 7)

/** @brief DS18B20 Convert T command sequence in pulse duration format */
static const uint8_t conv_cmd[] = {BYTE_TO_PULSES(0xCC), BYTE_TO_PULSES(0x44), 0};

/** @brief DS18B20 Read Scratchpad command sequence in pulse duration format */
static const uint8_t read_cmd[] = {BYTE_TO_PULSES(0xCC), BYTE_TO_PULSES(0xBE), 0};

/**
 * @brief Force timer update event and wait for update flag - used for timer initialization
 * @param T Timer register structure
 */
#define FORCE_UPDATE_EVENT(T)   \
    do {                        \
        (T).EGR = TIM_EGR(UG);  \
        __DSB();                \
        (T).SR &= ~TIM_SR(UIF); \
    } while (0)

/**
 * @}
 */

/**
 * @defgroup DS18B20_Private_Types DS18B20 Private Types
 * @{
 */

/**
 * @brief DS18B20 driver context structure using union for memory efficiency
 * @note Different stages of communication use the same memory for different purposes
 */
typedef struct {
    /**
     * @brief Union overlay for memory efficiency
     * @warning CRITICAL INVARIANT: scratchpad[n] aliases pulse[n] (same byte).
     *          decode_scratchpad() MUST read all 8 bits of pulse[byte*8..byte*8+7]
     *          BEFORE writing scratchpad[byte]. Reordering loops will corrupt bytes 0-8.
     */
    union {
        volatile uint16_t edge[DS18B20_SCRATCHPAD_BITS / 2]; /**< Edge timestamps for presence detection */
        volatile uint8_t pulse[DS18B20_SCRATCHPAD_BITS]; /**< Pulse durations for data decoding */
        uint8_t scratchpad[DS18B20_SCRATCHPAD_LEN]; /**< Sensor scratchpad data */
        uint64_t fill_union; /**< Utility field for filling the union */
    };
    uint8_t current_state; /**< Current state of the state machine */
    uint8_t address_mode; /**< 0 = Skip ROM (all devices), non-zero = Match ROM */
    uint8_t selected_rom[DS18B20_ROM_BYTES]; /**< ROM of the selected device */
    uint8_t addr_cmd[DS18B20_MATCH_SLOTS + 1]; /**< Pulse buffer for Match ROM command (+ trailing 0 for hardware bus release) */
} DS18B20_ctx_t;

/**
 * @}
 */

/**
 * @defgroup DS18B20_Private_Variables DS18B20 Private Variables
 * @{
 */

/** @brief Global driver context instance */
static DS18B20_ctx_t ctx;

/**
 * @brief Edge capture buffer for the merged search write+read operation
 * @note Holds [write-slot edge, id_bit, cmp_bit]. Channel 2 capture runs for
 *       the whole timer pass, so the direction-write rising edge is captured
 *       into entry 0 as well; id/cmp must be decoded from entries 1 and 2.
 */
static volatile uint16_t search_edge3[3];
/**
 * @brief Read pulse durations reloaded by DMA for the merged search operation
 *        (channel 4 feeds CCR1 from this). Entry 0 is loaded at slot 1's CC4
 *        event and kicks read slots 2-3, entry 1 re-arms the slot-3 kick, and
 *        the trailing 0 is written during slot 3 so the one-pulse timer stops
 *        with the line released to idle HIGH (hardware bus release).
 */
static const uint8_t search_read_pulse[3] = {ONE_PULSE, ONE_PULSE, 0};

/**
 * @}
 */

/**
 * @defgroup DS18B20_Private_Functions DS18B20 Private Functions
 * @{
 */

/**
 * @brief Default weak implementation for busy indicator (e.g. LED toggling during measurement)
 * @param[in] action 0 = idle, non-zero = busy
 */
__WEAK void ds18b20_busy(unsigned action) {
    (void)action;
    // Default implementation - empty (no LED control)
}

/**
 * @brief Default weak implementation for measurement completion callback
 * @param[in] temp_tenths Temperature value in tenths of degrees Celsius, or error code
 */
__WEAK void ds18b20_complete(int16_t temp_tenths) {
    (void)temp_tenths;
    // Default implementation - empty (no temperature handling)
}

/**
 * @brief Calculate Dallas/Maxim CRC-8 over a byte buffer
 * @param[in] data Input buffer
 * @param[in] len Number of bytes to process
 * @return CRC-8 checksum value
 */
uint8_t ds18b20_crc8(const uint8_t* data, uint8_t len) {
    uint8_t crc = 0;
    // Process each byte in the buffer
    for (uint8_t i = 0; i < len; i++) {
        uint8_t inByte = data[i];
        // Process each bit in the byte using Dallas/Maxim CRC8 algorithm
        for (uint8_t b = 0; b < DS18B20_BITS_PER_BYTE; b++) {
            uint8_t mix = (crc ^ inByte) & 0x01;
            crc >>= 1;
            if (mix) crc ^= DS18B20_CRC8_POLY;
            inByte >>= 1;
        }
    }
    return crc;
}

/**
 * @brief Calculate CRC8 checksum for DS18B20 scratchpad data validation
 * @return CRC8 checksum value
 */
__STATIC_FORCEINLINE uint8_t check_scratchpad_crc(void) {
    return ds18b20_crc8(ctx.scratchpad, DS18B20_CRC8_BYTES);
}

/**
 * @brief Decode pulse durations into scratchpad bytes using bit timing analysis
 * @note Branchless implementation: accumulates bits into native-width variable,
 *       then writes once per byte. Relies on union aliasing invariant — see DS18B20_ctx_t.
 */
__STATIC_FORCEINLINE void decode_scratchpad(void) {
    for (unsigned byte = 0; byte < DS18B20_SCRATCHPAD_LEN; ++byte) {
        const unsigned base = byte * DS18B20_BITS_PER_BYTE;
        unsigned value = 0;
        for (unsigned bit = 0; bit < DS18B20_BITS_PER_BYTE; ++bit) {
            value |= (unsigned)(ctx.pulse[base + bit] <= SHORT_PULSE_MAX) << bit;
        }
        ctx.scratchpad[byte] = (uint8_t)value;
    }
}

/**
 * @brief Convert raw temperature data from scratchpad to tenths of degrees Celsius
 * @return Temperature value in tenths of degrees Celsius
 */
__STATIC_FORCEINLINE int16_t decode_temperature(void) {
    // Combine LSB and MSB of temperature register (bytes 0 and 1)
    int16_t raw = (int16_t)((ctx.scratchpad[1] << 8) | ctx.scratchpad[0]);
    // Convert to tenths of degrees Celsius (raw value in 1/16th degrees)
    // Multiply by 10 then divide by 16 to get value in tenths of degree
    return (raw * 10) / 16;
}

/**
 * @brief Verify presence of DS18B20 sensor by checking reset pulse timing
 * @return 1 if device present, 0 if no device detected
 */
__STATIC_FORCEINLINE unsigned check_presence(void) {
    uint16_t reset = ctx.edge[0];
    uint16_t presence = ctx.edge[1];
    // Validate that reset pulse duration is within specification
    // and presence pulse timing indicates a responding device
    return (reset >= RESET_PULSE_MIN) && (reset <= RESET_PULSE_MAX) &&
           (presence >= PRESENCE_PULSE_MIN) && (presence <= PRESENCE_PULSE_MAX);
}

/**
 * @brief Start timer with specified period and repetition count for precise timing
 * @param[in] arr Auto-reload register value
 * @param[in] rcr Repetition counter value
 */
__STATIC_FORCEINLINE void start_timer(uint16_t arr, uint8_t rcr) {
    T1.ARR = arr;
    T1.RCR = rcr;
    // Force update event to load new values
    FORCE_UPDATE_EVENT(T1);
    // Start timer in One Pulse Mode (OPM) - runs once then stops
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Wait for temperature conversion to complete (750ms typical)
 * @note Non-blocking - starts timer that will generate update event when complete
 */
__STATIC_FORCEINLINE void wait_conversion(void) { start_timer(PAUSE_750MS); }

/**
 * @brief Start inter-measurement pause period (5s)
 * @note Non-blocking - starts timer for inter-measurement delay
 */
__STATIC_FORCEINLINE void start_cycle_pause(void) { start_timer(PAUSE_5S); }

/**
 * @brief Configure timer and DMA for capture operation
 * @param[out] dst Destination buffer for captured data
 * @param[in] count Number of transfers
 * @param[in] width DMA transfer width: 8 for 8-bit, 16 for 16-bit
 */
__STATIC_FORCEINLINE void arm_capture(volatile void* dst, uint16_t count, uint16_t width) {
    T1.CCMR1 = TIM_CCMR1(OC1M_0, OC1M_1, OC1M_2, OC1PE, CC2S_1, IC2F_0, IC2F_1, IC2F_2);
    T1.CCER = TIM_CCER(CC1E, CC2E);
    T1.DIER = TIM_DIER(CC2DE);
    FORCE_UPDATE_EVENT(T1);
    T1.CCR1 = 0;
    D13.CCR = 0;
    D13.CPAR = (uint32_t)&T1.CCR2;
    D13.CMAR = (uint32_t)dst;
    D13.CNDTR = count;
    D13.CCR = DS18B20_DMA_CCR_CAPTURE | ((width == 16) ? DMA_CCR_MSIZE_0 : 0);
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Initialize 1-Wire bus reset sequence using timer and DMA
 */
__STATIC_FORCEINLINE void reset_bus(void) {
    T1.RCR = 0;
    T1.ARR = RESET_TIMEOUT;
    T1.CCR1 = RESET_PULSE_DURATION;
    arm_capture((volatile void*)ctx.edge, CAPTURE_BUF_SIZE, 16);
}

/**
 * @brief Transmit a command sequence of arbitrary length to DS18B20 using DMA
 * @param[in] cmd Pointer to command sequence in pulse duration format
 * @param[in] slots Number of bit slots (bits) to transmit
 * @note Non-blocking - configures hardware to transmit command automatically.
 * @note The buffer must hold `slots + 1` entries and the entry at index
 *       `slots` must be 0: the final CC4-triggered DMA transfer feeds that
 *       trailing 0 into CCR1 during the last slot, so the one-pulse timer
 *       stops with the line already released to idle HIGH (hardware bus
 *       release — no software CCR1 write needed afterwards).
 */
__STATIC_FORCEINLINE void send_command_n(const uint8_t* cmd, uint16_t slots) {
    T1.RCR = slots - 1;
    T1.ARR = ONE_PULSE + ZERO_PULSE + GUARD_BAND;
    T1.CCR1 = cmd[0];
    T1.CCR4 = ONE_PULSE + ZERO_PULSE;
    T1.CCMR1 = TIM_CCMR1(OC1M_0, OC1M_1, OC1M_2);
    T1.CCER = TIM_CCER(CC1E);
    T1.DIER = TIM_DIER(CC4DE);
    FORCE_UPDATE_EVENT(T1);
    D14.CCR = 0;
    D14.CPAR = (uint32_t)&T1.CCR1;
    D14.CMAR = (uint32_t)&cmd[1];
    D14.CNDTR = slots; // Feed slots 2..N, then the trailing 0 (bus release)
    D14.CCR = DMA_CCR(DIR, MINC, PSIZE_0, EN);
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Transmit a two-byte command sequence (Skip ROM + command) using DMA
 * @param[in] cmd Pointer to command sequence in pulse duration format
 * @note Non-blocking - configures hardware to transmit command automatically
 */
__STATIC_FORCEINLINE void send_command(const uint8_t* cmd) {
    send_command_n(cmd, DS18B20_DMA_TRANSFERS);
}

/**
 * @brief Encode a byte into pulse durations (ONE_PULSE for '1', ZERO_PULSE for '0')
 * @param[out] out Output buffer (8 entries)
 * @param[in] byte Byte value to encode
 */
__STATIC_FORCEINLINE void encode_byte_pulses(uint8_t* out, uint8_t byte) {
    for (uint8_t i = 0; i < DS18B20_BITS_PER_BYTE; i++) {
        out[i] = (byte & (1u << i)) ? (uint8_t)ONE_PULSE : (uint8_t)ZERO_PULSE;
    }
}

/**
 * @brief Build the invariant Match ROM prefix (0x55 + selected ROM)
 * @note Fills the first DS18B20_PREFIX_SLOTS entries of ctx.addr_cmd.
 *       The prefix depends only on the selected device, so it is built
 *       once in ds18b20_select() and reused for every command.
 */
__STATIC_FORCEINLINE void build_addr_prefix(void) {
    uint8_t* p = ctx.addr_cmd;
    encode_byte_pulses(p, DS18B20_MATCH_ROM);
    p += DS18B20_BITS_PER_BYTE;
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        encode_byte_pulses(p, ctx.selected_rom[i]);
        p += DS18B20_BITS_PER_BYTE;
    }
}

/**
 * @brief Append one command byte to the pre-built Match ROM prefix
 * @param[in] cmd_byte Command byte to send after the ROM address
 * @note Requires build_addr_prefix() to have been called for the current
 *       selected device. Only the last byte (8 slots) is re-encoded per call.
 */
__STATIC_FORCEINLINE void build_addr_cmd(uint8_t cmd_byte) {
    encode_byte_pulses(&ctx.addr_cmd[DS18B20_PREFIX_SLOTS], cmd_byte);
}

/**
 * @brief Read scratchpad data from DS18B20 using timer capture and DMA
 * @note Non-blocking - configures hardware to capture data automatically
 */
__STATIC_FORCEINLINE void read_data(void) {
    T1.RCR = DS18B20_SCRATCHPAD_BITS - 1;
    T1.ARR = ONE_PULSE + ZERO_PULSE + GUARD_BAND;
    T1.CCR1 = ONE_PULSE;
    arm_capture((volatile void*)ctx.pulse, DS18B20_SCRATCHPAD_BITS, 8);
}

/**
 * @}
 */

/**
 * @defgroup DS18B20_Bus DS18B20 Internal 1-Wire Bus Primitives
 * @brief Private non-blocking 1-Wire bus primitives used by the measurement
 *        state machine and the built-in device search. Each function only
 *        schedules a hardware-timed operation on TIM1/DMA and returns
 *        immediately. They are internal to the driver and not exposed to the
 *        application.
 * @{
 */

/**
 * @brief Schedule a 1-Wire bus reset (presence pulse captured via DMA)
 */
static void ds18b20_bus_reset(void) { reset_bus(); }

/**
 * @brief Non-blocking completion check for the scheduled bus operation
 * @return 1 if finished (update flag cleared), 0 while still running
 */
static uint8_t ds18b20_bus_done(void) {
    if (T1.SR & TIM_SR(UIF)) {
        // No software bus release needed: every operation now returns the line
        // to idle HIGH in hardware. DMA-fed writes (send_command_n,
        // write_then_read) append a trailing 0 to the CCR1 feed, and the
        // direct-write/capture operations (reset, read, single slot) use an
        // OC1PE preload of 0 — both applied exactly when the one-pulse timer
        // stops. The bus therefore idles HIGH between slots regardless of how
        // long software takes to poll, and the next write pulse produces a
        // clean falling edge the DS18B20s re-sync to.
        T1.SR = 0;
        return 1u;
    }
    return 0u;
}

/**
 * @brief Check presence of the last scheduled bus reset
 * @return 1 if at least one device answered, 0 otherwise
 */
static uint8_t ds18b20_bus_present(void) { return (uint8_t)check_presence(); }

/**
 * @brief Encode a byte into pulse durations (ONE_PULSE for '1', ZERO_PULSE for '0')
 * @param[out] out Output buffer (8 entries)
 * @param[in] byte Byte value to encode
 */
static void ds18b20_bus_encode_byte(uint8_t* out, uint8_t byte) {
    encode_byte_pulses(out, byte);
}

/**
 * @brief Schedule a write of `slots` bit slots
 * @note Non-blocking: configures the hardware and returns. The pulses buffer
 *       must stay valid until ds18b20_bus_done() reports completion, since
 *       the DMA feeds CCR1 from it (except for a single slot, written directly).
 * @note For `slots > 1` the buffer must also hold a trailing 0 at index
 *       `slots` (see send_command_n) so the bus is released in hardware.
 */
static void ds18b20_bus_write_slots(const uint8_t* pulses, uint16_t slots) {
    if (slots == 1) {
        // Single slot: no DMA needed, avoids a zero-length DMA transaction
        T1.RCR = 0; // Single slot, no repetition
        T1.ARR = ONE_PULSE + ZERO_PULSE + GUARD_BAND; // Total bit slot time
        T1.CCR1 = pulses[0]; // Pulse duration encodes the bit
        // Configure channel 1 for output compare (drive bus low during the pulse).
        // OC1PE plus a preload zero release the bus at the terminal update event,
        // exactly when the one-pulse timer stops (hardware bus release).
        T1.CCMR1 = TIM_CCMR1(OC1M_0, OC1M_1, OC1M_2, OC1PE);
        T1.CCER = TIM_CCER(CC1E); // Enable output compare
        T1.DIER = 0; // No DMA for a single bit slot
        FORCE_UPDATE_EVENT(T1);
        T1.CCR1 = 0; // Preload 0 -> line idles HIGH when the timer stops
        T1.CR1 = TIM_CR1(OPM, CEN); // Start timer in one-pulse mode
        return;
    }
    send_command_n(pulses, slots);
}

/**
 * @brief Schedule a single-slot write of one raw bit (encodes 0/1 to a pulse)
 * @param[in] bit Bit value to write (0 or 1)
 */
static void ds18b20_bus_write_bit(uint8_t bit) {
    uint8_t pulse = bit ? (uint8_t)ONE_PULSE : (uint8_t)ZERO_PULSE;
    ds18b20_bus_write_slots(&pulse, 1);
}

/**
 * @brief Schedule a two-slot read for the Search ROM id/cmp bit pair
 * @note Non-blocking: captures both slot timings into ctx.edge via DMA.
 *       Read the decoded bits with ds18b20_bus_pair_id()/cmp() once
 *       ds18b20_bus_done() reports completion.
 */
static void ds18b20_bus_read_pair(void) {
    // Configure timer for a two-slot read (id_bit, cmp_bit)
    T1.RCR = 1; // Two read slots, then a single update event
    T1.ARR = ONE_PULSE + ZERO_PULSE + GUARD_BAND; // Total bit slot time
    T1.CCR1 = ONE_PULSE; // Read pulse duration (ONE_PULSE µs)
    // Configure channel 1 for output compare (generate read pulse)
    // Configure channel 2 for input capture (measure return pulse durations)
    T1.CCMR1 = TIM_CCMR1(OC1M_0, OC1M_1, OC1M_2, OC1PE, CC2S_1, IC2F_0, IC2F_1, IC2F_2);
    T1.CCER = TIM_CCER(CC1E, CC2E); // Enable both channels
    T1.DIER = TIM_DIER(CC2DE); // Enable DMA request on capture
    FORCE_UPDATE_EVENT(T1);
    T1.CCR1 = 0; // Clear output compare value
    // Configure DMA to capture both slot timings into the edge buffer
    D13.CCR = 0; // Clear DMA configuration
    D13.CPAR = (uint32_t)&T1.CCR2; // DMA source: capture register
    D13.CMAR = (uint32_t)ctx.edge; // DMA destination: edge timestamp buffer
    D13.CNDTR = 2; // Number of transfers (id_bit, cmp_bit)
    D13.CCR = DMA_CCR(MINC, PSIZE_0, MSIZE_0, EN); // Enable DMA with memory increment
    T1.CR1 = TIM_CR1(OPM, CEN); // Start timer in one-pulse mode
}

/**
 * @brief Decode the id-bit of the last ds18b20_bus_read_pair()
 * @return Bit value (0 or 1)
 */
static uint8_t ds18b20_bus_pair_id(void) {
    return (ctx.edge[0] <= SHORT_PULSE_MAX) ? 1u : 0u;
}

/**
 * @brief Decode the cmp-bit of the last ds18b20_bus_read_pair()
 * @return Bit value (0 or 1)
 */
static uint8_t ds18b20_bus_pair_cmp(void) {
    return (ctx.edge[1] <= SHORT_PULSE_MAX) ? 1u : 0u;
}

/**
 * @brief Schedule a merged single-slot write followed by a two-slot read pair
 * @param[in] bit Direction bit to write in slot 1 (0 or 1)
 * @note Non-blocking: one timer pass runs three slots — a write of `bit`
 *       (slot 1), then a read of the next id/cmp pair (slots 2-3). This
 *       halves the number of timer passes per search bit compared to a plain
 *       write followed by a separate read pair.
 * @note OC1 is configured in PWM mode WITHOUT preload (OC1PE clear), so the
 *       CCR4-triggered DMA reload of the read pulse takes effect immediately
 *       at the end of slot 1. With OC1PE set the reload would be buffered in
 *       the preload register and never applied before the read slots.
 * @note Channel 2 input capture is armed for the whole pass, so the write-slot
 *       rising edge lands in search_edge3[0]. Decode the id/cmp bits from
 *       search_edge3[1] and search_edge3[2] once ds18b20_bus_done() returns 1.
 */
static void ds18b20_bus_write_then_read(uint8_t bit) {
    const uint8_t write_pulse = bit ? (uint8_t)ONE_PULSE : (uint8_t)ZERO_PULSE;
    T1.RCR = 2; // Three slots, then a single update event
    T1.ARR = ONE_PULSE + ZERO_PULSE + GUARD_BAND; // Total bit slot time
    // Arm the direction pulse first. The bus was released idle-high by
    // ds18b20_bus_done(), so this write produces the single clean falling edge
    // the devices re-sync their slot timer to; the pulse then holds the bus low
    // through the whole (fast) setup. Holding it from the top instead of arming
    // it right before CEN means the CC2 capture is armed while the bus is low,
    // so the open-drain RC rise can never be mistaken for a slot edge.
    T1.CCR1 = write_pulse; // Slot 1 write pulse encodes the direction bit
    T1.CCR4 = ONE_PULSE + ZERO_PULSE; // End-of-slot reload trigger
    // OC1 in PWM mode (no preload so the reload is immediate), CC2 capture armed
    T1.CCMR1 = TIM_CCMR1(OC1M_0, OC1M_1, OC1M_2, CC2S_1, IC2F_0, IC2F_1, IC2F_2);
    T1.CCER = TIM_CCER(CC1E, CC2E); // Enable both channels
    // Disconnect DMA requests while re-arming the channels, then re-connect
    // them only after the timer flags are clean and just before starting.
    // (The end-of-slot CC4 compare event of the previous merged operation can
    // leave a pending request that fires the reload DMA immediately on re-arm,
    // overwriting the freshly written direction pulse in CCR1.)
    T1.DIER = 0;
    FORCE_UPDATE_EVENT(T1);
    // DMA Ch3: capture all three slot edges into the merged-edge buffer
    D13.CCR = 0;
    D13.CPAR = (uint32_t)&T1.CCR2;
    D13.CMAR = (uint32_t)search_edge3;
    D13.CNDTR = 3;
    D13.CCR = DMA_CCR(MINC, PSIZE_0, MSIZE_0, EN);
    // DMA Ch4: reload CCR1 with the read pulse for slots 2-3, then write the
    // trailing 0 during slot 3 so the one-pulse timer stops with the line
    // released to idle HIGH (hardware bus release).
    D14.CCR = 0;
    D14.CPAR = (uint32_t)&T1.CCR1;
    D14.CMAR = (uint32_t)search_read_pulse;
    D14.CNDTR = 3;
    D14.CCR = DMA_CCR(DIR, MINC, PSIZE_0, EN);
    T1.SR = 0; // Clear any pending capture/compare flags before enabling DMA requests
    T1.DIER = TIM_DIER(CC2DE, CC4DE); // Capture + CCR1 reload via DMA
    T1.CCR1 = write_pulse; // Re-arm the direction pulse (safe against a stale CC4 DMA reload)
    T1.CR1 = TIM_CR1(OPM, CEN); // Start timer in one-pulse mode
}

/**
 * @}
 */

/**
 * @defgroup DS18B20_Search_Internal DS18B20 Internal Non-Blocking Device Search
 * @brief Maxim Search ROM (0xF0) state machine. The algorithm is inherently
 *        sequential: the direction bit written at position i determines which
 *        devices keep participating at position i+1, so the whole transaction
 *        cannot be batched into a single DMA pass like the fixed measurement
 *        sequence. Instead, the linear blocking loop is decomposed into a
 *        compact state machine with loop counters kept in the context
 *        (id_bit_number, last_discrepancy, ...). Each state performs exactly
 *        one hardware-timed operation via the internal bus primitives, so a
 *        poll call never blocks.
 * @{
 */

/** @brief Search state machine phases */
typedef enum {
    DS18B20_SEARCH_RESET, /**< reset scheduled; check presence, send 0xF0 */
    DS18B20_SEARCH_CMD, /**< 0xF0 sent; prepare first bit iteration */
    DS18B20_SEARCH_READ_PAIR, /**< first id/cmp pair read; compute and write direction */
    DS18B20_SEARCH_WRITE_READ, /**< merged direction write + next pair read completed */
    DS18B20_SEARCH_WRITE_DIR, /**< final direction written; advance bit counters */
    DS18B20_SEARCH_DONE, /**< search finished; restore the driver state */
#ifdef DS18B20_TEST_HARNESS
    DS18B20_SEARCH_GAP /**< [TEST] timed idle-HIGH gap before the next slot */
#endif
} search_phase_t;

/**
 * @brief Non-blocking search context
 * @note Holds the loop counters of the search algorithm; the persistent pulse
 *       buffer (pulses) must stay valid across poll calls because the DMA
 *       feeds CCR1 from it asynchronously while the 0xF0 command is sent.
 */
typedef struct {
    search_phase_t phase; /**< Current phase of the search state machine */
    uint8_t rom[DS18B20_ROM_BYTES]; /**< ROM being assembled (bit by bit) */
    uint8_t pulses[DS18B20_BITS_PER_BYTE + 1]; /**< Pulse buffer for the 0xF0 command (+ trailing 0 for hardware bus release) */
    uint8_t id_bit_number; /**< Current bit position (1..64) */
    uint16_t last_discrepancy; /**< Last discrepancy point (Maxim algorithm) */
    uint16_t last_zero; /**< Last position where the '0' branch was taken */
    uint8_t found; /**< Number of DS18B20 devices found */
    uint8_t max; /**< Maximum number of devices to report */
    uint8_t finished; /**< 1 once the search has completed */
    ds18b20_search_sink_t sink; /**< Per-device callback */
} search_ctx_t;

/** @brief Global search context instance */
static search_ctx_t search_ctx;

#ifdef DS18B20_TEST_HARNESS
/** @brief [TEST] Idle-HIGH gap (µs) inserted after every completed search
 *         operation before scheduling the next one (0 = no gap). */
static uint16_t test_gap_us;
/** @brief [TEST] Search phase to resume after the gap wait completes */
static uint8_t test_gap_pending_phase;

/**
 * @brief [TEST] Set the idle-HIGH gap injected between search slots
 * @param[in] us Gap duration in microseconds (0 disables the injection)
 * @note Temporary test hook for the RTOS-latency experiment only.
 */
void ds18b20_test_set_gap_us(uint16_t us) { test_gap_us = us; }
#endif

/**
 * @brief Start a non-blocking device search
 * @param[in] sink Callback invoked per found DS18B20 device (may be NULL)
 * @param[in] max_devices Maximum number of devices to report (0 aborts)
 */
void ds18b20_search_start(ds18b20_search_sink_t sink, uint8_t max_devices) {
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        search_ctx.rom[i] = 0;
    }
    // Trailing zero consumed by the CCR1-feed DMA's final transfer: this is the
    // hardware bus release after the 0xF0 command (see send_command_n).
    search_ctx.pulses[DS18B20_BITS_PER_BYTE] = 0;
    search_ctx.sink = sink;
    search_ctx.max = max_devices;
    search_ctx.found = 0;
    search_ctx.finished = 0;
    search_ctx.last_discrepancy = 0;
    if (max_devices == 0) {
        search_ctx.phase = DS18B20_SEARCH_DONE;
        return;
    }
    search_ctx.phase = DS18B20_SEARCH_RESET;
    ds18b20_bus_reset(); // Schedule the first hardware operation
}

/**
 * @brief Process one decoded id/cmp pair: pick a direction, update the ROM,
 *        and schedule the next hardware operation
 * @param[in] id_bit Id bit of the current position
 * @param[in] cmp_bit Complement bit of the current position
 * @note For all but the last bit the direction write is merged with the read
 *       of the next pair (DS18B20_SEARCH_WRITE_READ); the 64th bit is written
 *       alone so the device can be finalized.
 */
static void ds18b20_search_advance_bit(uint8_t id_bit, uint8_t cmp_bit) {
    const uint8_t byte_idx = (search_ctx.id_bit_number - 1) / DS18B20_BITS_PER_BYTE;
    const uint8_t mask = (uint8_t)(1u << ((search_ctx.id_bit_number - 1) % DS18B20_BITS_PER_BYTE));
    uint8_t direction;

    if (id_bit && cmp_bit) {
        // No device follows this path - search tree exhausted
        search_ctx.phase = DS18B20_SEARCH_DONE;
        return;
    }
    if (id_bit != cmp_bit) {
        // Single device on this path - its bit fixes the direction
        direction = id_bit;
    } else if (search_ctx.id_bit_number < search_ctx.last_discrepancy) {
        // Follow the previously taken path
        direction = (search_ctx.rom[byte_idx] & mask) ? 1u : 0u;
        if (direction == 0) {
            // Remember the last 0-branch taken at a discrepancy
            search_ctx.last_zero = search_ctx.id_bit_number;
        }
    } else {
        // At the discrepancy point take the '1' branch first
        direction = (search_ctx.id_bit_number == search_ctx.last_discrepancy) ? 1u : 0u;
        if (direction == 0) {
            // Remember the last 0-branch taken at a discrepancy
            search_ctx.last_zero = search_ctx.id_bit_number;
        }
    }
    if (direction) {
        search_ctx.rom[byte_idx] |= mask;
    } else {
        search_ctx.rom[byte_idx] &= (uint8_t)~mask;
    }
    if (search_ctx.id_bit_number < DS18B20_ROM_BITS) {
        // Merge the direction write with the read of the next id/cmp pair.
        ds18b20_bus_write_then_read(direction);
        search_ctx.phase = DS18B20_SEARCH_WRITE_READ;
    } else {
        ds18b20_bus_write_bit(direction);
        search_ctx.phase = DS18B20_SEARCH_WRITE_DIR;
    }
}

/**
 * @brief Advance the non-blocking device search
 * @return 1 when the search is finished, 0 while still running
 */
uint8_t ds18b20_search_poll(void) {
    if (search_ctx.finished) {
        return 1;
    }

    if (search_ctx.phase == DS18B20_SEARCH_DONE) {
        // No hardware operation is pending at the end of the search: hand the
        // timer back to the measurement state machine exactly once.
        T1.EGR = TIM_EGR(UG);
        __DSB();
        search_ctx.finished = 1;
        return 1;
    }

    // Wait for the currently scheduled hardware operation to complete.
    // This is a non-blocking poll, not a busy-wait.
    if (!ds18b20_bus_done()) {
        return 0;
    }

#ifdef DS18B20_TEST_HARNESS
    // [TEST] Inject a hardware-timed idle-HIGH gap between search slots to
    // measure the DS18B20's tolerance to a delayed next slot (RTOS scenario).
    if (test_gap_us != 0u && search_ctx.phase != DS18B20_SEARCH_GAP) {
        test_gap_pending_phase = (uint8_t)search_ctx.phase;
        search_ctx.phase = DS18B20_SEARCH_GAP;
        start_timer(test_gap_us, 0);
        return 0;
    }
    if (search_ctx.phase == DS18B20_SEARCH_GAP) {
        search_ctx.phase = (search_phase_t)test_gap_pending_phase;
    }
#endif

    switch (search_ctx.phase) {
    case DS18B20_SEARCH_RESET:
        // Reset completed: a presence pulse means at least one device is on
        // the bus, so start a new search pass with the Search ROM command.
        if (!ds18b20_bus_present()) {
            search_ctx.phase = DS18B20_SEARCH_DONE;
            break;
        }
        ds18b20_bus_encode_byte(search_ctx.pulses, DS18B20_SEARCH_ROM);
        ds18b20_bus_write_slots(search_ctx.pulses, DS18B20_BITS_PER_BYTE);
        search_ctx.phase = DS18B20_SEARCH_CMD;
        break;

    case DS18B20_SEARCH_CMD:
        // 0xF0 sent: prepare the first bit iteration and read the id/cmp pair.
        search_ctx.id_bit_number = 1;
        search_ctx.last_zero = 0;
        ds18b20_bus_read_pair();
        search_ctx.phase = DS18B20_SEARCH_READ_PAIR;
        break;

    case DS18B20_SEARCH_READ_PAIR:
        // First id/cmp pair decoded from the plain two-slot read.
#ifdef DS18B20_SEARCH_DIAG
        uart_write_str("[P#1 ");
        uart_write_int(ds18b20_bus_pair_id());
        uart_write_str("/");
        uart_write_int(ds18b20_bus_pair_cmp());
        uart_write_str("] ");
#endif
        ds18b20_search_advance_bit(ds18b20_bus_pair_id(), ds18b20_bus_pair_cmp());
        break;

    case DS18B20_SEARCH_WRITE_READ:
        // The merged operation wrote the direction for the previous bit and
        // captured the id/cmp pair of the current bit into search_edge3.
        search_ctx.id_bit_number++;
#ifdef DS18B20_SEARCH_DIAG
        uart_write_str("[W+R#");
        uart_write_int(search_ctx.id_bit_number);
        uart_write_str(" ");
        for (uint8_t di = 0; di < 3; di++) {
            uart_write_int(search_edge3[di]);
            uart_write_str(":");
        }
        uart_write_str("->");
        uart_write_int((search_edge3[1] <= SHORT_PULSE_MAX) ? 1u : 0u);
        uart_write_str("/");
        uart_write_int((search_edge3[2] <= SHORT_PULSE_MAX) ? 1u : 0u);
        uart_write_str("] ");
#endif
        ds18b20_search_advance_bit(
            (search_edge3[1] <= SHORT_PULSE_MAX) ? 1u : 0u,
            (search_edge3[2] <= SHORT_PULSE_MAX) ? 1u : 0u);
        break;

    case DS18B20_SEARCH_WRITE_DIR:
        // The final (64th) direction bit was written: the ROM is assembled.
        search_ctx.id_bit_number++;
        search_ctx.last_discrepancy = search_ctx.last_zero;
        if (ds18b20_crc8(search_ctx.rom, DS18B20_ROM_BYTES) != 0) {
            search_ctx.phase = DS18B20_SEARCH_DONE;
            break;
        }
        if (search_ctx.rom[0] == DS18B20_FAMILY_CODE) {
            search_ctx.found++;
            if (search_ctx.sink && search_ctx.sink(search_ctx.rom)) {
                search_ctx.phase = DS18B20_SEARCH_DONE;
                break;
            }
            if (search_ctx.found >= search_ctx.max) {
                search_ctx.phase = DS18B20_SEARCH_DONE;
                break;
            }
        }
        if (search_ctx.last_discrepancy == 0) {
            search_ctx.phase = DS18B20_SEARCH_DONE;
            break;
        }
        // Another device may exist - run another search pass.
        ds18b20_bus_reset();
        search_ctx.phase = DS18B20_SEARCH_RESET;
        break;

    default:
        break;
    }

    return 0;
}

/**
 * @brief Number of DS18B20 devices found (valid once the search finished)
 * @return Count of found devices
 */
uint8_t ds18b20_search_count(void) { return search_ctx.found; }

/**
 * @}
 */

/**
 * @defgroup DS18B20_Public_Functions DS18B20 Public Functions
 * @{
 */

/**
 * @brief Initialize DS18B20 driver - configure clocks and peripherals
 */
void ds18b20_init(void) {
    // Enable clocks for required peripherals: GPIOA, TIM1, DMA1
    RC.APB2ENR |= RCC_APB2ENR(IOPAEN, TIM1EN);
    RC.AHBENR |= RCC_AHBENR(DMA1EN);
    // Configure timer prescaler for 1µs resolution (SYSCLK / 1000000 - 1)
    T1.PSC = TIM_PRESCALER;
    T1.EGR = TIM_EGR(UG);
    __DSB();
    T1.BDTR = TIM_BDTR(MOE);
    // Configure PA8 for 1-Wire communication (alternate function open drain)
    PA.CRH |= GPIO_CRH(CNF8_0, CNF8_1, MODE8_1);
}

/**
 * @brief Select which DS18B20 device to measure by its ROM address
 * @param[in] rom Pointer to the 8-byte ROM address (LSB first), or NULL to
 *                return to Skip ROM (broadcast) addressing
 * @note With a non-NULL address, the state machine sends Match ROM (0x55)
 *       plus the device address before each command, so only that device
 *       responds. Pass NULL (or a freshly initialised driver) to keep the
 *       legacy single-sensor Skip ROM behaviour. The address should come from
 *       the non-blocking device search (ds18b20_search_*).
 */
void ds18b20_select(const uint8_t* rom) {
    if (rom == 0) {
        ctx.address_mode = 0;
        return;
    }
    for (uint8_t i = 0; i < DS18B20_ROM_BYTES; i++) {
        ctx.selected_rom[i] = rom[i];
    }
    build_addr_prefix(); // Build the invariant Match ROM prefix once per selection
    ctx.address_mode = 1;
}

/**
 * @brief Check presence and issue command (shared by CONVERT and REQUEST states)
 * @param[in] cmd_byte Command byte to send
 * @param[in] skip_tbl Skip-ROM command table (for broadcast mode)
 * @param[in] next_state State to transition to on success
 */
static void issue_command(uint8_t cmd_byte, const uint8_t* skip_tbl, ds18b20_state_t next_state) {
    if (!check_presence()) {
        ds18b20_complete(DS18B20_TEMP_ERROR_NO_SENSOR);
        start_cycle_pause();
        ctx.current_state = DS18B20_ST_IDLE;
        return;
    }
    if (ctx.address_mode) {
        build_addr_cmd(cmd_byte);
        send_command_n(ctx.addr_cmd, DS18B20_MATCH_SLOTS);
    } else {
        send_command(skip_tbl);
    }
    ctx.current_state = next_state;
}

/**
 * @brief Main state machine function - must be called periodically from main loop
 * @note Non-blocking state machine that advances 1-Wire communication state
 * @note Uses timer update interrupt flag to determine when operations complete
 */
void ds18b20_poll(void) {

    // Check if timer update interrupt occurred (indicates operation completion)
    // This is the non-blocking way to detect when timed operations finish
    if (!(T1.SR & TIM_SR(UIF))) return;
    // Clear timer update interrupt flag
    T1.SR = 0;

    // State machine to manage 1-Wire communication sequence
    switch (ctx.current_state) {
    case DS18B20_ST_IDLE:
        // Initialize union memory (fills with 0xFF pattern)
        ctx.fill_union = (uint64_t)-1;
        // Transition to START state
        ctx.current_state = DS18B20_ST_START;
        /* fallthrough to START state immediately */
        __attribute__((fallthrough));

    case DS18B20_ST_START:
        // Turn on LED to indicate measurement in progress
        ds18b20_busy(!0);
        // Initiate 1-Wire bus reset sequence
        reset_bus();
        // Transition to CONVERT state
        ctx.current_state = DS18B20_ST_CONVERT;
        break;

    case DS18B20_ST_CONVERT:
        issue_command(DS18B20_CONVERT_T, conv_cmd, DS18B20_ST_WAIT);
        break;

    case DS18B20_ST_WAIT:
        // Start timer for conversion wait period (750ms typical)
        wait_conversion();
        ctx.current_state = DS18B20_ST_CONTINUE;
        break;

    case DS18B20_ST_CONTINUE:
        // Initiate second 1-Wire bus reset sequence
        reset_bus();
        ctx.current_state = DS18B20_ST_REQUEST;
        break;

    case DS18B20_ST_REQUEST:
        issue_command(DS18B20_READ_SCRATCHPAD, read_cmd, DS18B20_ST_READ);
        break;

    case DS18B20_ST_READ:
        // Initiate scratchpad data read using timer capture and DMA
        read_data();
        // Transition to DECODE state
        ctx.current_state = DS18B20_ST_DECODE;
        break;

    case 7: // DECODE - Process received data and report temperature
        // Decode captured pulse durations into scratchpad bytes
        decode_scratchpad();
        // Turn off LED to indicate measurement complete
        ds18b20_busy(0);

        // Validate reserved bytes per DS18B20 specification:
        // Byte 5 must be 0xFF, Byte 7 must be 0x10.
        // This catches all-zero, all-0xFF, and bus fault conditions.
        if (ctx.scratchpad[5] != 0xFF || ctx.scratchpad[7] != 0x10) {
            ds18b20_complete(DS18B20_TEMP_ERROR_CRC_FAIL);
            start_cycle_pause();
            ctx.current_state = 0;
            break;
        }

        // Validate CRC and report temperature or error
        if (ctx.scratchpad[DS18B20_SCRATCHPAD_LEN - 1] == check_scratchpad_crc()) {
            // CRC valid - decode and report temperature
            ds18b20_complete(decode_temperature());
        } else {
            // CRC invalid - report error
            ds18b20_complete(DS18B20_TEMP_ERROR_CRC_FAIL);
        }

        // Start inter-measurement pause period
        start_cycle_pause();
        // Return to IDLE state for next measurement cycle
        ctx.current_state = 0;
        break;

    default:
        // Unexpected state - report generic error
        ds18b20_complete(DS18B20_TEMP_ERROR_GENERIC);
        // Return to IDLE state
        ctx.current_state = 0;
        break;
    }
}

/**
 * @}
 */
