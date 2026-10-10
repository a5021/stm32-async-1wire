/**
 * @file app.h
 * @brief Shared application layer for the example projects
 * 
 * Bundles everything an example application needs: the DS18B20 driver API
 * plus a small platform layer (system clock, USART1 TX ring buffer, busy
 * LED, polled millisecond clock) behind a single header, so the demos stay
 * short and readable.
 *
 * Usage:
 * 1. #include "app.h" (defines UART_TX_BUF_SIZE, or -D on the command line)
 * 2. Call app_init() once at startup
 * 3. Use uart_write_*() to enqueue strings to the non-blocking TX ring buffer
 * 4. Call uart_poll_tx() periodically to feed the UART from the buffer
 * 5. Pace the measurement cycles with app_millis(): the driver measures one
 *    cycle per ds18b20_start_measure() and is idle until then
 *
 * Nothing here installs an interrupt: the millisecond clock is the SysTick
 * counter read by polling, so an example build has no ISR at all. With
 * -DOW_PORT_LOW_POWER=1 the driver itself sleeps through its long stages
 * (UIE + SEVONPEND, still without an NVIC interrupt).
 */

#ifndef APP_H
#define APP_H

#include "ds18b20.h"
#include "onewire.h"
#include <stdint.h>

#ifndef UART_TX_BUF_SIZE
#define UART_TX_BUF_SIZE 128u /**< Default TX ring buffer size (power of two) */
#endif

#if (UART_TX_BUF_SIZE & (UART_TX_BUF_SIZE - 1u)) != 0
#error "UART_TX_BUF_SIZE must be a power of two (e.g., 128, 256, 512, 1024)."
#endif

/** Mask for ring buffer index wrapping (power of two optimization) */
#define UART_TX_IDX_MASK (UART_TX_BUF_SIZE - 1u)
/** UART baud rate register value with rounding for accuracy */
#define USART_BRR_CALC(PCLK, BAUD) (((PCLK) + ((BAUD) / 2)) / (BAUD))

/* --- F4 APB prescalers and the console divisor derived from them -----------
 *
 * app.c programs RCC->CFGR from these divisors and derives the console UART
 * divisor from the same place, so the two cannot disagree. This lives in the
 * header rather than in app.c because the register write it feeds is inside
 * hardware_init(), which is #if !defined(DS18B20_TEST_HARNESS) - i.e. no host
 * build ever compiles it. Pinning the value here is what gives the F4 suite any
 * reach over it at all.
 *
 * That gap was not hypothetical: the divisor used to be a per-clock ternary
 * chain (168 ? 42 : 84 ? 42 : SYSCLK) with no 180MHz case, so the F446 fell
 * through to the raw-HSI default and sized BRR for PCLK2 = 180MHz when the part
 * runs 45/90. The result was a UART that worked perfectly at twice the wrong
 * baud rate, on a clean build. The divisors are the datasheet limits, which
 * differ per part (F407 42/84, F446 45/90) but fall out of the same /4 and /2 -
 * which is exactly why adding a clock to app.c alone is not enough.
 */
#if defined(OW_PORT_FAMILY_F4)
#if (OW_PORT_SYSCLK_MHZ) == 180 || (OW_PORT_SYSCLK_MHZ) == 168
#define OW_F4_APB1_DIV 4u
#define OW_F4_APB2_DIV 2u
#elif (OW_PORT_SYSCLK_MHZ) == 84
#define OW_F4_APB1_DIV 2u
#define OW_F4_APB2_DIV 2u
#else
/* Raw HSI and raw HSE: neither programs RCC->CFGR, so both buses stay /1. */
#define OW_F4_APB1_DIV 1u
#define OW_F4_APB2_DIV 1u
#endif
/** @brief APB1 peripheral clock in MHz (USART3 console path) */
#define OW_F4_PCLK1_MHZ ((OW_PORT_SYSCLK_MHZ) / OW_F4_APB1_DIV)
/** @brief APB2 peripheral clock in MHz (USART1 console path) */
#define OW_F4_PCLK2_MHZ ((OW_PORT_SYSCLK_MHZ) / OW_F4_APB2_DIV)
/** @brief Console BRR on the default USART1/APB2 path, at 115200 baud */
#define OW_F4_CONSOLE_BRR USART_BRR_CALC(OW_F4_PCLK2_MHZ * 1000000u, 115200)
/** @brief Console BRR on the optional OW_UART_USART3/APB1 path */
#define OW_F4_CONSOLE_BRR_APB1 USART_BRR_CALC(OW_F4_PCLK1_MHZ * 1000000u, 115200)

/* PCLK reaches USART_BRR_CALC as integer MHz, so a clock that is not a whole
 * multiple of its prescaler would be rounded here and quietly shift the baud
 * rate. Every supported F4 clock divides evenly; this says so. */
_Static_assert((OW_PORT_SYSCLK_MHZ) % (int)OW_F4_APB1_DIV == 0 &&
                   (OW_PORT_SYSCLK_MHZ) % (int)OW_F4_APB2_DIV == 0,
               "F4: PCLK is derived as SYSCLK/div in integer MHz, so SYSCLK must divide by both APB prescalers");
#endif /* OW_PORT_FAMILY_F4 */

/* --- G4 console divisor ----------------------------------------------------
 *
 * Same shape as the F4 block above, simpler: this backend leaves every APB
 * prescaler at /1 on both supported clocks (170 HSE+PLL, 16 raw HSI16), so
 * PCLK2 = SYSCLK and the divisor is one line. It still lives here rather
 * than in app.c for the same reason: the register write it feeds is inside
 * hardware_init(), which no host build compiles, so this is the host suite's
 * only reach over it (test_timing.c::test_g4_console_baud_divisor). */
#if defined(OW_PORT_FAMILY_G4)
/** @brief APB2 peripheral clock in MHz (USART1 console path; APB2 stays /1) */
#define OW_G4_PCLK2_MHZ ((OW_PORT_SYSCLK_MHZ))
/** @brief Console BRR on USART1/APB2 at 115200 baud */
#define OW_G4_CONSOLE_BRR USART_BRR_CALC(OW_G4_PCLK2_MHZ * 1000000u, 115200)
#endif /* OW_PORT_FAMILY_G4 */

/* --- H5 console divisor ----------------------------------------------------
 *
 * Same shape as G4: USART1 is on APB2, left at /1, so PCLK2 = SYSCLK and the
 * divisor is one line. It still lives here rather than in app.c for the same
 * reason: the register write it feeds is inside hardware_init(), which no
 * host build compiles, so this is the host suite's only reach over it
 * (test_timing.c::test_h5_console_baud_divisor). */
#if defined(OW_PORT_FAMILY_H5)
/** @brief APB2 peripheral clock in MHz (USART1 console path; APB2 stays /1) */
#define OW_H5_PCLK2_MHZ ((OW_PORT_SYSCLK_MHZ))
/** @brief Console BRR on USART1/APB2 at 115200 baud */
#define OW_H5_CONSOLE_BRR USART_BRR_CALC(OW_H5_PCLK2_MHZ * 1000000u, 115200)
#endif /* OW_PORT_FAMILY_H5 */

/**
 * @brief Initialize system clock, USART1 TX and the busy LED GPIO
 * @note One call instead of configure_system_clock() + hardware_init()
 */
void app_init(void);

#if !defined(DS18B20_TEST_HARNESS)
/**
 * @brief Milliseconds since app_init(), from the polled SysTick counter
 * @return Counter in ms (wraps after ~49 days)
 * @note Non-blocking: a single register read, no interrupt and no waiting.
 *       SysTick runs at 1 kHz with its interrupt disabled, so the build that
 *       uses this still contains no interrupt handler at all. Must be called
 *       at least once per millisecond - any polling main loop does.
 */
uint32_t app_millis(void);
#endif

#if defined(DS18B20_TEST_HARNESS) && (defined(OW_PORT_FAMILY_F4) || defined(OW_PORT_FAMILY_G4) || defined(OW_PORT_FAMILY_H5))
/**
 * @brief Configure system clock (exposed for the F4/G4/H5 host harness)
 * @note On target builds this is file-local and force-inlined in app.c.
 *       Under DS18B20_TEST_HARNESS the F4/G4 suite drives the real clock path
 *       against the RCC/FLASH mocks (test_timing), so it needs external
 *       linkage. Other families keep the function out of the harness.
 */
void configure_system_clock(void);
#endif

#if defined(DS18B20_TEST_HARNESS) && (defined(OW_PORT_FAMILY_F4) || defined(OW_PORT_FAMILY_G4) || defined(OW_PORT_FAMILY_H5))
/**
 * @brief Whether the requested clock actually started (F4/G4/H5 harness surface)
 * @note F4/G4/H5 report a clock that never came up through this rather than through a
 *       return value: changing configure_system_clock()'s signature changed
 *       codegen in the families that cannot fail, for no benefit. 1 = running,
 *       0 = an HSE or PLL wait timed out.
 */
uint8_t app_clock_ok(void);
#endif

/**
 * @brief Advance USART1 transmission by at most one byte (non-blocking)
 * @note Must be called periodically to feed the UART from the ring buffer
 */
void uart_poll_tx(void);

/**
 * @brief Block until every enqueued byte has been transmitted
 * @note Blocking, intended for diagnostic/blocking code paths only
 */
void uart_flush(void);

/**
 * @brief Enqueue a single byte into the USART1 TX ring buffer (non-blocking)
 * @param[in] b Byte to enqueue
 * @return 1 if enqueued, 0 if the buffer is full (byte dropped)
 * @note Never blocks: when the buffer is full the byte is dropped so the
 *       caller's code path stays non-blocking.
 */
int uart_tx_enqueue_byte(int b);

/**
 * @brief Enqueue a null-terminated string (non-blocking)
 * @param[in] s Null-terminated string to enqueue
 * @return Number of characters actually enqueued (may be less than strlen)
 */
int uart_write_str(const char* s);

/**
 * @brief Enqueue an integer as a decimal string (non-blocking)
 * @param[in] value Integer value to enqueue (full 32-bit range supported)
 * @return Number of characters actually enqueued
 */
int uart_write_int(int value);

/**
 * @brief Enqueue a byte as two uppercase hexadecimal digits (non-blocking)
 * @param[in] b Byte to enqueue
 * @return Number of characters actually enqueued (0, 1 or 2)
 */
int uart_write_hex(uint8_t b);

#endif // APP_H
