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

#if defined(DS18B20_TEST_HARNESS) && defined(OW_PORT_FAMILY_F4)
/**
 * @brief Configure system clock (exposed for the F4 host harness)
 * @note On target builds this is file-local and force-inlined in app.c.
 *       Under DS18B20_TEST_HARNESS the F4 suite drives the real clock path
 *       against the RCC/FLASH mocks (test_timing), so it needs external
 *       linkage. Other families keep the function out of the harness.
 */
void configure_system_clock(void);
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
