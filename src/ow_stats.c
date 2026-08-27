/**
 * @file ow_stats.c
 * @brief 1-Wire signal statistics implementation.
 * @see ow_stats.h for the public API.
 */

#include "ow_stats.h"

#ifdef OW_STATS_ENABLE

#include <string.h>
#include "app.h"

#if defined(OW_PORT_TARGET_G0)
#include "stm32g0xx.h"
#elif defined(OW_PORT_TARGET_F0)
#include "stm32f0xx.h"
#else
#include "stm32f1xx.h"
#endif

/* ---- private state ---- */

static ow_stats_t st;

/** Non-blocking dump state. */
static uint8_t dump_phase;    /**< 0 = idle, 1+ = active */
static uint8_t dump_sensor;   /**< current sensor index */
static uint8_t dump_subpos;   /**< sub-position within current section */

/* ---- UART helpers (blocking, paced to baud rate) ---- */

static void ow_tx_char(char c) {
#if defined(OW_PORT_TARGET_G0)
    while (!(USART1->ISR & USART_ISR_TXE_TXFNF)) {}
    USART1->TDR = (uint8_t)c;
#elif defined(OW_PORT_TARGET_F0)
    while (!(USART1->ISR & USART_ISR_TXE)) {}
    USART1->TDR = (uint8_t)c;
#else
    while (!(USART1->SR & USART_SR_TXE)) {}
    USART1->DR = (uint8_t)c;
#endif
}

static void ow_tx_str(const char *s) {
    while (*s) ow_tx_char(*s++);
}

static void ow_tx_hex(uint8_t b) {
    static const char hex[] = "0123456789ABCDEF";
    ow_tx_char(hex[(b >> 4) & 0x0F]);
    ow_tx_char(hex[b & 0x0F]);
}

static void ow_tx_int(int value) {
    char buf[12];
    char *p = buf + sizeof(buf) - 1;
    *p = '\0';
    if (value == 0) {
        *(--p) = '0';
    } else {
        unsigned int uv = (value < 0) ? (unsigned int)(-(value + 1)) + 1
                                      : (unsigned int)value;
        do { *(--p) = '0' + (uv % 10); uv /= 10; } while (uv);
        if (value < 0) *(--p) = '-';
    }
    ow_tx_str(p);
}

/* ---- histogram helpers ---- */

static uint8_t hist_bucket(uint8_t pulse) {
    if (pulse <= 2)  return 0;
    if (pulse <= 4)  return 1;
    if (pulse <= 6)  return 2;
    if (pulse <= 9)  return 3;
    if (pulse <= 12) return 4;
    if (pulse <= 14) return 5;
    if (pulse <= 19) return 6;
    if (pulse <= 24) return 7;
    if (pulse <= 29) return 8;
    if (pulse <= 39) return 9;
    if (pulse <= 49) return 10;
    if (pulse <= 59) return 11;
    return 12;
}

/* ---- sensor lookup ---- */

static uint8_t sensor_find_or_alloc(const uint8_t *rom) {
    if (!rom) return OW_STATS_MAX_SENSORS;
    for (uint8_t i = 0; i < st.sensor_count; i++) {
        if (memcmp(st.sensors[i].rom, rom, 8) == 0) return i;
    }
    if (st.sensor_count < OW_STATS_MAX_SENSORS) {
        uint8_t idx = st.sensor_count++;
        memcpy(st.sensors[idx].rom, rom, 8);
        st.sensors[idx].min_pulse = 0xFF;
        st.sensors[idx].max_pulse = 0;
        return idx;
    }
    return OW_STATS_MAX_SENSORS;
}

/* ---- public API ---- */

void ow_stats_init(void) {
    memset(&st, 0, sizeof(st));
    dump_phase = 0;
}

void ow_stats_capture_pulse(const volatile uint8_t *pulse, uint8_t n,
                            const uint8_t *rom) {
    uint8_t si = sensor_find_or_alloc(rom);
    for (uint8_t i = 0; i < n; i++) {
        uint8_t p = pulse[i];
        st.histogram[hist_bucket(p)]++;
        if (si < OW_STATS_MAX_SENSORS) {
            if (p < st.sensors[si].min_pulse) st.sensors[si].min_pulse = p;
            if (p > st.sensors[si].max_pulse) st.sensors[si].max_pulse = p;
        }
    }
    if (si < OW_STATS_MAX_SENSORS) {
        st.sensors[si].count++;
    }
}

void ow_stats_count_error(int16_t error, const uint8_t *rom) {
    (void)error;
    uint8_t si = sensor_find_or_alloc(rom);
    if (si < OW_STATS_MAX_SENSORS) {
        st.sensors[si].generic_err++;
    }
    st.total_errors++;
}

void ow_stats_dump_start(void) {
    dump_phase = 1;
    dump_sensor = 0;
    dump_subpos = 0;
}

uint8_t ow_stats_dump_poll(void) {
    if (dump_phase == 0) return 1;

    /* Each poll call outputs one section.  The ow_tx_* helpers block on TXE,
     * so the output is paced to the UART baud rate (~87 µs/byte at 115200).
     * For 6 sensors the whole dump takes ~30 ms — acceptable. */
    switch (dump_phase) {
    case 1:
        ow_tx_str("--- stats [");
        ow_tx_int(st.total_cycles);
        ow_tx_str(" c] ---\r\n");
        dump_phase = 2;
        dump_sensor = 0;
        break;

    case 2:
        if (dump_sensor < st.sensor_count) {
            const ow_stats_sensor_t *s = &st.sensors[dump_sensor];
            for (uint8_t j = 0; j < 8; j++) {
                ow_tx_hex(s->rom[j]);
                if (j != 7) ow_tx_char(' ');
            }
            ow_tx_char(':');
            ow_tx_int(s->min_pulse);
            ow_tx_char('-');
            ow_tx_int(s->max_pulse);
            ow_tx_char(' ');
            ow_tx_char('n');
            ow_tx_int(s->count);
            ow_tx_char(' ');
            ow_tx_char('e');
            ow_tx_int(s->crc_err + s->no_presence + s->generic_err);
            ow_tx_str("\r\n");
            dump_sensor++;
        } else {
            dump_phase = 3;
        }
        break;

    case 3:
        ow_tx_str("h:");
        for (uint8_t i = 0; i < OW_STATS_HIST_BUCKETS; i++) {
            if (st.histogram[i]) {
                ow_tx_int(i);
                ow_tx_char('=');
                ow_tx_int(st.histogram[i]);
                ow_tx_char(' ');
            }
        }
        ow_tx_str("\r\n");
        dump_phase = 4;
        break;

    case 4:
        ow_tx_str("t=");
        ow_tx_int(st.total_cycles);
        ow_tx_str("c ");
        ow_tx_int(st.total_errors);
        ow_tx_str("e\r\n");
        dump_phase = 0;
        break;
    }

    return (dump_phase == 0) ? 1 : 0;
}

void ow_stats_reset(void) {
    uint8_t n = st.sensor_count;
    uint8_t roms[OW_STATS_MAX_SENSORS][8];
    for (uint8_t i = 0; i < n; i++) {
        memcpy(roms[i], st.sensors[i].rom, 8);
    }
    memset(&st, 0, sizeof(st));
    for (uint8_t i = 0; i < n; i++) {
        memcpy(st.sensors[i].rom, roms[i], 8);
        st.sensors[i].min_pulse = 0xFF;
    }
    st.sensor_count = n;
}

uint16_t ow_stats_tick(void) {
    return ++st.total_cycles;
}

#endif /* OW_STATS_ENABLE */
