/* ============================================================
 *  test_app_uart.c - Application-layer UART TX ring buffer
 *
 *  Only the non-blocking ring buffer (uart_tx_enqueue_byte / uart_poll_tx /
 *  uart_flush / uart_write_str / uart_write_int / uart_write_hex) is compiled
 *  into the host test build; the hardware bring-up half of app.c is excluded
 *  by DS18B20_TEST_HARNESS.  These tests exercise that surface through the
 *  mocked USART1 peripheral.
 * ============================================================ */

#include "app.h"
#if defined(OW_PORT_TARGET_F0)
#include "stm32f0xx.h"
#elif defined(OW_PORT_TARGET_G0)
#include "stm32g0xx.h"
#else
#include "stm32f1xx.h"
#endif
#include "hw_model.h"
#include "unity.h"

#if defined(OW_PORT_TARGET_G0)
#define TXE_BIT USART_ISR_TXE_TXFNF
#define TX_SR ISR
#define TX_DR TDR
#elif defined(OW_PORT_TARGET_F0)
#define TXE_BIT USART_ISR_TXE
#define TX_SR ISR
#define TX_DR TDR
#else /* F1 */
#define TXE_BIT USART_SR_TXE
#define TX_SR SR
#define TX_DR DR
#endif

/* Drain any bytes left in the static ring buffer from earlier tests. */
static void uart_drain(void) {
    mock_usart1.TX_SR |= TXE_BIT;
    uart_flush();
}

/* Pop one enqueued byte (drives the mocked USART TX data register). */
static uint8_t uart_pop(void) {
    mock_usart1.TX_SR |= TXE_BIT;
    uart_poll_tx();
    return (uint8_t)mock_usart1.TX_DR;
}

static void expect_str(const char* s) {
    for (const char* p = s; *p; p++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)*p, uart_pop());
    }
}

void test_uart_write_str_transmits(void) {
    uart_drain();
    TEST_ASSERT_EQUAL_INT(2, uart_write_str("Hi"));
    expect_str("Hi");
}

void test_uart_write_int_zero_and_negative(void) {
    uart_drain();
    TEST_ASSERT_EQUAL_INT(1, uart_write_int(0));
    expect_str("0");

    uart_drain();
    TEST_ASSERT_EQUAL_INT(3, uart_write_int(-42));
    expect_str("-42");
}

void test_uart_write_int_positive(void) {
    uart_drain();
    TEST_ASSERT_EQUAL_INT(3, uart_write_int(123));
    expect_str("123");
}

void test_uart_write_hex(void) {
    uart_drain();
    TEST_ASSERT_EQUAL_INT(2, uart_write_hex(0xA5));
    expect_str("A5");
}

void test_uart_buffer_full_drops_byte(void) {
    uart_drain();
    int filled = 0;
    while (uart_tx_enqueue_byte('X') == 1) {
        filled++;
    }
    TEST_ASSERT_EQUAL_INT((int)(UART_TX_BUF_SIZE - 1u), filled);
    /* Full buffer: extra enqueue is dropped (returns 0). */
    TEST_ASSERT_EQUAL_INT(0, uart_tx_enqueue_byte('X'));

    /* Leave exactly one free slot, then a too-long string is partially queued
     * (uart_write_str breaks out once the buffer fills). */
    mock_usart1.TX_SR |= TXE_BIT;
    uart_poll_tx();
    TEST_ASSERT_EQUAL_INT(1, uart_write_str("ZZ"));
    uart_drain();
}

void run_test_app_uart(void) {
    TEST_RUN(test_uart_write_str_transmits);
    TEST_RUN(test_uart_write_int_zero_and_negative);
    TEST_RUN(test_uart_write_int_positive);
    TEST_RUN(test_uart_write_hex);
    TEST_RUN(test_uart_buffer_full_drops_byte);
}
