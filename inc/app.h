/**
 * @file app.h
 * @brief Shared application layer for the DS18B20 example projects
 * 
 * Bundles everything an example application needs: the DS18B20 driver API
 * plus a small platform layer (system clock, USART1 TX ring buffer, busy
 * LED) behind a single header, so the demos stay short and readable.
 * 
 * Usage:
 * 1. #include "app.h" (defines UART_TX_BUF_SIZE, or -D on the command line)
 * 2. Call app_init() once at startup
 * 3. Use uart_write_*() to enqueue strings to the lossless TX ring buffer
 * 4. Call uart_poll_tx() periodically to feed the UART from the buffer
 */

#ifndef APP_H
#define APP_H

#include "ds18b20.h"
#include <stdint.h>

#ifndef UART_TX_BUF_SIZE
#define UART_TX_BUF_SIZE 128u /**< Default TX ring buffer size (power of two) */
#endif

#if (UART_TX_BUF_SIZE & (UART_TX_BUF_SIZE - 1u)) != 0
#error "UART_TX_BUF_SIZE must be a power of two (e.g., 32, 64, 128, 256)."
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

/**
 * @brief Advance USART1 transmission by at most one byte (non-blocking)
 * @note Must be called periodically to feed the UART from the ring buffer
 */
void uart_poll_tx(void);

/**
 * @brief Enqueue a single byte into the USART1 TX ring buffer (lossless)
 * @param[in] b Byte to enqueue
 * @note Blocks only while the buffer is full, polling uart_poll_tx() to make
 *       room - a byte is never dropped
 */
void uart_tx_enqueue_byte(uint8_t b);

/**
 * @brief Enqueue a null-terminated string (lossless)
 * @param[in] s Null-terminated string to enqueue
 * @return Number of characters enqueued
 */
int uart_write_str(const char* s);

/**
 * @brief Enqueue an integer as a decimal string (lossless)
 * @param[in] value Integer value to enqueue (full 32-bit range supported)
 * @return Number of characters enqueued
 */
int uart_write_int(int value);

/**
 * @brief Enqueue a byte as two uppercase hexadecimal digits
 * @param[in] b Byte to enqueue
 * @return Number of characters enqueued (always 2)
 */
int uart_write_hex(uint8_t b);

/**
 * @brief Blocking drain of the USART1 TX ring buffer
 * @note Waits until every enqueued byte has been shifted out (TC set).
 *       Use once at startup so banners are never truncated.
 */
void uart_tx_flush(void);

#endif // APP_H
