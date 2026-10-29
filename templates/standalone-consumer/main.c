/**
 * @file main.c
 * @brief Standalone-consumer template: one DS18B20, Skip ROM, over the console
 *
 * This is the smallest complete application that uses the library as a vendored
 * dependency: board.c brings up the clock and the console, ds18b20_init() brings
 * up the 1-Wire engine (onewire_init() calls ow_port_init() internally), and the
 * main loop is a non-blocking poll. There is no interrupt anywhere.
 *
 * The driver measures only when asked: ds18b20_start_measure() runs one
 * conversion + read cycle, the result arrives in ds18b20_complete(), and the
 * driver parks. The pause to the next cycle is counted from that result here, so
 * the loop does nothing but poll the driver and ask for the next cycle when the
 * deadline passes.
 */

#include "board.h"
#include "ds18b20.h"
#include "onewire.h"

#include "stm32h5xx.h"

/** Time between two measurement cycles (ms) */
#define MEASURE_PERIOD_MS 5000u

/** board_millis() value at which the pause after the last result expires */
static uint32_t pause_end_ms;

/**
 * @brief DS18B20 measurement completion callback (strong override of the weak
 *        driver default)
 * @param[in] temp Temperature in tenths of degrees Celsius, or an error code
 */
void ds18b20_complete(int16_t temp) {
    if (temp == DS18B20_TEMP_ERROR_NO_SENSOR) {
        board_puts("DS18B20 error: no sensor detected.\r\n");
    } else if (temp == DS18B20_TEMP_ERROR_CRC_FAIL) {
        board_puts("DS18B20 error: CRC check failed.\r\n");
    } else if (temp == DS18B20_TEMP_ERROR_GENERIC) {
        board_puts("DS18B20 error: generic failure.\r\n");
    } else {
        int whole = temp / 10;
        int frac = temp % 10;
        if (frac < 0) {
            frac = -frac;
        }
        board_puts("Temperature: ");
        if (whole == 0 && temp < 0) {
            board_puts("-0");
        } else {
            board_put_int(whole);
        }
        board_puts(".");
        board_put_int(frac);
        board_puts(" C\r\n");
    }
    pause_end_ms = board_millis() + MEASURE_PERIOD_MS;
}

int main(void) {
    board_init();
    board_puts("\r\nstm32-async-1wire standalone consumer starting...\r\n");

    /* The archive and the application must agree on the build flags and on
     * the clock; either mismatch fails silently as a dead bus. Fail loudly
     * here instead, before the driver owns the timer. */
    if (OW_CONFIG_TOKEN != onewire_config_token()) {
        board_puts("Config mismatch: rebuild library and app with the same OW_ flags.\r\n");
        for (;;) {
        }
    }
    SystemCoreClockUpdate();
    if (!onewire_clock_ok(SystemCoreClock / 1000000u)) {
        board_puts("Clock mismatch: OW_SYSCLK_MHZ does not match the running SYSCLK.\r\n");
        for (;;) {
        }
    }

    ds18b20_init();
#if OW_PARASITE_POWER
    ds18b20_set_parasite(1); /* devices are powered from the data line */
#endif

    pause_end_ms = board_millis() + MEASURE_PERIOD_MS;
    ds18b20_start_measure();

    for (;;) {
        ds18b20_poll();
        if ((int32_t)(board_millis() - pause_end_ms) >= 0) {
            pause_end_ms = board_millis() + MEASURE_PERIOD_MS;
            ds18b20_start_measure();
        }
    }
}
