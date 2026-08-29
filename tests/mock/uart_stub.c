/**
 * @file uart_stub.c
 * @brief Minimal no-op UART stubs for fuzz targets.
 *
 * ow_stats.c references uart_write_*() via app.h.  Fuzz harnesses never
 * call ow_stats_dump_poll(), but the linker still needs the symbols.
 */

#include <stdint.h>

static uint8_t dummy_buf[1024];
static uint32_t dummy_head;

int uart_tx_enqueue_byte(int b) {
    if (dummy_head < sizeof(dummy_buf)) dummy_buf[dummy_head++] = (uint8_t)b;
    return 1;
}

int uart_write_str(const char* s) {
    int n = 0;
    while (*s && uart_tx_enqueue_byte(*s)) {
        s++;
        n++;
    }
    return n;
}

int uart_write_int(int value) {
    char buf[12];
    int i = 0;
    if (value < 0) {
        uart_tx_enqueue_byte('-');
        value = -value;
    }
    if (value == 0) {
        uart_tx_enqueue_byte('0');
        return 1;
    }
    while (value > 0) {
        buf[i++] = '0' + (value % 10);
        value /= 10;
    }
    int n = 0;
    while (i > 0) {
        uart_tx_enqueue_byte(buf[--i]);
        n++;
    }
    return n + (value < 0 ? 1 : 0);
}

int uart_write_hex(uint8_t b) {
    static const char hex[] = "0123456789abcdef";
    uart_tx_enqueue_byte(hex[(b >> 4) & 0x0F]);
    uart_tx_enqueue_byte(hex[b & 0x0F]);
    return 2;
}

void uart_poll_tx(void) { dummy_head = 0; }
