#ifndef MOCK_PORT_HOOKS_H
#define MOCK_PORT_HOOKS_H
/* Host stand-in for the port's timer start.
 *
 * The port starts the timer through OW_PORT_TIMER_START() so that the model
 * sees the exact moment the timer begins to run. That moment is the only
 * place where the *order* of the register writes is visible: the final state
 * of the registers is the same whether the driver armed the DMA before
 * starting the timer or after.
 *
 * The check the model makes there is the one the final state cannot show:
 * if the timer says it will use a DMA channel (the DIER enable bits), that
 * channel must be armed. An edge that arrives while the channel is still
 * disarmed is lost, and the first slot of the operation would silently
 * capture nothing.
 *
 * This header is included by the mock device headers, so the override is in
 * place before ow_port.h reaches its own default. */
#include "hw_model.h"

#define OW_PORT_TIMER_START()   \
    do {                        \
        hw_model_timer_start(); \
    } while (0)

#endif /* MOCK_PORT_HOOKS_H */
