#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

extern int main(void);

void Reset_Handler(void);
void Default_Handler(void);

void NMI_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void)    __attribute__((weak, alias("Default_Handler")));

extern void USART2_IRQHandler(void);
extern void DMA1_Stream6_IRQHandler(void);

__attribute__((section(".isr_vector")))
void (* const vector_table[])(void) =
{
    (void (*)(void))&_estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0,
    0,
    0,
    0,
    SVC_Handler,
    DebugMon_Handler,
    0,
    PendSV_Handler,
    SysTick_Handler,
    /* IRQ0..IRQ37: slots filled as milestones need them (RM0368 Table 38). */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                  /* IRQ0..IRQ9   */
    0, 0, 0, 0, 0, 0, 0, DMA1_Stream6_IRQHandler,  /* IRQ10..IRQ17 (17 = DMA1_Stream6) */
    0, 0,                                          /* IRQ18..IRQ19 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                  /* IRQ20..IRQ29 */
    0, 0, 0, 0, 0, 0, 0, 0,                        /* IRQ30..IRQ37 */
    USART2_IRQHandler,  /* IRQ38 */
};

void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }

    dst = &_sbss;
    while (dst < &_ebss) {
        *dst++ = 0;
    }

    main();

    while (1) { }
}

void Default_Handler(void)
{
    while (1) { }
}
