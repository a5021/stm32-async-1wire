#include "onewire.h"
#include "macro.h"
#include "stm32f1xx.h"

/**
 * @defgroup ONEWIRE_Private_Constants ONEWIRE Private Constants
 * @{
 */

/** @brief Timer configuration for 1µs resolution (PSC = SYSCLK / 1MHz - 1) */
#ifdef HSI_8MHZ
#define ONEWIRE_TIM_PRESCALER 7 // 8MHz / 8 = 1MHz → 1µs/tick
#else
#define ONEWIRE_TIM_PRESCALER 71 // 72MHz / 72 = 1MHz → 1µs/tick
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
/** @brief Size of edge capture buffer for presence detection */
#define CAPTURE_BUF_SIZE 2
/** @brief DMA channel control bits for 16-bit capture: MINC | PSIZE_0 | EN */
#define ONEWIRE_DMA_CCR_CAPTURE (DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_EN)
/** @brief CRC8 polynomial of the Dallas/Maxim 1-Wire algorithm */
#define ONEWIRE_CRC8_POLY 0x8C

/** @brief Force timer update event and wait for update flag - used for timer initialization */
#define FORCE_UPDATE_EVENT(T)   \
    do {                        \
        (T).EGR = TIM_EGR(UG);  \
        __DSB();                \
        (T).SR &= ~TIM_SR(UIF); \
    } while (0)

/** @} */

/**
 * @defgroup ONEWIRE_Private_Variables ONEWIRE Private Variables
 * @{
 */

/** @brief Edge capture buffer for the merged search write+read operation
 * @note Holds [write-slot edge, id_bit, cmp_bit]. Channel 2 capture runs for
 *       the whole timer pass, so the direction-write rising edge is captured
 *       into entry 0 as well; id/cmp must be decoded from entries 1 and 2. */
static volatile uint16_t search_edge3[3];

/** @brief Read pulse durations reloaded by DMA for the merged search operation
 *        (channel 4 feeds CCR1 from this). Entry 0 is loaded at slot 1's CC4
 *        event and kicks read slots 2-3, entry 1 re-arms the slot-3 kick, and
 *        the trailing 0 is written during slot 3 so the one-pulse timer stops
 *        with the line released to idle HIGH (hardware bus release). */
static const uint8_t search_read_pulse[3] = {ONEWIRE_ONE_PULSE, ONEWIRE_ONE_PULSE, 0};

/** @brief Edge capture buffer used by the search engine for bus resets and
 *         plain id/cmp pair reads (the merged write+read uses search_edge3). */
static volatile uint16_t search_pair_edge[CAPTURE_BUF_SIZE];

/** @brief Search state machine phases */
typedef enum {
    ONEWIRE_SEARCH_RESET, /**< reset scheduled; check presence, send the search command */
    ONEWIRE_SEARCH_CMD, /**< search command sent; prepare first bit iteration */
    ONEWIRE_SEARCH_READ_PAIR, /**< first id/cmp pair read; compute and write direction */
    ONEWIRE_SEARCH_WRITE_READ, /**< merged direction write + next pair read completed */
    ONEWIRE_SEARCH_WRITE_DIR, /**< final direction written; advance bit counters */
    ONEWIRE_SEARCH_DONE, /**< search finished; restore the owner state */
#ifdef DS18B20_TEST_HARNESS
    ONEWIRE_SEARCH_GAP /**< [TEST] timed idle-HIGH gap before the next slot */
#endif
} onewire_search_phase_t;

/**
 * @brief Non-blocking search context
 * @note Holds the loop counters of the search algorithm; the persistent pulse
 *       buffer (pulses) must stay valid across poll calls because the DMA
 *       feeds CCR1 from it asynchronously while the search command is sent.
 */
typedef struct {
    onewire_search_phase_t phase; /**< Current phase of the search state machine */
    uint8_t command; /**< Search command byte (0xF0 Search ROM / 0xEC Alarm Search) */
    uint8_t family; /**< 1-Wire family code to accept, or 0 to accept every family */
    uint8_t rom[ONEWIRE_ROM_BYTES]; /**< ROM being assembled (bit by bit) */
    uint8_t pulses[ONEWIRE_BITS_PER_BYTE + 1]; /**< Pulse buffer for the search command (+ trailing 0 for hardware bus release) */
    uint8_t id_bit_number; /**< Current bit position (1..64) */
    uint16_t last_discrepancy; /**< Last discrepancy point (Maxim algorithm) */
    uint16_t last_zero; /**< Last position where the '0' branch was taken */
    uint8_t found; /**< Number of accepted devices found */
    uint8_t max; /**< Maximum number of devices to report */
    uint8_t finished; /**< 1 once the search has completed */
    onewire_search_sink_t sink; /**< Per-device callback */
} onewire_search_ctx_t;

/** @brief Global search context instance */
static onewire_search_ctx_t search_ctx;

#ifdef DS18B20_TEST_HARNESS
/** @brief [TEST] Idle-HIGH gap (µs) inserted after every completed search
 *         operation before scheduling the next one (0 = no gap). */
static uint16_t test_gap_us;
/** @brief [TEST] Search phase to resume after the gap wait completes */
static uint8_t test_gap_pending_phase;
#endif

/** @} */

/**
 * @defgroup ONEWIRE_Private_Functions ONEWIRE Private Functions
 * @{
 */

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
    D13.CCR = ONEWIRE_DMA_CCR_CAPTURE | ((width == 16) ? DMA_CCR_MSIZE_0 : 0);
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Transmit a command sequence of arbitrary length using DMA
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
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND;
    T1.CCR1 = cmd[0];
    T1.CCR4 = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE;
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

/** @} */

/**
 * @defgroup ONEWIRE_Bus_Impl ONEWIRE Non-Blocking Bus Primitives
 * @{
 */

void onewire_init(void) {
    // No search running after init: lets the slave driver own the timer until
    // the application starts a search.
    search_ctx.finished = 1;
    // Enable clocks for required peripherals: GPIOA, TIM1, DMA1
    RC.APB2ENR |= RCC_APB2ENR(IOPAEN, TIM1EN);
    RC.AHBENR |= RCC_AHBENR(DMA1EN);
    // Configure timer prescaler for 1µs resolution (SYSCLK / 1000000 - 1)
    T1.PSC = ONEWIRE_TIM_PRESCALER;
    T1.EGR = TIM_EGR(UG);
    __DSB();
    T1.BDTR = TIM_BDTR(MOE);
    // Configure PA8 for 1-Wire communication (alternate function open drain)
    PA.CRH |= GPIO_CRH(CNF8_0, CNF8_1, MODE8_1);
}

uint8_t onewire_bus_done(void) {
    if (T1.SR & TIM_SR(UIF)) {
        // No software bus release needed: every operation now returns the line
        // to idle HIGH in hardware. DMA-fed writes (send_command_n,
        // write_then_read) append a trailing 0 to the CCR1 feed, and the
        // direct-write/capture operations (reset, read, single slot) use an
        // OC1PE preload of 0 — both applied exactly when the one-pulse timer
        // stops. The bus therefore idles HIGH between slots regardless of how
        // long software takes to poll, and the next write pulse produces a
        // clean falling edge the 1-Wire slaves re-sync to.
        T1.SR = 0;
        return 1u;
    }
    return 0u;
}

void onewire_reset(volatile uint16_t* edge_out) {
    T1.RCR = 0;
    T1.ARR = RESET_TIMEOUT;
    T1.CCR1 = RESET_PULSE_DURATION;
    arm_capture((volatile void*)edge_out, CAPTURE_BUF_SIZE, 16);
}

uint8_t onewire_present(const volatile uint16_t* edge) {
    uint16_t reset = edge[0];
    uint16_t presence = edge[1];
    // Validate that reset pulse duration is within specification
    // and presence pulse timing indicates a responding device
    return (reset >= RESET_PULSE_MIN) && (reset <= RESET_PULSE_MAX) &&
           (presence >= PRESENCE_PULSE_MIN) && (presence <= PRESENCE_PULSE_MAX);
}

void onewire_start_timer(uint16_t arr, uint8_t rcr) {
    T1.ARR = arr;
    T1.RCR = rcr;
    // Force update event to load new values
    FORCE_UPDATE_EVENT(T1);
    // Start timer in One Pulse Mode (OPM) - runs once then stops
    T1.CR1 = TIM_CR1(OPM, CEN);
}

void onewire_write_slots(const uint8_t* pulses, uint16_t slots) {
    if (slots == 1) {
        // Single slot: no DMA needed, avoids a zero-length DMA transaction
        T1.RCR = 0; // Single slot, no repetition
        T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND; // Total bit slot time
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

void onewire_write_bit(uint8_t bit) {
    uint8_t pulse = bit ? (uint8_t)ONEWIRE_ONE_PULSE : (uint8_t)ONEWIRE_ZERO_PULSE;
    onewire_write_slots(&pulse, 1);
}

void onewire_read_pair(volatile uint16_t* edge_out) {
    // Configure timer for a two-slot read (id_bit, cmp_bit)
    T1.RCR = 1; // Two read slots, then a single update event
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND; // Total bit slot time
    T1.CCR1 = ONEWIRE_ONE_PULSE; // Read pulse duration
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
    D13.CMAR = (uint32_t)edge_out; // DMA destination: edge timestamp buffer
    D13.CNDTR = 2; // Number of transfers (id_bit, cmp_bit)
    D13.CCR = DMA_CCR(MINC, PSIZE_0, MSIZE_0, EN); // Enable DMA with memory increment
    T1.CR1 = TIM_CR1(OPM, CEN); // Start timer in one-pulse mode
}

void onewire_pair_bits(const volatile uint16_t* edge, uint8_t* id_bit, uint8_t* cmp_bit) {
    *id_bit = onewire_bit_from_pulse(edge[0]);
    *cmp_bit = onewire_bit_from_pulse(edge[1]);
}

void onewire_write_then_read(uint8_t bit) {
    const uint8_t write_pulse = bit ? (uint8_t)ONEWIRE_ONE_PULSE : (uint8_t)ONEWIRE_ZERO_PULSE;
    T1.RCR = 2; // Three slots, then a single update event
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND; // Total bit slot time
    // Arm the direction pulse first. The bus was released idle-high by
    // onewire_bus_done(), so this write produces the single clean falling edge
    // the devices re-sync their slot timer to; the pulse then holds the bus low
    // through the whole (fast) setup. Holding it from the top instead of arming
    // it right before CEN means the CC2 capture is armed while the bus is low,
    // so the open-drain RC rise can never be mistaken for a slot edge.
    T1.CCR1 = write_pulse; // Slot 1 write pulse encodes the direction bit
    T1.CCR4 = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE; // End-of-slot reload trigger
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

void onewire_read_data(volatile uint8_t* dst, uint8_t bytes) {
    const uint16_t bits = (uint16_t)bytes * ONEWIRE_BITS_PER_BYTE;
    T1.RCR = bits - 1;
    T1.ARR = ONEWIRE_ONE_PULSE + ONEWIRE_ZERO_PULSE + ONEWIRE_GUARD_BAND;
    T1.CCR1 = ONEWIRE_ONE_PULSE;
    arm_capture((volatile void*)dst, bits, 8);
}

void onewire_decode_pulses(uint8_t* dst, const volatile uint8_t* pulse, uint8_t nbytes) {
    for (uint8_t byte = 0; byte < nbytes; byte++) {
        uint8_t value = 0;
        for (uint8_t bit = 0; bit < ONEWIRE_BITS_PER_BYTE; bit++) {
            value |= (uint8_t)(onewire_bit_from_pulse(pulse[byte * ONEWIRE_BITS_PER_BYTE + bit]) << bit);
        }
        dst[byte] = value;
    }
}

void onewire_encode_byte(uint8_t* out, uint8_t byte) {
    for (uint8_t i = 0; i < ONEWIRE_BITS_PER_BYTE; i++) {
        out[i] = (byte & (1u << i)) ? (uint8_t)ONEWIRE_ONE_PULSE : (uint8_t)ONEWIRE_ZERO_PULSE;
    }
}

uint8_t onewire_crc8(const uint8_t* data, uint8_t len) {
    uint8_t crc = 0;
    // Process each byte in the buffer
    for (uint8_t i = 0; i < len; i++) {
        uint8_t inByte = data[i];
        // Process each bit in the byte using Dallas/Maxim CRC8 algorithm
        for (uint8_t b = 0; b < ONEWIRE_BITS_PER_BYTE; b++) {
            uint8_t mix = (crc ^ inByte) & 0x01;
            crc >>= 1;
            if (mix) crc ^= ONEWIRE_CRC8_POLY;
            inByte >>= 1;
        }
    }
    return crc;
}

/** @} */

/**
 * @defgroup ONEWIRE_Search_Impl ONEWIRE Generic Search ROM Engine
 * @{
 */

/**
 * @brief Process one decoded id/cmp pair: pick a direction, update the ROM,
 *        and schedule the next hardware operation
 * @param[in] id_bit Id bit of the current position
 * @param[in] cmp_bit Complement bit of the current position
 * @note For all but the last bit the direction write is merged with the read
 *       of the next pair (ONEWIRE_SEARCH_WRITE_READ); the 64th bit is written
 *       alone so the device can be finalized.
 */
static void onewire_search_advance_bit(uint8_t id_bit, uint8_t cmp_bit) {
    const uint8_t byte_idx = (search_ctx.id_bit_number - 1) / ONEWIRE_BITS_PER_BYTE;
    const uint8_t mask = (uint8_t)(1u << ((search_ctx.id_bit_number - 1) % ONEWIRE_BITS_PER_BYTE));
    uint8_t direction;

    if (id_bit && cmp_bit) {
        // No device follows this path - search tree exhausted
        search_ctx.phase = ONEWIRE_SEARCH_DONE;
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
    if (search_ctx.id_bit_number < ONEWIRE_ROM_BITS) {
        // Merge the direction write with the read of the next id/cmp pair.
        onewire_write_then_read(direction);
        search_ctx.phase = ONEWIRE_SEARCH_WRITE_READ;
    } else {
        onewire_write_bit(direction);
        search_ctx.phase = ONEWIRE_SEARCH_WRITE_DIR;
    }
}

void onewire_search_start(onewire_search_sink_t sink, uint8_t max_devices,
                          uint8_t command, uint8_t family) {
    if (!search_ctx.finished) {
        return; // a search is already running
    }
    for (uint8_t i = 0; i < ONEWIRE_ROM_BYTES; i++) {
        search_ctx.rom[i] = 0;
    }
    // Trailing zero consumed by the CCR1-feed DMA's final transfer: this is the
    // hardware bus release after the search command (see send_command_n).
    search_ctx.pulses[ONEWIRE_BITS_PER_BYTE] = 0;
    search_ctx.sink = sink;
    search_ctx.max = max_devices;
    search_ctx.found = 0;
    search_ctx.finished = 0;
    search_ctx.last_discrepancy = 0;
    search_ctx.command = command;
    search_ctx.family = family;
    if (max_devices == 0) {
        search_ctx.phase = ONEWIRE_SEARCH_DONE;
        return;
    }
    search_ctx.phase = ONEWIRE_SEARCH_RESET;
    onewire_reset(search_pair_edge); // Schedule the first hardware operation
}

uint8_t onewire_search_poll(void) {
    if (search_ctx.finished) {
        return 1;
    }

    if (search_ctx.phase == ONEWIRE_SEARCH_DONE) {
        // No hardware operation is pending at the end of the search: hand the
        // timer back to the owner exactly once.
        T1.EGR = TIM_EGR(UG);
        __DSB();
        search_ctx.finished = 1;
        return 1;
    }

    // Wait for the currently scheduled hardware operation to complete.
    // This is a non-blocking poll, not a busy-wait.
    if (!onewire_bus_done()) {
        return 0;
    }

#ifdef DS18B20_TEST_HARNESS
    // [TEST] Inject a hardware-timed idle-HIGH gap between search slots to
    // measure a 1-Wire slave's tolerance to a delayed next slot (RTOS scenario).
    if (test_gap_us != 0u && search_ctx.phase != ONEWIRE_SEARCH_GAP) {
        test_gap_pending_phase = (uint8_t)search_ctx.phase;
        search_ctx.phase = ONEWIRE_SEARCH_GAP;
        onewire_start_timer(test_gap_us, 0);
        return 0;
    }
    if (search_ctx.phase == ONEWIRE_SEARCH_GAP) {
        search_ctx.phase = (onewire_search_phase_t)test_gap_pending_phase;
    }
#endif

    switch (search_ctx.phase) {
    case ONEWIRE_SEARCH_RESET:
        // Reset completed: a presence pulse means at least one device is on
        // the bus, so start a new search pass with the search command
        // (0xF0 Search ROM / 0xEC Alarm Search).
        if (!onewire_present(search_pair_edge)) {
            search_ctx.phase = ONEWIRE_SEARCH_DONE;
            break;
        }
        onewire_encode_byte(search_ctx.pulses, search_ctx.command);
        onewire_write_slots(search_ctx.pulses, ONEWIRE_BITS_PER_BYTE);
        search_ctx.phase = ONEWIRE_SEARCH_CMD;
        break;

    case ONEWIRE_SEARCH_CMD:
        // Search command sent: prepare the first bit iteration and read the
        // id/cmp pair.
        search_ctx.id_bit_number = 1;
        search_ctx.last_zero = 0;
        onewire_read_pair(search_pair_edge);
        search_ctx.phase = ONEWIRE_SEARCH_READ_PAIR;
        break;

    case ONEWIRE_SEARCH_READ_PAIR:
        // First id/cmp pair decoded from the plain two-slot read.
        {
            uint8_t id_bit;
            uint8_t cmp_bit;
            onewire_pair_bits(search_pair_edge, &id_bit, &cmp_bit);
            onewire_search_advance_bit(id_bit, cmp_bit);
        }
        break;

    case ONEWIRE_SEARCH_WRITE_READ:
        // The merged operation wrote the direction for the previous bit and
        // captured the id/cmp pair of the current bit into search_edge3.
        search_ctx.id_bit_number++;
        onewire_search_advance_bit(
            onewire_bit_from_pulse(search_edge3[1]),
            onewire_bit_from_pulse(search_edge3[2]));
        break;

    case ONEWIRE_SEARCH_WRITE_DIR:
        // The final (64th) direction bit was written: the ROM is assembled.
        search_ctx.id_bit_number++;
        search_ctx.last_discrepancy = search_ctx.last_zero;
        if (onewire_crc8(search_ctx.rom, ONEWIRE_ROM_BYTES) != 0) {
            search_ctx.phase = ONEWIRE_SEARCH_DONE;
            break;
        }
        // The family filter decides which devices are accepted: only accepted
        // devices increment the found counter and reach the sink. The sink may
        // stop the search early with a non-zero return value.
        if (search_ctx.family == 0u || search_ctx.rom[0] == search_ctx.family) {
            search_ctx.found++;
            if (search_ctx.sink && search_ctx.sink(search_ctx.rom)) {
                search_ctx.phase = ONEWIRE_SEARCH_DONE;
                break;
            }
            if (search_ctx.found >= search_ctx.max) {
                search_ctx.phase = ONEWIRE_SEARCH_DONE;
                break;
            }
        }
        if (search_ctx.last_discrepancy == 0) {
            search_ctx.phase = ONEWIRE_SEARCH_DONE;
            break;
        }
        // Another device may exist - run another search pass.
        onewire_reset(search_pair_edge);
        search_ctx.phase = ONEWIRE_SEARCH_RESET;
        break;

    case ONEWIRE_SEARCH_DONE:
#ifdef DS18B20_TEST_HARNESS
    case ONEWIRE_SEARCH_GAP:
#endif
        // DONE and GAP are handled before the switch (see above); keep as a
        // no-op so -Wswitch-enum stays satisfied.
        break;

    default:
        break;
    }

    return 0;
}

uint8_t onewire_search_count(void) { return search_ctx.found; }

uint8_t onewire_search_active(void) { return (uint8_t)!search_ctx.finished; }

/** @} */

#ifdef DS18B20_TEST_HARNESS
void onewire_test_set_gap_us(uint16_t us) { test_gap_us = us; }
#endif
