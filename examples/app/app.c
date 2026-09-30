/**
 * @file app.c
 * @brief Shared application layer implementation (UART TX, system clock, LED)
 */

#include "app.h"
#if defined(OW_PORT_FAMILY_F0)
#include "stm32f0xx.h"
#elif defined(OW_PORT_FAMILY_F3)
#include "stm32f3xx.h"
#elif defined(OW_PORT_FAMILY_G0)
#include "stm32g0xx.h"
#elif defined(OW_PORT_FAMILY_F4)
#include "stm32f4xx.h"
#else
#include "stm32f1xx.h"
#endif

// ======== USART1 TX ring buffer ========
static uint32_t uart_tx_head = 0; // write index - points to next free slot
static uint32_t uart_tx_tail = 0; // read index - points to oldest data
static uint8_t uart_tx_buf[UART_TX_BUF_SIZE]; // circular buffer for UART transmission

/**
 * @brief Advance USART1 transmission by at most one byte (non-blocking)
 * @note Must be called periodically to feed the UART from the ring buffer
 */
void uart_poll_tx(void) {
#if defined(OW_PORT_FAMILY_G0)
    // G0 uses the modern USART naming: TXE/TXFNF lives in ISR, data in TDR
    if ((USART1->ISR & USART_ISR_TXE_TXFNF) && (uart_tx_tail != uart_tx_head)) {
        uint8_t b = uart_tx_buf[uart_tx_tail];
        uart_tx_tail = (uart_tx_tail + 1u) & UART_TX_IDX_MASK;
        USART1->TDR = b;
    }
#elif defined(OW_PORT_FAMILY_F0) || defined(OW_PORT_FAMILY_F3)
    // F0 and F3 unified USART naming: TXE lives in ISR, data goes to TDR.
    // F3 spells the flag plain USART_ISR_TXE - unlike G0 there is no TXFNF.
    if ((USART1->ISR & USART_ISR_TXE) && (uart_tx_tail != uart_tx_head)) {
        // Get byte from buffer at tail position
        uint8_t b = uart_tx_buf[uart_tx_tail];
        // Advance tail pointer with wrap-around
        uart_tx_tail = (uart_tx_tail + 1u) & UART_TX_IDX_MASK;
        // Write byte to UART data register for transmission
        USART1->TDR = b;
    }
#else
    // Check if UART is ready to transmit (TXE flag set) and buffer not empty.
    // Optional OW_UART_USART3 selects USART3/PB10 instead of USART1 (TX on PB6).
#if defined(OW_UART_USART3)
    if ((USART3->SR & USART_SR_TXE) && (uart_tx_tail != uart_tx_head)) {
        uint8_t b = uart_tx_buf[uart_tx_tail];
        uart_tx_tail = (uart_tx_tail + 1u) & UART_TX_IDX_MASK;
        USART3->DR = b;
    }
#else
    if ((USART1->SR & USART_SR_TXE) && (uart_tx_tail != uart_tx_head)) {
        // Get byte from buffer at tail position
        uint8_t b = uart_tx_buf[uart_tx_tail];
        // Advance tail pointer with wrap-around
        uart_tx_tail = (uart_tx_tail + 1u) & UART_TX_IDX_MASK;
        // Write byte to UART data register for transmission
        USART1->DR = b;
    }
#endif
#endif
}

/**
 * @brief Block until every enqueued byte has been transmitted
 * @note Blocking, intended for diagnostic/blocking code paths only; the
 *       demos keep the non-blocking uart_poll_tx() discipline.
 */
void uart_flush(void) {
    while (uart_tx_tail != uart_tx_head) {
        uart_poll_tx();
    }
}

/**
 * @brief Enqueue a single byte into the UART transmit buffer (non-blocking)
 * @param[in] b Byte to enqueue
 * @return 1 if enqueued, 0 if the buffer is full (byte dropped)
 * @note Never blocks: when the buffer is full the byte is dropped so the
 *       caller's code path stays non-blocking.
 */
int uart_tx_enqueue_byte(int b) {
    uint32_t head = uart_tx_head;
    // Calculate next head position with wrap-around using power-of-two mask
    uint32_t next = (head + 1u) & UART_TX_IDX_MASK;
    if (next != uart_tx_tail) { // Room is available
        uart_tx_buf[head] = (uint8_t)b; // Store byte at current head position
        uart_tx_head = next; // Update head pointer
        return 1;
    }
    return 0; // Buffer full - drop the byte to stay non-blocking
}

/**
 * @brief Enqueue an entire null-terminated string (non-blocking)
 * @param[in] s Null-terminated string to enqueue
 * @return Number of characters actually enqueued (may be less than strlen)
 */
int uart_write_str(const char* s) {
    const char* start = s;
    while (*s) {
        if (uart_tx_enqueue_byte(*s)) {
            s++;
        } else {
            break; // Buffer full - stop to stay non-blocking
        }
    }
    return (int)(s - start);
}

/**
 * @brief Convert integer to string and enqueue for UART transmission
 * @param[in] value Integer value to convert and transmit
 * @return Number of characters enqueued
 * @note Buffer holds 12 chars, enough for the full int32 range (-2147483648)
 */
int uart_write_int(int value) {
    char buf[12]; // enough for -2147483648 and '\0'
    char* p = buf + sizeof(buf) - 1;
    *p = '\0';

    if (value == 0) { // Special case for zero
        *(--p) = '0';
    } else {
        int is_negative = 0;
        unsigned int uvalue;

        if (value < 0) { // Handle negative numbers
            is_negative = 1;
            uvalue = (unsigned int)-(value + 1) + 1;
        } else {
            uvalue = (unsigned int)value;
        }

        do { // Convert digits from least significant to most significant
            *(--p) = '0' + (uvalue % 10);
            uvalue /= 10;
        } while (uvalue);

        if (is_negative) *(--p) = '-'; // Add negative sign if needed
    }
    return uart_write_str(p);
}

/**
 * @brief Enqueue one byte as two uppercase hexadecimal digits (non-blocking)
 * @param[in] b Byte to convert and transmit
 * @return Number of characters actually enqueued (0, 1 or 2)
 */
int uart_write_hex(uint8_t b) {
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    n += uart_tx_enqueue_byte(hex[(b >> 4) & 0x0F]);
    n += uart_tx_enqueue_byte(hex[b & 0x0F]);
    return n;
}

/* ---- ow_stats output callbacks (strong definitions) ---- */

void ow_stats_putchar(char c) {
    uart_tx_enqueue_byte((int)c);
}

void ow_stats_puts(const char* s) {
    uart_write_str(s);
}

void ow_stats_print_int(int32_t v) {
    uart_write_int((int)v);
}

void ow_stats_print_hex(uint8_t v) {
    uart_write_hex(v);
}

void ow_stats_tx_enqueue(char c) {
    uart_tx_enqueue_byte((int)c);
}

/* Hardware bring-up (system clock, USART1 TX, LED GPIO) with full register
 *-level access.  Excluded from the host test build, which only exercises the
 * non-blocking UART ring buffer above — except configure_system_clock(),
 * which the F4 harness compiles (and test_timing drives) against the RCC
 * and FLASH mocks. hardware_init/app_init/ds18b20_busy stay target-only. */

#if !defined(DS18B20_TEST_HARNESS) || defined(OW_PORT_FAMILY_F4)
/**
 * @brief Configure system clock
 * @note The source is derived from OW_PORT_SYSCLK_MHZ (see onewire.h).
 *       F1: 72MHz via HSE+PLL x9, or raw HSI at 8MHz. F030x6 has no HSE:
 *       48MHz via HSI/2+PLL x12, or raw HSI at 8MHz. G031x6 has no HSE:
 *       64MHz via HSI16+PLL (M=1, N=8, R=2), or raw HSI16 at 16MHz.
 *       F4: 180MHz (F446, HSE+PLL with the over-drive sequence), 168MHz
 *       (F405/F407) or 84MHz (the F401 cap) via HSE+PLL, raw HSI
 *       at 16MHz, or raw HSE at the crystal's own frequency.
 * @note On F4 the crystal comes from OW_HSE_MHZ, a *board* property, because
 *       one part ships on boards with different crystals. A wrong value cannot
 *       be caught at compile time - the PLL simply never locks - so the three
 *       waits that can never end are bounded (HSERDY, PLLRDY, and the F446's
 *       two over-drive flags).
 * @note On failure the flag is cleared and the clock tree is left untouched: the
 *       1us tick, SysTick and the console divisor are all compiled against the
 *       requested OW_PORT_SYSCLK_MHZ, so carrying on from the reset HSI would
 *       scale every 1-Wire timing by an unknown factor. app_init() reports the
 *       reason over the console and stops.
 */
#if defined(OW_PORT_FAMILY_F4) || defined(OW_PORT_FAMILY_F3)
/* Set by configure_system_clock() and read by app_init(). A return value would
 * have been the obvious channel, but changing the signature changed codegen in
 * families that can never fail, so the state is a flag and the signature is
 * untouched. Declared for every clock on the families that can fail at all;
 * the raw-oscillator clocks simply never clear it.
 *
 * F3 is here for the same reason F4 is, and by the same mechanism: its 72MHz
 * default runs off the external oscillator, which on the F3-DISCOVERY is the
 * 8MHz square wave the ST-LINK drives onto OSC_IN. Drive that board from
 * something other than the ST-LINK and HSERDY never arrives, which without
 * this would be an unbounded wait on a board that looks dead with no output. */
static uint8_t ow_clock_ok = 1u;

/* The wait helper is excluded for the one clock on each family that configures
 * nothing - the MCU is already on its reset oscillator - and so has no wait to
 * bound, which -Werror would otherwise report as an unused function. That
 * clock is 16MHz on F4 (raw HSI) and 8MHz on F3 (raw HSI again: RCC_CFGR.SW=00
 * selects HSI undivided, the /2 only feeding the PLL). */
#if defined(OW_PORT_FAMILY_F3)
#define OW_CLOCK_NEEDS_WAIT ((OW_PORT_SYSCLK_MHZ) != 8)
#else
#define OW_CLOCK_NEEDS_WAIT ((OW_PORT_SYSCLK_MHZ) != 16)
#endif
#if OW_CLOCK_NEEDS_WAIT
/* Bounded wait for a hardware flag, for the two waits that can otherwise never
 * end. Nothing time-based is running yet - SysTick starts in app_time_init(),
 * after the clock - so this is an iteration count rather than a calibrated
 * timeout: generous enough that a real PLL always wins, small enough that a
 * missing external clock stops the boot instead of hanging with a silent
 * console. */
#define OW_CLOCK_FLAG_TIMEOUT 4000000u
static uint8_t ow_wait_flag(volatile uint32_t* reg, uint32_t bit) {
    uint32_t n = OW_CLOCK_FLAG_TIMEOUT;
    while (n && !(*reg & bit))
        n--;
    return (*reg & bit) ? 1u : 0u;
}
#endif
#endif

#if defined(DS18B20_TEST_HARNESS) && (defined(OW_PORT_FAMILY_F4) || defined(OW_PORT_FAMILY_F3))
/* The F4/F3 host harness needs to know whether the clock started, since that is
 * now a flag rather than a return value. It lives here, not beside the console
 * helpers: those sit in the !DS18B20_TEST_HARNESS block, so under the harness
 * this would be compiled away while app.h still declares it. */
uint8_t app_clock_ok(void) {
    return ow_clock_ok;
}
#endif

void configure_system_clock(void) {
#if defined(OW_PORT_FAMILY_G0)
#if (OW_PORT_SYSCLK_MHZ) == 64
    // HSI16 is on and stable right after reset. PLL source must be selected
    // explicitly: on this family PLLSRC=00 means "no clock sent to the PLL",
    // HSI16 encodes as 10 (RCC_PLLCFGR_PLLSRC_HSI). M(=1) xN(=8) R(=2) then
    // gives 64MHz; PLLREN enables the PLLR output the SYSCLK mux uses.
    RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_HSI | RCC_PLLCFGR_PLLN_3 |
                   RCC_PLLCFGR_PLLR_0 | RCC_PLLCFGR_PLLREN;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY))
        ;
    // Flash latency: 2 wait states above 48MHz (RM0444)
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_LATENCY_1;
    // Switch system clock to PLLRCLK
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLLRCLK;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLLRCLK)
        ;
#elif (OW_PORT_SYSCLK_MHZ) == 16
    // Raw HSI16: the MCU already runs on the internal 16MHz RC after reset —
    // nothing to configure
#else
#error "Unsupported OW_PORT_SYSCLK_MHZ for G0: use 64 (HSI16+PLL) or 16 (raw HSI16)"
#endif
#elif defined(OW_PORT_FAMILY_F0)
#if (OW_PORT_SYSCLK_MHZ) == 48
    // PLL input is HSI/2 = 4MHz; x12 gives 48MHz. Configure the multiplier
    // before enabling the PLL so it locks on a valid clock (per RM0360).
    RCC->CFGR = RCC_CFGR_PLLMUL12;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY))
        ;
    // Flash latency for 48MHz operation (1 wait state)
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY;
    // Switch system clock to PLL
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL)
        ;
#elif (OW_PORT_SYSCLK_MHZ) == 8
    // Raw HSI: the MCU already runs on the internal 8MHz RC after reset —
    // nothing to configure
#else
#error "Unsupported OW_PORT_SYSCLK_MHZ for F0: use 48 (HSI+PLL) or 8 (raw HSI)"
#endif
#elif defined(OW_PORT_FAMILY_F4)
/* HSE + PLL, shared by both F4 target clocks.
 *
 * The crystal frequency is a property of the *board* and the target clock of the
 * *application*, so they arrive as separate knobs: OW_HSE_MHZ and
 * OW_PORT_SYSCLK_MHZ. Aiming the PLL input at exactly 1MHz - the value ST
 * recommends, mid-window of the 1-2MHz range - makes both dividers fall out:
 *
 *   PLLM = OW_HSE_MHZ        ->  PLL input = 1MHz
 *   PLLN = 2 * SYSCLK        ->  VCO = 2*SYSCLK, and PLLP = 2 divides it back
 *
 * An 8MHz crystal therefore lands on the historical M=8 with N=336 (168MHz),
 * N=168 (84MHz) or N=360 (180MHz) - the 168/336 pair is the configuration the
 * F407DISCOVERY was validated at - while a 25MHz board reaches the F401's 84MHz
 * cap with M=25/N=168. Deriving the dividers beats encoding one crystal per
 * target: the old code hardcoded M=8, so a 25MHz crystal could not reach its cap
 * at all, and the lock wait never ended.
 *
 * A clock the part cannot reach is rejected here rather than at runtime. The
 * per-part ceiling lives in ow_port_f4.h (OW_PORT_F4_MAX_SYSCLK_MHZ): 180MHz is a
 * real frequency on the F446 and an out-of-spec one on an F407, and the two do
 * not differ by a divisor - they differ by over-drive, flash latency and APB
 * limits, so a build that asked for 180 on an F407 could not be made correct
 * by scaling the PLL down.
 */
#if (OW_PORT_SYSCLK_MHZ) > (OW_PORT_F4_MAX_SYSCLK_MHZ)
#error "OW_PORT_SYSCLK_MHZ exceeds this F4 part's ceiling (OW_PORT_F4_MAX_SYSCLK_MHZ in ow_port_f4.h). 180MHz needs the F446's over-drive; 168 and 84 are the F407/F401 clocks."
#endif

    /* The APB prescalers this backend programs, and the console divisor derived
 * from them, live in app.h (OW_F4_APB1_DIV / OW_F4_APB2_DIV /
 * OW_F4_PCLK*_MHZ / OW_F4_CONSOLE_BRR) so the host suite can assert the value.
 * See the comment there for why a per-clock table in this file was a bug
 * waiting to happen. */

#if (OW_PORT_SYSCLK_MHZ) == 168 || (OW_PORT_SYSCLK_MHZ) == 84 || (OW_PORT_SYSCLK_MHZ) == 180
#define OW_F4_PLLM OW_HSE_MHZ
#define OW_F4_PLLN ((OW_PORT_SYSCLK_MHZ) * 2u)

    _Static_assert(OW_F4_PLLM >= 2u && OW_F4_PLLM <= 63u,
                   "F4: PLLM is 5 bits wide (2..63), so a crystal outside that cannot be "
                   "divided down to a 1MHz PLL input - check HSE_MHZ");
    _Static_assert(OW_F4_PLLN <= 511u, "F4: PLLN is 9 bits wide");
    _Static_assert(OW_F4_PLLN >= 100u && OW_F4_PLLN <= 432u,
                   "F4: the VCO must be 100..432MHz (RM0090/RM0368); at a 1MHz PLL input "
                   "the VCO equals PLLN");

/* Start HSE and bring the PLL to lock, returning 0 on either timeout. Q=7 keeps
 * the tree CubeMX-canonical and puts 48MHz on USB at 168MHz; USB is unused here
 * either way. */
#define OW_F4_PLL_START()                                                                    \
    do {                                                                                     \
        RCC->CR |= RCC_CR_HSEON;                                                             \
        if (!ow_wait_flag(&RCC->CR, RCC_CR_HSERDY)) {                                        \
            ow_clock_ok = 0u;                                                                \
            return;                                                                          \
        }                                                                                    \
        RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_HSE | (OW_F4_PLLM << RCC_PLLCFGR_PLLM_Pos) |       \
                       (OW_F4_PLLN << RCC_PLLCFGR_PLLN_Pos) | (0u << RCC_PLLCFGR_PLLP_Pos) | \
                       (7u << RCC_PLLCFGR_PLLQ_Pos);                                         \
        RCC->CR |= RCC_CR_PLLON;                                                             \
        if (!ow_wait_flag(&RCC->CR, RCC_CR_PLLRDY)) {                                        \
            ow_clock_ok = 0u;                                                                \
            return;                                                                          \
        }                                                                                    \
    } while (0)

#if (OW_PORT_SYSCLK_MHZ) == 180
    OW_F4_PLL_START();
    /* Over-drive (RM0390 §5.4.6), the one thing 180MHz needs that 168 does not.
     * Ordered like ST's own RCC_ClockConfig example: lock the PLL, turn OD on and
     * wait for it, and only then raise the flash latency and switch SYSCLK - the
     * core never runs fast before its wait states and voltage are right.
     *
     * Both waits are bounded and report through the same clock-failure flag as
     * the PLL, because a stuck ODRDY/ODSWRDY is as fatal as an unlocked PLL:
     * without it the 180MHz switch is out of spec. PWR sits on APB1, so its
     * clock has to be enabled before CR can be written at all.
     *
     * This stays in the 180 branch rather than being hoisted into the shared PLL
     * code on purpose: stm32f407xx.h defines none of these four bits (the F407
     * caps at 168 and never needs OD), so a shared version would not compile
     * there. */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    /* PWR_CR.VOS is deliberately not written. RM0390 (Power controller, "Bits
     * 15:14 VOS[1:0]") gives the whole field:
     *
     *   00: Reserved (Scale 3 mode selected)
     *   01: Scale 3 mode
     *   10: Scale 2 mode
     *   11: Scale 1 mode (reset value)
     *
     * The reset value is therefore already Scale 1, which is the scale 180MHz
     * is rated for, and the part was measured coming up that way: PWR_CR read
     * 0x0003C000 with ODEN and ODSWEN both set, i.e. VOS = 0b11.
     *
     * Do not "fix" this by writing PWR_CR_VOS_0. The bit suffix is not a scale
     * selector: _0 sets bit 14, so VOS becomes 0b01, which is Scale 3 - the
     * lowest scale, and the opposite of what is wanted here. The F407's VOS is
     * a single bit where 1 does mean Scale 1, and carrying that reading over to
     * the F446's two-bit field inverts it.
     *
     * RM0390 also constrains any future write: VOS "can be modified only when
     * the PLL is OFF", and "the new value programmed is active only when the
     * PLL is ON", with Scale 3 selected automatically while the PLL is off. So
     * a scale change would have to be written here, before the PLL switch
     * further down - not after it. */
    PWR->CR |= PWR_CR_ODEN;
    if (!ow_wait_flag(&PWR->CSR, PWR_CSR_ODRDY)) {
        ow_clock_ok = 0u;
        return;
    }
    /* Over-drive in the power-saving path; required by the same sequence even
     * though this application never enters a low-power mode. */
    PWR->CR |= PWR_CR_ODSWEN;
    if (!ow_wait_flag(&PWR->CSR, PWR_CSR_ODSWRDY)) {
        ow_clock_ok = 0u;
        return;
    }
    // Flash latency: 5 wait states, the same table row as 168MHz (RM0390): the
    // 180MHz cap moves the voltage-scale-1 limit, not the wait-state count.
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN |
                 FLASH_ACR_LATENCY_5WS;
    // APB1 /4 = 45MHz, APB2 /2 = 90MHz (both at the F446's datasheet limits -
    // different numbers from the F407's 42/84, which is why this is a separate
    // branch rather than a shared block). TIM1 is on APB2: with a prescaler != 1
    // the timer clock is doubled, so TIM1 = 2 * 90 = 180MHz = SYSCLK (the
    // ow_port 1us-tick invariant), which is what keeps
    // OW_PORT_TIM_PRESCALER at SYSCLK_MHZ - 1.
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2)) |
                RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2;
#elif (OW_PORT_SYSCLK_MHZ) == 168
    OW_F4_PLL_START();
    // Flash latency: 5 wait states for 150 < HCLK <= 168MHz (RM0090). 168MHz is
    // the max without over-drive, so over-drive is not enabled.
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN |
                 FLASH_ACR_LATENCY_5WS;
    // APB1 /4 = 42MHz, APB2 /2 = 84MHz (both at their datasheet limits). TIM1
    // is on APB2: with a prescaler != 1 the timer clock is doubled, so
    // TIM1 = 2 * 84 = 168MHz = SYSCLK (the ow_port 1us-tick invariant).
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2)) |
                RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2;
#elif (OW_PORT_SYSCLK_MHZ) == 84
    OW_F4_PLL_START();
    // Flash latency: 2 wait states, covering 60 < HCLK <= 90MHz at voltage scale 1
    // (RM0368). 84MHz needs 2, not 1.
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN |
                 FLASH_ACR_LATENCY_2WS;
    // APB1 /2 = 42MHz, APB2 /2 = 42MHz (both at their datasheet limits). TIM1
    // is on APB2: with a prescaler != 1 the timer clock is doubled, so
    // TIM1 = 2 * 42 = 84MHz = SYSCLK (the ow_port 1us-tick invariant).
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2)) |
                RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV2;
#endif
    // Switch system clock to PLL. Left unbounded, as before: with the PLL already
    // locked and selected this cannot hang.
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL)
        ;
#elif (OW_PORT_SYSCLK_MHZ) == 16
    // Raw HSI: the MCU already runs on the internal 16MHz RC after reset -
    // nothing to configure (APB2 stays /1, so TIM1 = HSI = 16MHz)
#elif (OW_PORT_SYSCLK_MHZ) == (OW_HSE_MHZ)
    /* Raw HSE: SYSCLK straight off the crystal, no PLL, so the core clock *is* the
     * crystal. Selected only when the requested clock equals it, which is the
     * point: asking for 8MHz on a 25MHz board used to compile and run with every
     * 1-Wire timing scaled by 3.125, silently. */
    _Static_assert((OW_PORT_SYSCLK_MHZ) <= 30u,
                   "F4: the raw-HSE path programs 0 flash wait states, valid to 30MHz");
    RCC->CR |= RCC_CR_HSEON;
    if (!ow_wait_flag(&RCC->CR, RCC_CR_HSERDY)) {
        ow_clock_ok = 0u;
        return;
    }
    // Flash latency: 0 wait states (<= 30MHz); caches on for deterministic timing.
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;
    // APB1/APB2 stay /1, so TIM1 = SYSCLK = HSE.
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_HSE;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSE)
        ;
#else
#error "Unsupported OW_PORT_SYSCLK_MHZ for F4: use 168 or 84 (HSE+PLL, crystal via HSE_MHZ), 180 on an F446 (HSE+PLL+over-drive), 16 (raw HSI), or a value equal to HSE_MHZ (raw HSE)"
#endif
#elif defined(OW_PORT_FAMILY_F3)
/* STM32F3 clock tree, and the reason the supported set is what it is.
 *
 * This family has no HSI16 - only HSI at 8MHz - and PLLSRC (RM0316 9.4.2,
 * the xB/C part) offers exactly two PLL inputs: HSI/2, or HSE/PREDIV. PLLMUL
 * runs 2..16. That fixes the whole reachable set:
 *
 *     4MHz x 16 = 64MHz   <- the HSI ceiling, not a choice
 *     8MHz x  9 = 72MHz   <- the part ceiling, and the only route to it
 *
 * so the two PLL clocks below are the two ends of what the silicon allows, and
 * 72MHz is out of reach from the internal RC. It needs the external oscillator:
 * on the STM32F3-DISCOVERY that is the 8MHz square wave the ST-LINK drives onto
 * OSC_IN, which is why it is read in bypass mode (RCC_CR.HSEBYP) rather than
 * through a crystal oscillator. PREDIV=1 is its reset value, so the 72MHz path
 * writes no CFGR2 at all.
 *
 * APB1 is capped at 36MHz and APB2 runs at full speed (RM0316 3.2.3), so APB1
 * goes to /2 and APB2 stays /1. TIM1 is on APB2: with a prescaler of 1 the
 * timer clock is PCLK2 = SYSCLK rather than the doubled 2 x PCLK a prescaler
 * other than 1 would give, which is what keeps the ow_port 1us-tick invariant
 * and OW_PORT_TIM_PRESCALER at SYSCLK_MHZ - 1 across all three clocks.
 *
 * There is no over-drive sequence here (that is F4-only) and no voltage scale to
 * set: this family has no PWR_CR.VOS field, so the mistake that cost a day on
 * the F446 cannot be made on an F3. Flash is 2 wait states for both PLL clocks
 * (RM0316: 48 < HCLK <= 72MHz) and 0 at the raw 8MHz.
 */
#define OW_F3_PPRE (RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV1)
#if (OW_PORT_SYSCLK_MHZ) == 8
    /* Raw HSI: RCC_CFGR.SW = 00 selects HSI undivided after reset, so the part
     * is already running at the requested clock and nothing is configured. The
     * /2 in PLLSRC divides the PLL *input* only, not SYSCLK - which is why this
     * is 8MHz and not 4MHz, the same fact F0's raw-HSI path relies on. */
    _Static_assert((OW_PORT_SYSCLK_MHZ) <= 24u,
                   "F3: the raw-HSI path programs 0 flash wait states, valid to 24MHz");
#elif (OW_PORT_SYSCLK_MHZ) == 64 || (OW_PORT_SYSCLK_MHZ) == 72
#if (OW_PORT_SYSCLK_MHZ) == 64
    /* HSI/2 = 4MHz, PLLMUL16 - the highest the internal RC can reach. */
#define OW_F3_PLL_SRC RCC_CFGR_PLLSRC_HSI_DIV2
#define OW_F3_PLL_MUL RCC_CFGR_PLLMUL16
    /* Verify the divider arithmetic rather than trusting the comment: 4 x 16. */
    _Static_assert((8u / 2u) * 16u == (OW_PORT_SYSCLK_MHZ),
                   "F3: 64MHz is HSI/2 (4MHz) x PLLMUL16");
#else
    /* HSE undivided (PREDIV = 1, its reset value) x PLLMUL9. PLLSRC is the
     * xB/C one-bit encoding here; the xD/E parts use a two-bit field with a
     * PREDIV1 entry, which is a different register layout and a separate
     * backend. */
#define OW_F3_PLL_SRC RCC_CFGR_PLLSRC_HSE_PREDIV
#define OW_F3_PLL_MUL RCC_CFGR_PLLMUL9
    _Static_assert((OW_HSE_MHZ) / 1u * 9u == (OW_PORT_SYSCLK_MHZ),
                   "F3: 72MHz is HSE/PREDIV1 x PLLMUL9; the board oscillator is 8MHz");
    /* Bypass: the ST-LINK drives OSC_IN with a square wave, not a crystal. */
    RCC->CR |= RCC_CR_HSEBYP | RCC_CR_HSEON;
    if (!ow_wait_flag(&RCC->CR, RCC_CR_HSERDY)) {
        ow_clock_ok = 0u;
        return;
    }
#endif
    /* Flash: 2 wait states for 48 < HCLK <= 72MHz, prefetch on. No caches on
     * this family (the F4 ICEN/DCEN bits have no counterpart). */
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;
    /* Configure the multiplier and the APB prescalers before enabling the PLL,
     * so it locks on a valid input (RM0316: PLLSRC is writable only with the
     * PLL disabled). */
    RCC->CFGR = OW_F3_PLL_MUL | OW_F3_PLL_SRC | OW_F3_PPRE;
    RCC->CR |= RCC_CR_PLLON;
    if (!ow_wait_flag(&RCC->CR, RCC_CR_PLLRDY)) {
        ow_clock_ok = 0u;
        return;
    }
    /* Switch SYSCLK to the PLL. Left unbounded, as on F4 and F1: with the PLL
     * already locked and selected, this cannot hang. */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL)
        ;
#else
#error "Unsupported OW_PORT_SYSCLK_MHZ for F3: use 8 (raw HSI), 64 (HSI/2 + PLL x16, the HSI ceiling) or 72 (HSE/PREDIV + PLL x9, the part ceiling)"
#endif
#undef OW_F3_PPRE
#else /* F1 */
#if (OW_PORT_SYSCLK_MHZ) == 72
    // Enable HSI and HSE oscillators
    RCC->CR = RCC_CR_HSION | RCC_CR_HSEON;
    // Wait for HSE to stabilize - HSERDY is the hardware stabilization
    // indicator, so no fixed delay is required
    while (!(RCC->CR & RCC_CR_HSERDY))
        ;
    // Configure PLL: HSE source, multiply by 9, APB1 prescaler /2
    RCC->CFGR = RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9 | RCC_CFGR_PPRE1_DIV2;
    // Enable PLL only after HSE is confirmed stable, so the PLL locks on a
    // valid clock (per RM0008: HSE must be ready before enabling the PLL)
    RCC->CR |= RCC_CR_PLLON;
    // Wait for the PLL to lock
    while (!(RCC->CR & RCC_CR_PLLRDY))
        ;
    // Configure flash latency for 72MHz operation
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;
    // Switch system clock to PLL
    RCC->CFGR = RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_SW_PLL;
    // Wait for system clock switch to PLL
    while ((RCC->CFGR & RCC_CFGR_SWS_PLL) != RCC_CFGR_SWS_PLL)
        ;
    // Disable HSI oscillator
    RCC->CR &= ~RCC_CR_HSION;
#elif (OW_PORT_SYSCLK_MHZ) == 8
    // Raw HSI: the MCU already runs on the internal 8MHz RC after reset —
    // nothing to configure
#else
#error "Unsupported OW_PORT_SYSCLK_MHZ for F1: use 72 (HSE+PLL) or 8 (raw HSI)"
#endif
#endif
}
#endif /* !DS18B20_TEST_HARNESS || OW_PORT_FAMILY_F4 */

#if !defined(DS18B20_TEST_HARNESS)
/**
 * @brief Initialize microcontroller peripherals for UART communication and LED control
 * @note F1: USART1 TX on PA9 (AF push-pull), LED on PC13. F0: same PA9 UART
 *       via MODER/AFR, LED on PA4 (no GPIOC on F030x6). F3: same PA9 UART
 *       (AF7) and LED on PE8, active high. G0: UART TX on logical PA9 (PA11
 *       pad after the SYSCFG remap, see ow_port_g0.h), LED on PA4 (no PC13
 *       bonded out on TSSOP20). F4: USART1 TX on PB6 (AF7; the F4DISCOVERY has
 *       no USART1-to-ST-LINK route on PA9), LED on PD12, with an optional
 *       OW_UART_USART3 path on PB10. The F4 console pin is PB6 because that is
 *       where the F4DISCOVERY's ST-LINK VCP is, and it is also where a WeAct
 *       F446RET6's CP210x VCP answers (measured); for a board whose VCP is on
 *       PA9, build with -DOW_UART_USART1_PA9. The F4 busy LED defaults to PD12
 *       (LD4 green on the F4DISCOVERY, active high); a board whose LED is on
 *       PB2 - the WeAct F446RET6 bench has it there, labelled "B2" on the
 *       silkscreen, active high (per the WeAct schematic) - opts in with
 *       -DOW_F4_LED_PB2.
 */
#if defined(OW_PORT_FAMILY_F4) || defined(OW_PORT_FAMILY_F3)
__STATIC_FORCEINLINE void app_set_console_baud(uint32_t pclk_mhz) {
#if defined(OW_UART_USART3)
    /* An F4-only knob: USART3 sits on APB1. No F3 build defines it. */
    USART3->BRR = USART_BRR_CALC(pclk_mhz * 1000000u, 115200);
#else
    USART1->BRR = USART_BRR_CALC(pclk_mhz * 1000000u, 115200);
#endif
}
#endif

__STATIC_FORCEINLINE void hardware_init(void) {
#if defined(OW_PORT_FAMILY_F4)
    // Enable AHB1 GPIOA/GPIOD/GPIOB and APB2 USART1 clocks (GPIOA/DMA2/TIM1 clock is
    // enabled by ow_port_init())
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIODEN | RCC_AHB1ENR_GPIOBEN;

    // APB bus clocks for the console UART, derived from the prescalers this
    // file programs (OW_F4_APB1_DIV/OW_F4_APB2_DIV above), not restated here:
    // 168MHz and 180MHz -> APB1 /4, APB2 /2 (42/84 and 45/90); 84MHz -> /2, /2
    // (42/42); raw 16/8MHz -> /1, /1 (both = SYSCLK).
    // Test: tests/test/test_timing.c::test_console_baud_divisor()

    // F4 busy LED. Default (F4DISCOVERY): LD4 (green) on PD12, active high
    // (pin -> LED -> GND). With -DOW_F4_LED_PB2 the bench WeAct F446RET6's
    // B2 LED is used instead (silkscreen "B2" = PB2, active high per the
    // manufacturer): no extra port clock is needed because GPIOB is already
    // enabled for the PB6 console pin.
#if defined(OW_F4_LED_PB2)
    GPIOB->MODER = (GPIOB->MODER & ~GPIO_MODER_MODER2) | GPIO_MODER_MODER2_0;
    GPIOB->OTYPER &= ~GPIO_OTYPER_OT_2;
#else
    GPIOD->MODER = (GPIOD->MODER & ~GPIO_MODER_MODER12) | GPIO_MODER_MODER12_0;
#endif

    // Optional OW_UART_USART3 selects USART3 TX on PB10 (AF7) instead of
    // USART1 on PB6 (the STM32F4DISCOVERY has no USART1-to-ST-LINK route).
    // USART3 is on APB1.
#if defined(OW_UART_USART3)
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
    GPIOB->MODER = (GPIOB->MODER & ~GPIO_MODER_MODER10) | GPIO_MODER_MODER10_1;
    GPIOB->OTYPER &= ~GPIO_OTYPER_OT_10;
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~GPIO_AFRH_AFSEL10) | (7u << GPIO_AFRH_AFSEL10_Pos); /* AF7 = USART3 */
    // PCLK1: OW_F4_APB1_DIV at the configured SYSCLK (/4 at 168 and 180MHz,
    // /2 at 84MHz, /1 on the raw clocks).
    app_set_console_baud(OW_F4_PCLK1_MHZ);
    USART3->CR1 = USART_CR1_TE | USART_CR1_UE; // Enable USART3; TX enable only
#else
    /* USART1 TX pin. PB6 is the F4DISCOVERY default because that board routes
     * its ST-LINK virtual COM port there; it is not a general F4 default. A
     * WeAct F446RET6 also answers on PB6 (measured - its CP210x VCP is wired
     * there, not on PA9), so the default works on both boards without flags.
     *
     * The pin stays a knob because a board that puts its VCP on PA9 would be
     * silent, and silently so: the clock-failure banner would go out the same
     * missing pin, so "no output" could not be told apart from "the clock never
     * started". Hence OW_UART_USART1_PA9, default unchanged.
     *
     * PA9 is safe alongside the bus: ow_port_f4.h takes PA10 for TIM1_CH3 and
     * PA11 for the logic-analyzer marker, and only PA10 would clash with a
     * board's USART1_RX - which nothing here configures, the console is TX only.
     */
#if defined(OW_UART_USART1_PA9)
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODER9) | GPIO_MODER_MODER9_1;
    GPIOA->OTYPER &= ~GPIO_OTYPER_OT_9;
    /* PA9 is pin 9, so it is in the high half: AFRH / AFR[1]. The F4 headers
     * define GPIO_AFRH_AFSEL9* and no GPIO_AFRL_AFSEL9 at all, which is what a
     * build with -DOW_UART_USART1_PA9 catches immediately - the F0/G0 PA9 path
     * below reads the same register and gets this right. */
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~GPIO_AFRH_AFSEL9) | (7u << GPIO_AFRH_AFSEL9_Pos); /* AF7 = USART1 */
#else
    // Configure PB6 as alternate function push-pull output (AF7 = USART1_TX).
    // The F4DISCOVERY has no USART1-to-ST-LINK route on PA9, so the console
    // rides USART1 on PB6 (AF7); PA9 stays free.
    GPIOB->MODER = (GPIOB->MODER & ~GPIO_MODER_MODER6) | GPIO_MODER_MODER6_1;
    GPIOB->OTYPER &= ~GPIO_OTYPER_OT_6;
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~GPIO_AFRL_AFSEL6) | (7u << GPIO_AFRL_AFSEL6_Pos); /* AF7 = USART1 */
#endif
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    // USART1 is on APB2: OW_F4_APB2_DIV at the configured SYSCLK (/2 at 168,
    // 180 and 84MHz, /1 on the raw clocks).
    app_set_console_baud(OW_F4_PCLK2_MHZ);
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE; // Enable USART1; TX enable only
#endif
#elif defined(OW_PORT_FAMILY_F3)
    /* Console on PA9, LED on PE8 - the STM32F3-DISCOVERY (MB1035B).
     *
     * PA9 is the natural choice here: USART1_TX is AF7 on this pin, and it
     * sits next to PA10 without touching it, so the bus and the console share
     * the port. (The F4 backend needs the -DOW_UART_USART1_PA9 equivalent
     * because its board routes the VCP to PB6 instead; this board's ST-LINK
     * VCP is on PA2/PA3, so an external USB-UART on PA9 is what this pin is
     * for, and that is how the F3 numbers were taken.)
     *
     * LED: all eight indicators on MB1035B are on GPIOE, PE8..PE15, per ST's
     * own BSP header. PE8 is LED4 (blue). Active high, which is the opposite of
     * F1's PC13 and F0's assumed PA4 - ST's BSP_LED_On writes GPIO_PIN_SET here
     * - so the polarity in ds18b20_busy() is the other way round from those.
     */
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOEEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    // PA9: alternate function push-pull, AF7 = USART1_TX. Pin 9 is in the high
    // half of AFR, so AFR[1] field 1 - named AFRH_AFRH1 on this family, which
    // has no AFRH_AFSEL* macros at all.
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODER9) | GPIO_MODER_MODER9_1;
    GPIOA->OTYPER &= ~GPIO_OTYPER_OT_9;
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~GPIO_AFRH_AFRH1) | (7u << GPIO_AFRH_AFRH1_Pos);

    // PE8: plain push-pull output for the busy LED, active high.
    GPIOE->MODER = (GPIOE->MODER & ~GPIO_MODER_MODER8) | GPIO_MODER_MODER8_0;
    GPIOE->OTYPER &= ~GPIO_OTYPER_OT_8;

    // USART1 is on APB2, which this backend leaves at /1 at every supported
    // clock (8, 64, 72) - see configure_system_clock() - so PCLK2 = SYSCLK and
    // the divisor needs no separate clock variable.
    app_set_console_baud(OW_PORT_SYSCLK_MHZ);
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE; // Enable USART1; TX enable only
#elif defined(OW_PORT_FAMILY_F0) || defined(OW_PORT_FAMILY_G0)
    // Enable clock for GPIOA and USART1 (G0: GPIO on IOPENR, USART1 on APBENR2)
#if defined(OW_PORT_FAMILY_G0)
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN;
    RCC->APBENR2 |= RCC_APBENR2_USART1EN;
#else
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
#endif

    // Configure PA9 as alternate function push-pull output (F0: AF1, G0: AF1
    // = USART1_TX; on G0 the signal lands on the PA11 pad via SYSCFG remap)
#if defined(OW_PORT_FAMILY_G0)
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE9) | GPIO_MODER_MODE9_1;
#else
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODER9) | GPIO_MODER_MODER9_1;
#endif
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~GPIO_AFRH_AFSEL9) | (1u << GPIO_AFRH_AFSEL9_Pos);

    // Configure PA4 as general purpose output for LED control
#if defined(OW_PORT_FAMILY_G0)
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE4) | GPIO_MODER_MODE4_0;
#else
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODER4) | GPIO_MODER_MODER4_0;
#endif

    // Configure USART1: 115200 baud, 8 data bits, no parity, 1 stop bit, TX only
    USART1->BRR = USART_BRR_CALC((OW_PORT_SYSCLK_MHZ) * 1000000u, 115200); // PCLK = SYSCLK
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE; // Enable USART1; TX enable only
#else
    // Enable clock for GPIOA, USART1, and GPIOC peripherals
    RCC->APB2ENR |= (RCC_APB2ENR_IOPAEN | RCC_APB2ENR_USART1EN | RCC_APB2ENR_IOPCEN);

    // Configure PA9 as alternate function push-pull output, 2MHz speed
    // Clear existing configuration bits
    GPIOA->CRH &= ~(GPIO_CRH_MODE9 | GPIO_CRH_CNF9);
    // Set alternate function push-pull output mode, 2MHz speed
    GPIOA->CRH |= (GPIO_CRH_MODE9_1 | GPIO_CRH_CNF9_1);

    // Configure PC13 as general purpose output, 2MHz speed for LED control
    GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);
    GPIOC->CRH |= GPIO_CRH_MODE13_1;

    // Configure USART1: 115200 baud, 8 data bits, no parity, 1 stop bit, TX only
    USART1->BRR = USART_BRR_CALC((OW_PORT_SYSCLK_MHZ) * 1000000u, 115200); // PCLK2 = SYSCLK
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE; // Enable USART1; TX enable only
#endif
}

void app_time_init(void); /* defined below, with the polled millisecond clock */

/**
 * @brief Initialize system clock, USART1 TX and the busy LED GPIO
 */
#if defined(OW_PORT_FAMILY_F4) || defined(OW_PORT_FAMILY_F3)
/**
 * @brief Report a clock that never started, then stop.
 * @note Called with the console already up so the reason can be printed, which is
 *       the whole point: an unbounded PLL wait used to leave the board looking
 *       dead with no output at all. The clock tree is deliberately left alone -
 *       the 1us tick, SysTick and the console divisor are all compiled against
 *       the requested OW_PORT_SYSCLK_MHZ, so carrying on from the reset
 *       oscillator would scale every 1-Wire timing by an unknown factor.
 *       Stopping, and naming the knob to fix, is the honest outcome.
 */
static void app_stop_on_clock_failure(void) {
#if defined(OW_PORT_FAMILY_F3)
    /* The F3 default cannot come from the internal RC at all: with PLLMUL
     * topping out at 16 on a 4MHz HSI/2 input, 64MHz is the internal ceiling
     * and 72MHz needs the external oscillator. On the F3-DISCOVERY that is the
     * ST-LINK's MCO square wave, so the one-line fix is to keep the ST-LINK
     * attached - or to ask for a clock the internal RC can actually make. */
    static const char msg[] =
        "\r\nFATAL: the system clock did not start.\r\n"
        "  The external oscillator never reported ready, so the requested\r\n"
        "  clock is not running. At 72MHz the F3 PLL needs the board's 8MHz\r\n"
        "  external clock, which on the F3-DISCOVERY is driven by the ST-LINK\r\n"
        "  and is read in bypass mode: keep the ST-LINK connected, or build\r\n"
        "  with SYSCLK_MHZ=64 (HSI/2+PLL, the internal ceiling) or\r\n"
        "  SYSCLK_MHZ=8 (raw HSI).\r\n"
        "  Stopping on purpose - the 1us tick is compiled for the requested clock,\r\n"
        "  so the bus could not be timed correctly from here.\r\n";
#else
    static const char msg[] =
        "\r\nFATAL: the system clock did not start.\r\n"
        "  HSE or the PLL never reported ready, so the requested clock is not\r\n"
        "  running. On F4 the PLL input is derived from HSE_MHZ: if that does not\r\n"
        "  match the crystal on this board the PLL cannot lock. Rebuild with the\r\n"
        "  real crystal, or run from the internal RC with SYSCLK_MHZ=16.\r\n"
        "  Stopping on purpose - the 1us tick is compiled for the requested clock,\r\n"
        "  so the bus could not be timed correctly from here.\r\n";
#endif
    /* The divisor is for the reset oscillator, not the clock that was asked for. */
    app_set_console_baud(16u);
    uint32_t guard = 4000000u;
    const char* p = msg;
    while (*p && guard) {
        int n = uart_write_str(p);
        if (n > 0)
            p += n;
        uart_poll_tx();
        guard--;
    }
    for (;;)
        uart_poll_tx();
}
#endif

void app_init(void) {
    configure_system_clock();
    hardware_init();
    app_time_init();
#if defined(OW_PORT_FAMILY_F4) || defined(OW_PORT_FAMILY_F3)
    if (!ow_clock_ok)
        app_stop_on_clock_failure();
#endif
    /* Let the bus settle before the application's first transaction.
     *
     * Measured on a WeAct F446RET6 with 7 DS18B20 in parasite power: the first
     * search after a reset finds nothing, and every later one finds all seven.
     * A logic analyzer shows the bus is correct on the failing boot - 480.81us
     * reset pulses, 115.38us responses, nominal slots - so this is not bus
     * timing and not a firmware bug. It tracks how long the sensors have been
     * idle: after a programming cycle or a long idle they do not answer the
     * first reset.
     *
     * Bracketed by measurement, not chosen: 5ms failed 5 of 5 first boots, 50ms
     * passed 2 of 3, 200ms passed 3 of 3, and 400ms and 1000ms were also clean.
     * 200ms is the smallest value with margin, and it costs nothing at boot.
     *
     * This lives in the example harness rather than the driver on purpose: it is
     * a power-on settling concern of this wiring, and baking a 200ms wait into
     * the library would tax every user of a normally powered bus. Nothing on the
     * bus is touched before the application's first transaction, so settling here
     * settles before that transaction. */
    {
        uint32_t settle = app_millis();
        while ((app_millis() - settle) < 200u) {
        }
    }
}

/* ---- Polled millisecond clock (no interrupt, no waiting) ----
 *
 * The driver measures only when the application asks it to, so the application
 * owns the cadence. The clock is the ARM SysTick counter running at 1 kHz with
 * its interrupt *disabled*: no handler is installed and no NVIC bit is enabled.
 * The main loop just reads COUNTFLAG - set once per wrap - and folds it into a
 * millisecond counter, so app_millis() costs a single register read and never
 * waits. A missed tick can only happen if the loop does not run for a whole
 * millisecond, which is the one rule the examples follow. */

static uint32_t app_ms;

/**
 * @brief Start the polled millisecond clock
 * @note Called from app_init() after the system clock is configured, because
 *       the reload is derived from that clock (1 kHz from HCLK).
 */
void app_time_init(void) {
    app_ms = 0;
    SysTick->LOAD = ((uint32_t)(OW_PORT_SYSCLK_MHZ) * 1000u) - 1u;
    SysTick->VAL = 0;
    /* CLKSOURCE | ENABLE, deliberately without TICKINT: the counter runs and
     * sets COUNTFLAG, but never enters an exception handler. */
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
}

/**
 * @brief Milliseconds since app_time_init()
 * @return Counter in ms (wraps after ~49 days)
 * @note Non-blocking: reads the SysTick status flag and folds a wrap into the
 *       counter. Must be called at least once per millisecond, which any
 *       polling main loop does.
 */
uint32_t app_millis(void) {
    if (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) {
        (void)SysTick->CTRL; /* reading CTRL clears COUNTFLAG */
        app_ms++;
    }
    return app_ms;
}

/**
 * @brief Busy indicator - toggles LED during measurement
 * @param[in] action 0 = idle, non-zero = busy
 * @note Non-blocking LED control using atomic BSRR register operations.
 *       Strong definition overrides the weak one in the DS18B20 driver.
 *       F1: LED on PC13 (active low). F0: LED on PA4 (active low assumed).
 *       F3: LED on PE8 (active high - the STM32F3-DISCOVERY's indicators are
 *       active high, so this is the opposite of F1 and F0).
 *       F4 (STM32F4DISCOVERY): LD4 green on PD12 (active high). F4 with
 *       -DOW_F4_LED_PB2: the WeAct F446RET6 bench LED on PB2 (active high).
 */
void ds18b20_busy(unsigned action) {
#if defined(OW_PORT_FAMILY_F3)
    // PE8 is active high on the F3-DISCOVERY, so "busy" is a set, not a reset.
    GPIOE->BSRR = action ? GPIO_BSRR_BS_8 : GPIO_BSRR_BR_8;
#elif defined(OW_PORT_FAMILY_F0) || defined(OW_PORT_FAMILY_G0)
    if (action) {
        // Turn LED on (PA4 low)
#if defined(OW_PORT_FAMILY_G0)
        GPIOA->BSRR = GPIO_BSRR_BR4;
#else
        GPIOA->BSRR = GPIO_BSRR_BR_4;
#endif
    } else {
        // Turn LED off (PA4 high)
#if defined(OW_PORT_FAMILY_G0)
        GPIOA->BSRR = GPIO_BSRR_BS4;
#else
        GPIOA->BSRR = GPIO_BSRR_BS_4;
#endif
    }
#elif defined(OW_PORT_FAMILY_F4)
#if defined(OW_F4_LED_PB2)
    // PB2 is active high on the WeAct bench board (per its schematic), the
    // same polarity as the F4DISCOVERY's PD12, so "busy" is a set.
    if (action) {
        GPIOB->BSRR = GPIO_BSRR_BS2;
    } else {
        GPIOB->BSRR = GPIO_BSRR_BR2;
    }
#else
    if (action) {
        // Turn LED on (PD12 high)
        GPIOD->BSRR = GPIO_BSRR_BS12;
    } else {
        // Turn LED off (PD12 low)
        GPIOD->BSRR = GPIO_BSRR_BR12;
    }
#endif
#else
    if (action) {
        // Turn LED on (PC13 low due to pull-up LED configuration)
        // BSRR BR register: atomic bit reset operation
        GPIOC->BSRR = GPIO_BSRR_BR13;
    } else {
        // Turn LED off (PC13 high)
        // BSRR BS register: atomic bit set operation
        GPIOC->BSRR = GPIO_BSRR_BS13;
    }
#endif
}
#endif /* !DS18B20_TEST_HARNESS */
