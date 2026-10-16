/**
 * @file board.h
 * @brief Minimal H503 board support for the standalone-consumer template
 *
 * The library itself carries the whole 1-Wire port (inc/ow_port.h is a header
 * of static inline register writes), so a consumer needs no glue beyond a
 * system clock, a console and a millisecond tick. This file is that glue for
 * the WeAct STM32H503CB: 64MHz raw HSI, USART1 TX on PA9 at 115200 8N1, and a
 * polled 1 kHz SysTick with its interrupt disabled.
 *
 * It is deliberately tiny and self-contained - copy it into your own project
 * and replace it with your board's bring-up. Nothing here is specific to the
 * 1-Wire driver.
 *
 * Layout assumed: USART1 TX = PA9 (AF7), 1-Wire bus = PA10 (TIM1_CH3, taken by
 * the port), so the console and the bus share port A without colliding.
 */

#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the system clock (64MHz raw HSI), the console and the tick
 * @note Call once, before ds18b20_init(). Programs the flash latency and clears
 *       the H5 HSI divider - the reset core runs at 32MHz (HSIDIV=/2), which
 *       would scale every 1-Wire timing by two if left alone.
 */
void board_init(void);

/**
 * @brief Milliseconds since board_init(), from the polled SysTick counter
 * @return Counter in ms (wraps after ~49 days)
 * @note Non-blocking: one register read, no interrupt. Must be polled at least
 *       once per millisecond - any main loop does.
 */
uint32_t board_millis(void);

/** @brief Blocking USART1 TX of one character */
void board_putc(char c);

/** @brief Blocking USART1 TX of a null-terminated string */
void board_puts(const char* s);

/** @brief Blocking USART1 TX of a signed integer in decimal */
void board_put_int(int32_t v);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_H */
