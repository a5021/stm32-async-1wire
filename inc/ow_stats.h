/**
 * @file ow_stats.h
 * @brief 1-Wire signal statistics — optional compile-in module.
 *
 * Collects per-sensor pulse-width statistics, a global pulse histogram,
 * and error counters.  Enabled by defining OW_STATS_ENABLE at build time
 * (e.g. -DOW_STATS_ENABLE).  When the macro is not defined, every inline
 * body compiles away to nothing so there is zero overhead in production
 * builds.
 *
 * The dump is non-blocking and platform-independent: output goes through
 * an injectable sink (ow_stats_set_sink), ow_stats_dump_start() initiates
 * it, ow_stats_dump_poll() emits a few bytes per main-loop call.
 *
 * @note RAM cost: ~290 bytes (8 sensors × 26 B + 16-entry uint32_t histogram
 *       [64 B] + cycle/error counters; 13 of the 16 histogram buckets, indices
 *       0–12, are populated).
 */

#ifndef OW_STATS_H
#define OW_STATS_H

#include <stdint.h>

/**
 * @brief Output sink: non-blocking UART-like callback table.
 *
 * The dump routine delegates all output through this table, keeping the
 * library free of platform dependencies.  Register before the first call
 * to ow_stats_dump_poll(); when unset the default no-op sink is used.
 */
typedef struct {
    int  (*write_str)(const char* s);
    int  (*write_int)(int value);
    int  (*write_hex)(uint8_t b);
    int  (*enqueue_byte)(int b);
    void (*poll_tx)(void);
} ow_stats_sink_t;

#ifdef OW_STATS_ENABLE

/** Maximum number of sensors tracked simultaneously. */
#define OW_STATS_MAX_SENSORS 8

/** Number of histogram buckets (logarithmic, 0–60+ us). */
#define OW_STATS_HIST_BUCKETS 16

/**
 * @brief Set the output sink for the stats dump.
 * @param[in] sink Pointer to a static sink table, or NULL for no-op output.
 */
void ow_stats_set_sink(const ow_stats_sink_t* sink);

/**
 * @brief Per-sensor statistics record.
 */
typedef struct {
    uint8_t rom[8]; /**< 64-bit ROM address (LSB first) */
    uint8_t min_pulse; /**< Shortest observed pulse width (us) */
    uint8_t max_pulse; /**< Longest  observed pulse width (us) */
    uint32_t count; /**< Successful scratchpad reads */
    uint32_t crc_err; /**< CRC-8 mismatches */
    uint32_t no_presence; /**< No presence pulse detected */
    uint32_t generic_err; /**< Unexpected / reserved-byte errors */
} ow_stats_sensor_t;

/**
 * @brief Aggregate statistics context.
 */
typedef struct {
    ow_stats_sensor_t sensors[OW_STATS_MAX_SENSORS];
    uint8_t sensor_count; /**< Number of known sensors */
    uint32_t histogram[OW_STATS_HIST_BUCKETS]; /**< Pulse-width distribution */
    uint32_t total_cycles; /**< Full measurement cycles completed */
    uint32_t total_errors; /**< Sum of all error counters */
} ow_stats_t;

/**
 * @brief Zero-initialise the statistics context.
 */
void ow_stats_init(void);

/**
 * @brief Snapshot raw pulse durations before decode_scratchpad().
 * @param[in] pulse Pointer to the DMA capture buffer (us per byte).
 * @param[in] n     Number of pulse slots (typically DS18B20_SCRATCHPAD_BITS = 72).
 * @param[in] rom   8-byte ROM of the current sensor, or NULL for Skip ROM.
 *
 * The function copies pulse data into the histogram and updates per-sensor
 * min/max counters.  Must be called BEFORE decode_scratchpad() overwrites
 * the buffer via the union alias.
 */
void ow_stats_capture_pulse(const volatile uint8_t* pulse, uint8_t n,
                            const uint8_t* rom);

/**
 * @brief Record an error event.
 * @param[in] error The ds18b20 error code (DS18B20_TEMP_ERROR_*).
 * @param[in] rom   8-byte ROM of the current sensor, or NULL for Skip ROM.
 */
void ow_stats_count_error(int16_t error, const uint8_t* rom);

/**
 * @brief Begin a non-blocking stats dump via the configured sink.
 *
 * Initiates the dump; call ow_stats_dump_poll() from the main loop to
 * advance it by a few bytes per iteration.  Returns immediately.
 */
void ow_stats_dump_start(void);

/**
 * @brief Advance the non-blocking stats dump by one line.
 * @return 1 when the dump is complete, 0 if more output remains.
 *
 * Outputs one sensor line (or the header/histogram/total) per call,
 * then calls the sink's poll_tx.  Call repeatedly from the main loop
 * until it returns 1.
 */
uint8_t ow_stats_dump_poll(void);

/**
 * @brief Reset all counters and histogram, keep sensor ROM table.
 */
void ow_stats_reset(void);

/**
 * @brief Increment the cycle counter (called from the demo callback).
 * @return The new cycle count.
 */
uint32_t ow_stats_tick(void);

#else /* OW_STATS_ENABLE not defined — zero-overhead stubs */

static inline void ow_stats_init(void) {}
static inline void ow_stats_set_sink(const ow_stats_sink_t* sink) {
    (void)sink;
}
static inline void ow_stats_capture_pulse(const volatile uint8_t* pulse,
                                          uint8_t n, const uint8_t* rom) {
    (void)pulse;
    (void)n;
    (void)rom;
}
static inline void ow_stats_count_error(int16_t error, const uint8_t* rom) {
    (void)error;
    (void)rom;
}
static inline void ow_stats_dump_start(void) {}
static inline uint8_t ow_stats_dump_poll(void) { return 1; }
static inline void ow_stats_reset(void) {}
static inline uint32_t ow_stats_tick(void) { return 0; }

#endif /* OW_STATS_ENABLE */

#endif /* OW_STATS_H */
