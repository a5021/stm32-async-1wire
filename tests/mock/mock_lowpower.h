/* Shared host stubs for the opt-in low-power WFE path (OW_LOW_POWER).
 *
 * Each CMSIS mock defines MOCK_TIM1_UP_IRQ_VALUE (the family's update-vector
 * number, irrelevant on the host) and aliases its own TIM1_*_IRQn name to
 * MOCK_TIM1_UP_IRQn after including this header. The stubs compile only into
 * low-power test builds, so the default busy-poll builds stay byte-identical.
 */
#ifndef MOCK_LOWPOWER_H
#define MOCK_LOWPOWER_H

#if OW_LOW_POWER

#define MOCK_TIM1_UP_IRQn MOCK_TIM1_UP_IRQ_VALUE

#define SCB_SCR_SEVONPEND_Msk 0x00000010u

typedef struct {
    volatile uint32_t SCR;
} SCB_Type;
extern SCB_Type mock_scb;
#define SCB (&mock_scb)

#define __SEV() ((void)0)
#ifdef MOCK_WFE_HOOK
/* Route the sleep through a host hook so a test can model the update event
 * arriving asynchronously (mock_wfe_set_hook). Without the define this stays
 * the plain no-op and every existing low-power build is unchanged. */
void mock_wfe_hook(void);
#define __WFE() mock_wfe_hook()
#else
#define __WFE() ((void)0)
#endif

#define NVIC_ClearPendingIRQ(__IRQn) ((void)(__IRQn))

#endif /* OW_LOW_POWER */

#endif /* MOCK_LOWPOWER_H */
