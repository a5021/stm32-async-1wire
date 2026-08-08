#include "ds18b20.h"
#include "macro.h"
#include "stm32f1xx.h"

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
/** @brief Total length of DS18B20 scratchpad in bytes */
#define DS18B20_SCRATCHPAD_LEN 9
/** @brief Total number of bits in DS18B20 scratchpad */
#define DS18B20_SCRATCHPAD_BITS (DS18B20_SCRATCHPAD_LEN * DS18B20_BITS_PER_BYTE)
/** @brief Total number of bits in a device ROM address */
#define DS18B20_ROM_BITS (DS18B20_ROM_BYTES * DS18B20_BITS_PER_BYTE)
/** @brief Total slots for Match ROM + 8-byte ROM + command */
#define DS18B20_MATCH_SLOTS ((DS18B20_ROM_BYTES + 2) * DS18B20_BITS_PER_BYTE)
/** @brief Slots for the invariant Match ROM + 8-byte ROM prefix (built on select) */
#define DS18B20_PREFIX_SLOTS ((DS18B20_ROM_BYTES + 1) * DS18B20_BITS_PER_BYTE)
/** @brief Threshold to distinguish short/long pulses (10µs) */
#define SHORT_PULSE_MAX 0x0A
/** @brief Number of DMA transfers for command transmission */
#define DS18B20_DMA_TRANSFERS 16
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
static const uint8_t conv_cmd[] = {BYTE_TO_PULSES(0xCC), BYTE_TO_PULSES(0x44)};

/** @brief DS18B20 Read Scratchpad command sequence in pulse duration format */
static const uint8_t read_cmd[] = {BYTE_TO_PULSES(0xCC), BYTE_TO_PULSES(0xBE)};

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
        volatile uint16_t edge[36]; /**< Edge timestamps for presence detection */
        volatile uint8_t pulse[72]; /**< Pulse durations for data decoding */
        uint8_t scratchpad[9]; /**< Sensor scratchpad data */
        uint64_t fill_union; /**< Utility field for filling the union */
    };
    uint8_t current_state; /**< Current state of the state machine */
    uint8_t address_mode; /**< 0 = Skip ROM (all devices), non-zero = Match ROM */
    uint8_t selected_rom[DS18B20_ROM_BYTES]; /**< ROM of the selected device */
    uint8_t addr_cmd[DS18B20_MATCH_SLOTS]; /**< Pulse buffer for Match ROM command */
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
    // Validate that reset pulse duration is within specification
    // and presence pulse timing indicates a responding device
    return (ctx.edge[0] >= RESET_PULSE_MIN) && (ctx.edge[0] <= RESET_PULSE_MAX) &&
           (ctx.edge[1] >= PRESENCE_PULSE_MIN) && (ctx.edge[1] <= PRESENCE_PULSE_MAX);
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
 * @brief Initialize 1-Wire bus reset sequence using timer and DMA
 */
__STATIC_FORCEINLINE void reset_bus(void) {
    // Configure timer for reset pulse generation (480µs low)
    T1.ARR = RESET_TIMEOUT; // Total reset slot time (960µs)
    T1.CCR1 = RESET_PULSE_DURATION; // Reset pulse duration (480µs)
    // Configure channel 1 for output compare (drive bus low)
    // Configure channel 2 for input capture (detect presence pulse)
    T1.CCMR1 = TIM_CCMR1(OC1M_0, OC1M_1, OC1M_2, OC1PE, CC2S_1, IC2F_0, IC2F_1, IC2F_2);
    T1.CCER = TIM_CCER(CC1E, CC2E); // Enable both channels
    T1.RCR = 0; // No repetition
    // Configure DMA to capture presence pulse edge timestamps
    D13.CCR = 0; // Clear DMA configuration
    D13.CPAR = (uint32_t)&T1.CCR2; // DMA destination: timer capture register
    D13.CMAR = (uint32_t)ctx.edge; // DMA source: edge timestamp buffer
    D13.CNDTR = CAPTURE_BUF_SIZE; // Number of transfers (2 edges)
    D13.CCR = DMA_CCR(MINC, PSIZE_0, MSIZE_0, EN); // Enable DMA with memory increment
    // Force timer update to load configuration
    FORCE_UPDATE_EVENT(T1);
    T1.CCR1 = 0; // Clear output compare value
    T1.DIER = TIM_DIER(CC2DE); // Enable DMA request on capture
    T1.CR1 = TIM_CR1(OPM, CEN); // Start timer in one-pulse mode
}

/**
 * @brief Transmit a command sequence of arbitrary length to DS18B20 using DMA
 * @param[in] cmd Pointer to command sequence in pulse duration format
 * @param[in] slots Number of bit slots (bits) to transmit
 * @note Non-blocking - configures hardware to transmit command automatically.
 *       The buffer must hold `slots` pulse values; the first one is written
 *       to CCR1 directly, the remaining `slots-1` are fed via DMA.
 */
__STATIC_FORCEINLINE void send_command_n(const uint8_t* cmd, uint16_t slots) {
    // Configure timer for command transmission using DMA
    T1.RCR = slots - 1; // Number of repetitions (one slot per transfer)
    T1.ARR = ONE_PULSE + ZERO_PULSE + GUARD_BAND; // Total bit slot time
    T1.CCR1 = cmd[0]; // First pulse duration
    T1.CCR4 = ONE_PULSE + ZERO_PULSE; // Update trigger time
    // Configure channel 1 for output compare mode
    T1.CCMR1 = TIM_CCMR1(OC1M_0, OC1M_1, OC1M_2);
    T1.CCER = TIM_CCER(CC1E); // Enable output compare
    T1.DIER = TIM_DIER(CC4DE); // Enable DMA request on update
    // Force timer update to load configuration
    FORCE_UPDATE_EVENT(T1);
    // Configure DMA to transmit command pulse sequence
    D14.CCR = 0; // Clear DMA configuration
    D14.CPAR = (uint32_t)&T1.CCR1; // DMA destination: output compare register
    D14.CMAR = (uint32_t)&cmd[1]; // DMA source: command data (skip first byte)
    // One transfer per timer period, minus the first period whose CCR1 value
    // was preloaded above. The timer still generates 'slots' DMA requests,
    // but the DMA channel auto-disables after the final transfer (NDTR hits 0)
    // and the remaining request is ignored.
    D14.CNDTR = slots - 1; // Number of CCR1 updates needed
    D14.CCR = DMA_CCR(DIR, MINC, PSIZE_0, EN); // Enable DMA with memory increment
    T1.CR1 = TIM_CR1(OPM, CEN); // Start timer in one-pulse mode
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
    // Configure timer for data reading with input capture
    T1.RCR = DS18B20_SCRATCHPAD_BITS - 1; // Number of repetitions (72 bits)
    T1.ARR = ONE_PULSE + ZERO_PULSE + GUARD_BAND; // Total bit slot time
    T1.CCR1 = ONE_PULSE; // Read pulse duration (ONE_PULSE µs)
    // Configure channel 1 for output compare (generate read pulse)
    // Configure channel 2 for input capture (measure return pulse durations)
    T1.CCMR1 = TIM_CCMR1(OC1M_0, OC1M_1, OC1M_2, OC1PE, CC2S_1, IC2F_0, IC2F_1, IC2F_2);
    T1.CCER = TIM_CCER(CC1E, CC2E); // Enable both channels
    T1.DIER = TIM_DIER(CC2DE); // Enable DMA request on capture
    // Force timer update to load configuration
    FORCE_UPDATE_EVENT(T1);
    T1.CCR1 = 0; // Clear output compare value
    // Configure DMA to capture pulse durations into pulse buffer
    D13.CCR = 0; // Clear DMA configuration
    D13.CPAR = (uint32_t)&T1.CCR2; // DMA destination: capture register
    D13.CMAR = (uint32_t)ctx.pulse; // DMA source: pulse duration buffer
    D13.CNDTR = DS18B20_SCRATCHPAD_BITS; // Number of transfers (72 bits)
    D13.CCR = DMA_CCR(MINC, PSIZE_0, EN); // Enable DMA with memory increment
    T1.CR1 = TIM_CR1(OPM, CEN); // Start timer in one-pulse mode
}

/**
 * @}
 */

/**
 * @defgroup DS18B20_LowLevel DS18B20 Low-Level Blocking 1-Wire Primitives
 * @brief Blocking primitives for custom 1-Wire protocols (e.g., device search).
 * @note These busy-wait on the timer update flag. They reuse the same
 *       hardware-timed 1-Wire primitives as the state machine but wait for
 *       each operation synchronously. They MUST NOT be called while the
 *       non-blocking state machine is active (between ds18b20_init() and
 *       the first ds18b20_poll()). After using these, call ds18b20_restore()
 *       before starting ds18b20_poll().
 * @{
 */

/**
 * @brief Wait for the timer to finish its current one-shot operation
 * @note Busy-waits on the update flag. Used only by the low-level primitives.
 */
static void wait_timer_done(void) {
    __DSB();
    while (!(T1.SR & TIM_SR(UIF))) {
    }
    T1.SR = 0;
}

/**
 * @brief Write one bit to the 1-Wire bus as a single hardware-timed slot
 * @param[in] bit 1 = short low pulse (~5µs), 0 = long low pulse (~60µs)
 * @note Blocking: waits for the slot to finish before returning.
 */
void ds18b20_write_bit(uint8_t bit) {
    T1.RCR = 0; // Single slot, no repetition
    T1.ARR = ONE_PULSE + ZERO_PULSE + GUARD_BAND; // Total bit slot time
    T1.CCR1 = bit ? ONE_PULSE : ZERO_PULSE; // Pulse duration encodes the bit
    // Configure channel 1 for output compare (drive bus low during the pulse)
    T1.CCMR1 = TIM_CCMR1(OC1M_0, OC1M_1, OC1M_2);
    T1.CCER = TIM_CCER(CC1E); // Enable output compare
    T1.DIER = 0; // No DMA for a single bit slot
    FORCE_UPDATE_EVENT(T1);
    T1.CR1 = TIM_CR1(OPM, CEN); // Start timer in one-pulse mode
    wait_timer_done();
}

/**
 * @brief Write one byte to the 1-Wire bus, LSB first
 * @param[in] byte Byte value to transmit
 * @note Blocking: 8 sequential single-slot writes, waits between slots.
 */
void ds18b20_write_byte(uint8_t byte) {
    for (uint8_t i = 0; i < DS18B20_BITS_PER_BYTE; i++) {
        ds18b20_write_bit((byte >> i) & 0x01);
    }
}

/**
 * @brief Read one bit from the 1-Wire bus as a single hardware-timed slot
 * @return The bit value read (0 or 1)
 * @note Blocking: waits for the slot to finish before returning.
 */
uint8_t ds18b20_read_bit(void) {
    volatile uint8_t sample = 0;
    T1.RCR = 0; // Single slot, no repetition
    T1.ARR = ONE_PULSE + ZERO_PULSE + GUARD_BAND; // Total bit slot time
    T1.CCR1 = ONE_PULSE; // Read pulse duration (ONE_PULSE µs)
    // Configure channel 1 for output compare (generate read pulse)
    // Configure channel 2 for input capture (measure return pulse duration)
    T1.CCMR1 = TIM_CCMR1(OC1M_0, OC1M_1, OC1M_2, OC1PE, CC2S_1, IC2F_0, IC2F_1, IC2F_2);
    T1.CCER = TIM_CCER(CC1E, CC2E); // Enable both channels
    T1.DIER = TIM_DIER(CC2DE); // Enable DMA request on capture
    FORCE_UPDATE_EVENT(T1);
    T1.CCR1 = 0; // Clear output compare value
    // Configure DMA to capture pulse duration into the local buffer
    D13.CCR = 0; // Clear DMA configuration
    D13.CPAR = (uint32_t)&T1.CCR2; // DMA source: capture register
    D13.CMAR = (uint32_t)&sample; // DMA destination: local sample buffer
    D13.CNDTR = 1; // Number of transfers (1 bit)
    D13.CCR = DMA_CCR(MINC, PSIZE_0, EN); // Enable DMA with memory increment
    T1.CR1 = TIM_CR1(OPM, CEN); // Start timer in one-pulse mode
    wait_timer_done();
    // Decode using the same threshold as decode_scratchpad()
    return (sample <= SHORT_PULSE_MAX) ? 1u : 0u;
}

/**
 * @brief Read one byte from the 1-Wire bus, LSB first
 * @return The byte value read
 * @note Blocking: 8 sequential single-slot reads, waits between slots.
 */
uint8_t ds18b20_read_byte(void) {
    uint8_t byte = 0;
    for (uint8_t i = 0; i < DS18B20_BITS_PER_BYTE; i++) {
        if (ds18b20_read_bit()) {
            byte |= (1u << i);
        }
    }
    return byte;
}

/**
 * @brief Perform a blocking 1-Wire reset and check for a presence pulse
 * @return 1 if at least one device answered the reset, 0 otherwise
 */
uint8_t ds18b20_reset(void) {
    reset_bus();
    wait_timer_done();
    return check_presence();
}

/**
 * @brief Restore the non-blocking state machine after using low-level primitives
 * @note Call this after finishing low-level operations and before starting
 *       ds18b20_poll(). It re-primes the state machine so the first poll()
 *       begins a measurement cycle.
 */
void ds18b20_restore(void) {
    T1.EGR = TIM_EGR(UG);
    __DSB();
}

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
 *       a bus search using the low-level primitives.
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
    case 0: // IDLE - Initialize for new measurement cycle
        // Initialize union memory (fills with 0xFF pattern)
        ctx.fill_union = (uint64_t)-1;
        // Transition to START state
        ctx.current_state = 1;
        /* fallthrough to START state immediately */
        /* fallthrough  */

    case 1: // START - Begin measurement cycle, turn on LED
        // Turn on LED to indicate measurement in progress
        ds18b20_busy(!0);
        // Initiate 1-Wire bus reset sequence
        reset_bus();
        // Transition to CONVERT state
        ctx.current_state = 2;
        break;

    case 2: // CONVERT - Check presence and send convert command
        // Verify DS18B20 presence using captured edge timestamps
        if (check_presence()) {
            // Device present - send temperature conversion command,
            // addressing all devices (Skip ROM) or the selected one (Match ROM)
            if (ctx.address_mode) {
                build_addr_cmd(DS18B20_CONVERT_T);
                send_command_n(ctx.addr_cmd, DS18B20_MATCH_SLOTS);
            } else {
                send_command(conv_cmd);
            }
            // Transition to WAIT state to allow conversion time
            ctx.current_state = 3;
        } else {
            // No device present - report error and pause
            ds18b20_complete(DS18B20_TEMP_ERROR_NO_SENSOR);
            // Start inter-measurement pause
            start_cycle_pause();
            // Return to IDLE state
            ctx.current_state = 0;
        }
        break;

    case 3: // WAIT - Wait for temperature conversion to complete
        // Start timer for conversion wait period (750ms typical)
        wait_conversion();
        // Transition to CONTINUE state
        ctx.current_state = 4;
        break;

    case 4: // CONTINUE - Prepare for data readback
        // Initiate second 1-Wire bus reset sequence
        reset_bus();
        // Transition to REQUEST state
        ctx.current_state = 5;
        break;

    case 5: // REQUEST - Check presence and send read command
        // Verify DS18B20 presence again
        if (check_presence()) {
            // Device present - send read scratchpad command,
            // addressing all devices (Skip ROM) or the selected one (Match ROM)
            if (ctx.address_mode) {
                build_addr_cmd(DS18B20_READ_SCRATCHPAD);
                send_command_n(ctx.addr_cmd, DS18B20_MATCH_SLOTS);
            } else {
                send_command(read_cmd);
            }
            // Transition to READ state
            ctx.current_state = 6;
        } else {
            // No device present - report error and pause
            ds18b20_complete(DS18B20_TEMP_ERROR_NO_SENSOR);
            // Start inter-measurement pause
            start_cycle_pause();
            // Return to IDLE state
            ctx.current_state = 0;
        }
        break;

    case 6: // READ - Read scratchpad data from sensor
        // Initiate scratchpad data read using timer capture and DMA
        read_data();
        // Transition to DECODE state
        ctx.current_state = 7;
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
        if (ctx.scratchpad[8] == check_scratchpad_crc()) {
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
