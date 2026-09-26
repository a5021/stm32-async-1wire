/* ============================================================
 *  port_setup_probe.h - family-neutral view of the port's setup state
 *
 *  The state ow_port_init() programs: clock gates, timer prescaler and the
 *  bus-pin configuration. It is a flat struct with one field per concern rather
 *  than a register dump, because the fields that exist differ per family - F1
 *  has no MODER/OTYPER/OSPEEDR and no SYSCFG, F4 has no SYSCFG either - and a
 *  test that wants to know "what did the bus pin end up as" should not have to
 *  care which CMSIS dialect answers.
 *
 *  Shared by test_port_init_contract.c, which pins the values, and
 *  test_state_machine.c, which checks that ds18b20_init() reaches ow_port_init()
 *  and changes nothing beyond it.
 * ============================================================ */

#ifndef PORT_SETUP_PROBE_H
#define PORT_SETUP_PROBE_H

#include "mock_target.h"
#include "ow_port.h"
#include <stdint.h>

typedef struct {
    uint32_t clk; /* GPIOA + TIM1 + DMA clock gates, packed */
    uint32_t psc; /* TIM1 prescaler */
    uint32_t bdtr; /* TIM1 BDTR */
    uint32_t pin_mode; /* CRH on F1, MODER elsewhere */
    uint32_t pin_otype; /* OTYPER where the family has one, else 0 */
    uint32_t pin_af; /* AFR[1] where the family has one, else 0 */
    uint32_t pin_speed; /* the drive-strength field: OSPEEDR, or CRH MODE10 on F1 */
    uint32_t remap; /* SYSCFG pad remap where the family has one, else 0 */
} port_setup_t;

port_setup_t port_setup_snapshot(void);

#endif /* PORT_SETUP_PROBE_H */
