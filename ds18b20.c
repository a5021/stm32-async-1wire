#include "ds18b20.h"
#include "stm32f1xx.h"
#include "macro.h"

/**
 * @defgroup DS18B20_Private_Types DS18B20 Private Types
 * @{
 */

/**
 * @brief DS18B20 driver context structure using union for memory efficiency
 * @note Different stages of communication use the same memory for different purposes
 */
typedef struct {
    union {
        volatile uint16_t edge[36];       /**< Edge timestamps for presence detection */
        volatile uint8_t  pulse[72];      /**< Pulse durations for data decoding */
        uint8_t           scratchpad[9];  /**< Sensor scratchpad data */
        uint64_t          fill_union;     /**< Utility field for filling the union */
    };
    uint8_t               current_state;  /**< Current state of the state machine */
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
 * @defgroup DS18B20_Private_Constants DS18B20 Private Constants
 * @{
 */

/** @brief Timer configuration for 1µs resolution (72MHz system clock / 72 = 1MHz) */
#define TIM_PRESCALER           71        
/** @brief Minimum reset pulse duration in microseconds */
#define RESET_PULSE_MIN       480U        
/** @brief Maximum reset pulse duration in microseconds */
#define RESET_PULSE_MAX       540U        
/** @brief Minimum presence pulse positive width in microseconds */
#define POSITIVE_WIDTH_MIN     15U        
/** @brief Maximum presence pulse positive width in microseconds */
#define POSITIVE_WIDTH_MAX     60U        
/** @brief Minimum presence pulse negative width in microseconds */
#define NEGATIVE_WIDTH_MIN     60U        
/** @brief Maximum presence pulse negative width in microseconds */
#define NEGATIVE_WIDTH_MAX    240U        
/** @brief Calculated minimum presence pulse timing */
#define PRESENCE_PULSE_MIN   (RESET_PULSE_MIN + POSITIVE_WIDTH_MIN + NEGATIVE_WIDTH_MIN)
/** @brief Calculated maximum presence pulse timing */
#define PRESENCE_PULSE_MAX   (RESET_PULSE_MAX + POSITIVE_WIDTH_MAX + NEGATIVE_WIDTH_MAX)
/** @brief Duration to drive bus low during reset in microseconds */
#define RESET_PULSE_DURATION   RESET_PULSE_MIN  
/** @brief Total reset timeslot timeout in microseconds */
#define RESET_TIMEOUT          (RESET_PULSE_MIN * 2) 
/** @brief CRC8 polynomial for DS18B20 scratchpad validation (Dallas/Maxim algorithm) */
#define DS18B20_CRC8_POLY     0x8C        
/** @brief Number of bytes to include in CRC calculation */
#define DS18B20_CRC8_BYTES       8        
/** @brief Size of edge capture buffer for presence detection */
#define CAPTURE_BUF_SIZE         2        
/** @brief Duration of '1' bit pulse in microseconds */
#define ONE_PULSE                1        
/** @brief Duration of '0' bit pulse in microseconds */
#define ZERO_PULSE              60        
/** @brief Total length of DS18B20 scratchpad in bytes */
#define DS18B20_SCRATCHPAD_LEN   9        
/** @brief Standard 8 bits per byte */
#define DS18B20_BITS_PER_BYTE    8        
/** @brief Total number of bits in DS18B20 scratchpad */
#define DS18B20_SCRATCHPAD_BITS (DS18B20_SCRATCHPAD_LEN * DS18B20_BITS_PER_BYTE)
/** @brief Threshold to distinguish short/long pulses (10µs) */
#define SHORT_PULSE_MAX       0x0A        
/** @brief Number of DMA transfers for command transmission */
#define DS18B20_DMA_TRANSFERS   16        

/**
 * @brief Convert byte bit to pulse duration (1µs for '1', 60µs for '0')
 * @param B Byte value
 * @param N Bit position (0-7)
 * @return Pulse duration in microseconds
 */
#define B2P(B, N) (B) & (1 << N) ? ONE_PULSE : ZERO_PULSE

/**
 * @brief Convert entire byte to sequence of pulse durations for transmission
 * @param B Byte value to convert
 */
#define BYTE_TO_PULSES(B) \
    B2P(B, 0), B2P(B, 1), B2P(B, 2), B2P(B, 3),\
    B2P(B, 4), B2P(B, 5), B2P(B, 6), B2P(B, 7)

/** @brief DS18B20 Convert T command sequence in pulse duration format */
static const uint8_t conv_cmd[] = { BYTE_TO_PULSES(0xCC), BYTE_TO_PULSES(0x44), 0 };

/** @brief DS18B20 Read Scratchpad command sequence in pulse duration format */
static const uint8_t read_cmd[] = { BYTE_TO_PULSES(0xCC), BYTE_TO_PULSES(0xBE), 0 };

/**
 * @brief Force timer update event and wait for update flag - used for timer initialization
 * @param T Timer register structure
 */
#define FORCE_UPDATE_EVENT(T) do { \
    (T).EGR = TIM_EGR(UG); \
    while(!((T).SR & TIM_SR(UIF))); \
    (T).SR &= ~TIM_SR(UIF); \
} while(0)

/**
 * @}
 */

/**
 * @defgroup DS18B20_Private_Functions DS18B20 Private Functions
 * @{
 */

/**
 * @brief Default weak implementation for LED control - provides visual feedback during operations
 * @param[in] action 0 to turn LED off, non-zero to turn LED on
 */
__WEAK void ds18b20_led_control(unsigned action) {
    (void)action;
    // Default implementation - empty (no LED control)
}

/**
 * @brief Default weak implementation for temperature ready callback - reports results
 * @param[in] temp_tenths Temperature value in tenths of degrees Celsius, or error code
 */
#if defined ELAPSED_TIME
__WEAK void ds18b20_temp_ready(int16_t temp_tenths, uint32_t t) {
    (void)t;
#else
__WEAK void ds18b20_temp_ready(int16_t temp_tenths) {
#endif
    (void)temp_tenths;
    // Default implementation - empty (no temperature handling)
}

/**
 * @brief Calculate CRC8 checksum for DS18B20 scratchpad data validation
 * @return CRC8 checksum value
 */
__STATIC_FORCEINLINE uint8_t check_scratchpad_crc(void) {
    uint8_t crc = 0;
    // Process each byte in the scratchpad (first 8 bytes) for CRC calculation
    for (uint8_t i = 0; i < DS18B20_CRC8_BYTES; i++) {
        uint8_t inByte = ctx.scratchpad[i];
        // Process each bit in the byte using Dallas/Maxim CRC8 algorithm
        for (uint8_t b = 0; b < 8; b++) {
            uint8_t mix = (crc ^ inByte) & 0x01;
            crc >>= 1;
            if (mix) crc ^= DS18B20_CRC8_POLY;
            inByte >>= 1;
        }
    }
    return crc;
}

/**
 * @brief Decode pulse durations into scratchpad bytes using bit timing analysis
 */
__STATIC_FORCEINLINE void decode_scratchpad(void) {
    // Process each byte in the scratchpad (9 bytes total)
    for (unsigned byte = 0; byte < DS18B20_SCRATCHPAD_LEN; ++byte) {
        const unsigned bit_start = byte * DS18B20_BITS_PER_BYTE;
        // Process each bit in the byte (8 bits per byte)
        for (unsigned bit = 0; bit < DS18B20_BITS_PER_BYTE; ++bit) {
            // Determine if pulse represents logic '1' or '0' based on duration threshold
            // Pulses <= 10µs are considered logic '1', > 10µs are logic '0'
            if (ctx.pulse[bit_start + bit] <= SHORT_PULSE_MAX) {
                ctx.scratchpad[byte] |= (1 << bit);  // Set bit to 1
            } else {
                ctx.scratchpad[byte] &= ~(1 << bit);  // Reset bit to 0
            }
        }
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
    T1.ARR = arr; T1.RCR = rcr;
    // Force update event to load new values
    FORCE_UPDATE_EVENT(T1);
    // Start timer in One Pulse Mode (OPM) - runs once then stops
    T1.CR1 = TIM_CR1(OPM, CEN);
}

/**
 * @brief Wait for temperature conversion to complete (750ms typical)
 * @note Non-blocking - starts timer that will generate update event when complete
 */
__STATIC_FORCEINLINE void wait_conversion(void) { start_timer(62500, 11); }

/**
 * @brief Start inter-measurement pause period (500ms)
 * @note Non-blocking - starts timer for inter-measurement delay
 */
__STATIC_FORCEINLINE void start_cycle_pause(void) { start_timer(62500, 79); }

/**
 * @brief Initialize 1-Wire bus reset sequence using timer and DMA
 */
__STATIC_FORCEINLINE void reset_bus(void) {
    // Configure timer for reset pulse generation (480µs low)
    T1.ARR  = RESET_TIMEOUT;              // Total reset slot time (960µs)
    T1.CCR1 = RESET_PULSE_DURATION;       // Reset pulse duration (480µs)
    // Configure channel 1 for output compare (drive bus low)
    // Configure channel 2 for input capture (detect presence pulse)
    T1.CCMR1 = TIM_CCMR1(OC1M_0,OC1M_1,OC1M_2,OC1PE, CC2S_1, IC2F_0,IC2F_1,IC2F_2);
    T1.CCER  = TIM_CCER(CC1E, CC2E);      // Enable both channels
    T1.RCR   = 0;                         // No repetition
    // Configure DMA to capture presence pulse edge timestamps
    D13.CCR  = 0;                         // Clear DMA configuration
    D13.CPAR = (uint32_t)&T1.CCR2;        // DMA destination: timer capture register
    D13.CMAR = (uint32_t)ctx.edge;        // DMA source: edge timestamp buffer
    D13.CNDTR= CAPTURE_BUF_SIZE;          // Number of transfers (2 edges)
    D13.CCR  = DMA_CCR(MINC, PSIZE_0, MSIZE_0, EN); // Enable DMA with memory increment
    // Force timer update to load configuration
    FORCE_UPDATE_EVENT(T1);
    T1.CCR1 = 0;                          // Clear output compare value
    T1.DIER = TIM_DIER(CC2DE);            // Enable DMA request on capture
    T1.CR1  = TIM_CR1(OPM, CEN);          // Start timer in one-pulse mode
}

/**
 * @brief Transmit command sequence to DS18B20 using DMA
 * @param[in] cmd Pointer to command sequence in pulse duration format
 * @note Non-blocking - configures hardware to transmit command automatically
 */
__STATIC_FORCEINLINE void send_command(const uint8_t *cmd) {
    // Configure timer for command transmission using DMA
    T1.RCR = DS18B20_DMA_TRANSFERS - 1;   // Number of repetitions (16 transfers)
    T1.ARR = ONE_PULSE + ZERO_PULSE + 1;  // Total bit slot time (62µs)
    T1.CCR1 = cmd[0];                     // First pulse duration
    T1.CCR4 = ONE_PULSE + ZERO_PULSE;     // Update trigger time
    // Configure channel 1 for output compare mode
    T1.CCMR1 = TIM_CCMR1(OC1M_0,OC1M_1,OC1M_2);
    T1.CCER = TIM_CCER(CC1E);             // Enable output compare
    T1.DIER = TIM_DIER(CC4DE);            // Enable DMA request on update
    // Force timer update to load configuration
    FORCE_UPDATE_EVENT(T1);
    // Configure DMA to transmit command pulse sequence
    D14.CCR = 0;                          // Clear DMA configuration
    D14.CPAR = (uint32_t)&TIM1->CCR1;     // DMA destination: output compare register
    D14.CMAR = (uint32_t)&cmd[1];         // DMA source: command data (skip first byte)
    D14.CNDTR = DS18B20_DMA_TRANSFERS;    // Number of transfers
    D14.CCR = DMA_CCR(DIR, MINC, PSIZE_0, EN); // Enable DMA with memory increment
    T1.CR1 = TIM_CR1(OPM, CEN);           // Start timer in one-pulse mode
}

/**
 * @brief Read scratchpad data from DS18B20 using timer capture and DMA
 * @note Non-blocking - configures hardware to capture data automatically
 */
__STATIC_FORCEINLINE void read_data(void) {
    // Configure timer for data reading with input capture
    T1.RCR = DS18B20_SCRATCHPAD_BITS - 1; // Number of repetitions (72 bits)
    T1.ARR = ONE_PULSE + ZERO_PULSE + 1;  // Total bit slot time (62µs)
    T1.CCR1 = ONE_PULSE;                  // Read pulse duration (1µs)
    // Configure channel 1 for output compare (generate read pulse)
    // Configure channel 2 for input capture (measure return pulse durations)
    T1.CCMR1 = TIM_CCMR1(OC1M_0,OC1M_1,OC1M_2,OC1PE, CC2S_1,IC2F_0,IC2F_1,IC2F_2);
    T1.CCER = TIM_CCER(CC1E, CC2E);       // Enable both channels
    T1.DIER = TIM_DIER(CC2DE);            // Enable DMA request on capture
    // Force timer update to load configuration
    FORCE_UPDATE_EVENT(T1);
    T1.CCR1 = 0;                          // Clear output compare value
    // Configure DMA to capture pulse durations into pulse buffer
    D13.CCR = 0;                          // Clear DMA configuration
    D13.CPAR = (uint32_t)&T1.CCR2;        // DMA destination: capture register
    D13.CMAR = (uint32_t)ctx.pulse;       // DMA source: pulse duration buffer
    D13.CNDTR= DS18B20_SCRATCHPAD_BITS;   // Number of transfers (72 bits)
    D13.CCR = DMA_CCR(MINC, PSIZE_0, EN); // Enable DMA with memory increment
    T1.CR1 = TIM_CR1(OPM, CEN);           // Start timer in one-pulse mode
}

/**
 * @brief Configure system clock to 72MHz using PLL
 */
__STATIC_FORCEINLINE void configure_system_clock(void) {
    // Enable HSI and HSE oscillators
    R.CR = RCC_CR(HSION, HSEON);
    // Configure PLL: HSE source, multiply by 9, APB1 prescaler /2
    R.CFGR = RCC_CFGR(PLLSRC, PLLMULL9, PPRE1_DIV2);
    // Enable PLL
    R.CR = RCC_CR(HSION, HSEON, PLLON);
    // Wait for PLL and HSE ready flags
    while (RCC_CR(PLLRDY, HSERDY) != (R.CR & RCC_CR(PLLRDY, HSERDY)));
    // Configure flash latency for 72MHz operation
    F.ACR = FLASH_ACR(PRFTBE, LATENCY_2);
    // Switch system clock to PLL
    R.CFGR = RCC_CFGR(PLLSRC, PLLMULL9, PPRE1_DIV2, SW_PLL);
    // Wait for system clock switch to PLL
    while ((R.CFGR & RCC_CFGR(SWS_PLL)) != RCC_CFGR(SWS_PLL));
    // Disable HSI oscillator to save power
    R.CR &= ~RCC_CR(HSION);
}

/**
 * @brief Initialize low-level peripherals for DS18B20 communication
 */
__STATIC_FORCEINLINE void init_peripherals_lowlevel(void) {
    // Enable clocks for required peripherals: GPIOA, GPIOC, TIM1, DMA1
    R.APB2ENR |= RCC_APB2ENR(IOPAEN, IOPCEN, TIM1EN);
    R.AHBENR  |= RCC_AHBENR(DMA1EN);
    // Configure timer prescaler for 1µs resolution (72MHz/72 = 1MHz)
    T1.PSC     = TIM_PRESCALER;
    T1.EGR     = TIM_EGR(UG);
    T1.BDTR    = TIM_BDTR(MOE);
    // Configure PA8 for 1-Wire communication (alternate function open drain)
    PA.CRH    |= GPIO_CRH(CNF8_0, CNF8_1, MODE8_1);
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
    // Configure system clock to 72MHz
    configure_system_clock();
    // Initialize low-level peripherals for 1-Wire communication
    init_peripherals_lowlevel();
}

/**
 * @brief Main state machine function - must be called periodically from main loop
 * @note Non-blocking state machine that advances 1-Wire communication state
 * @note Uses timer update interrupt flag to determine when operations complete
 */
void ds18b20_poll(void) {

    #if defined ELAPSED_TIME
        static uint32_t elapsed_time;
    #endif

    // Check if timer update interrupt occurred (indicates operation completion)
    // This is the non-blocking way to detect when timed operations finish
    if (!(TIM1->SR & TIM_SR(UIF))) return;
    // Clear timer update interrupt flag
    TIM1->SR = 0;

    // State machine to manage 1-Wire communication sequence
    switch (ctx.current_state) {
        case 0: // IDLE - Initialize for new measurement cycle
             #if defined ELAPSED_TIME
                 elapsed_time = DWT->CYCCNT;
             #endif
            // Initialize union memory (fills with 0xFF pattern)
            ctx.fill_union = (uint64_t)-1;
            // Transition to START state
            ctx.current_state = 1;
            /* fallthrough to START state immediately */
            /* fallthrough  */

        case 1: // START - Begin measurement cycle, turn on LED
            // Turn on LED to indicate measurement in progress
            ds18b20_led_control(!0);
            // Initiate 1-Wire bus reset sequence
            reset_bus();
            // Transition to CONVERT state
            ctx.current_state = 2;
            break;

        case 2: // CONVERT - Check presence and send convert command
            // Verify DS18B20 presence using captured edge timestamps
            if (check_presence()) {
                // Device present - send temperature conversion command
                send_command(conv_cmd);
                // Transition to WAIT state to allow conversion time
                ctx.current_state = 3;
            } else {
                // No device present - report error and pause
                ds18b20_temp_ready(DS18B20_TEMP_ERROR_NO_SENSOR
                #if defined ELAPSED_TIME
                    , DWT->CYCCNT - elapsed_time
                #endif
                );
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
                // Device present - send read scratchpad command
                send_command(read_cmd);
                // Transition to READ state
                ctx.current_state = 6;
            } else {
                // No device present - report error and pause
                ds18b20_temp_ready(DS18B20_TEMP_ERROR_NO_SENSOR
                #if defined ELAPSED_TIME
                    , DWT->CYCCNT - elapsed_time
                #endif
                );
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
            ds18b20_led_control(0);

            // Validate CRC and report temperature or error
            if (ctx.scratchpad[8] == check_scratchpad_crc()) {
                // CRC valid - decode and report temperature
                ds18b20_temp_ready(decode_temperature()
                #if defined ELAPSED_TIME
                    , DWT->CYCCNT - elapsed_time
                #endif
                );
            } else {
                // CRC invalid - report error
                ds18b20_temp_ready(DS18B20_TEMP_ERROR_CRC_FAIL
                #if defined ELAPSED_TIME
                    , DWT->CYCCNT - elapsed_time
                #endif
                );
            }

            // Start inter-measurement pause period
            start_cycle_pause();
            // Return to IDLE state for next measurement cycle
            ctx.current_state = 0;
            break;

        default:
            // Unexpected state - report generic error
            ds18b20_temp_ready(DS18B20_TEMP_ERROR_GENERIC
            #if defined ELAPSED_TIME
                , DWT->CYCCNT - elapsed_time
            #endif
            );
            // Return to IDLE state
            ctx.current_state = 0;
            break;
    }
}

/**
 * @}
 */
