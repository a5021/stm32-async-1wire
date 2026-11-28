/* Standalone register check for the STM32F1 OW_BUS_DRIVE mapping.
 * The normal port-setup table covers the default MAX value; this executable is
 * built once for each class so every MODE10 encoding is checked independently.
 */
#include "hw_model.h"
#include "mock_target.h"
#include "ow_port.h"

#include <stdio.h>

#if !defined(STM32F1)
#error "test_bus_drive_f1.c requires the STM32F1 host mock"
#endif

static uint32_t expected_mode_bits(void) {
#if OW_BUS_DRIVE == OW_BUS_DRIVE_WEAK
    return GPIO_CRH_MODE10_0; /* 2 MHz */
#elif OW_BUS_DRIVE == OW_BUS_DRIVE_MEDIUM
    return GPIO_CRH_MODE10_1; /* 10 MHz */
#else
    return GPIO_CRH_MODE10; /* 50 MHz */
#endif
}

int main(void) {
    const uint32_t mode = expected_mode_bits();
    const uint32_t field = GPIO_CRH_MODE10 | GPIO_CRH_CNF10;
    const uint32_t open_drain = mode | GPIO_CRH_CNF10_0 | GPIO_CRH_CNF10_1;
    const uint32_t push_pull = mode | GPIO_CRH_CNF10_1;

    hw_reset_all();
    ow_port_init();
    if ((mock_gpioa.CRH & field) != open_drain) {
        printf("F1 OW_BUS_DRIVE=%d: open-drain CRH mismatch\n", OW_BUS_DRIVE);
        return 1;
    }

    ow_port_set_pin_mode(1u);
    if ((mock_gpioa.CRH & field) != push_pull) {
        printf("F1 OW_BUS_DRIVE=%d: push-pull CRH mismatch\n", OW_BUS_DRIVE);
        return 1;
    }

    ow_port_set_pin_mode(0u);
    if ((mock_gpioa.CRH & field) != open_drain) {
        printf("F1 OW_BUS_DRIVE=%d: open-drain restore mismatch\n", OW_BUS_DRIVE);
        return 1;
    }

    printf("F1 OW_BUS_DRIVE=%d: MODE10/CNF10 mapping OK\n", OW_BUS_DRIVE);
    return 0;
}
