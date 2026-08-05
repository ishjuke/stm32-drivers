#include <stdint.h>

#define RCC_BASE     0x40023800UL
#define RCC_AHB1ENR  (*(volatile uint32_t *)(RCC_BASE + 0x30))
#define RCC_APB1ENR  (*(volatile uint32_t *)(RCC_BASE + 0x40))

#define GPIOA_BASE   0x40020000UL
#define GPIOA_MODER  (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_BSRR   (*(volatile uint32_t *)(GPIOA_BASE + 0x18))
#define GPIOA_AFRL   (*(volatile uint32_t *)(GPIOA_BASE + 0x20))

#define LD2_PIN      5

#define USART2_BASE  0x40004400UL
#define USART2_SR    (*(volatile uint32_t *)(USART2_BASE + 0x00))
#define USART2_DR    (*(volatile uint32_t *)(USART2_BASE + 0x04))
#define USART2_BRR   (*(volatile uint32_t *)(USART2_BASE + 0x08))
#define USART2_CR1   (*(volatile uint32_t *)(USART2_BASE + 0x0C))

#define USART_SR_TXE  (1U << 7)
#define USART_SR_RXNE (1U << 5)
#define USART_CR1_UE     (1U << 13)
#define USART_CR1_RXNEIE (1U << 5)
#define USART_CR1_TE     (1U << 3)
#define USART_CR1_RE     (1U << 2)

/* NVIC: IRQ38 (USART2, confirmed against ST's CMSIS stm32f401xe.h) falls
 * in ISER1, which covers IRQ32..63. Bit position = 38 - 32 = 6. */
#define NVIC_ISER1      (*(volatile uint32_t *)0xE000E104UL)
#define USART2_IRQn_BIT (1U << 6)

#define RB_SIZE 64   /* power of two on purpose: (head+1) & (RB_SIZE-1) replaces a modulo */
static volatile uint8_t  rb[RB_SIZE];
static volatile uint32_t head;  /* written only by the ISR (producer) */
static volatile uint32_t tail;  /* written only by main (consumer) */

/*
 * Single-producer/single-consumer, no critical section needed:
 *   - head is written only here (the ISR); tail is written only by main.
 *     Neither side ever writes the other's index, so there's no
 *     read-modify-write race on a shared variable.
 *   - head/tail are naturally-aligned 32-bit words on a 32-bit core: a
 *     Cortex-M4 LDR/STR of an aligned word is a single indivisible bus
 *     transaction, so the reader always sees a fully-old or fully-new
 *     value, never a torn one.
 *   - volatile is still required, but for a different reason: it stops
 *     the compiler from caching head/tail in a register across loop
 *     iterations. Without it, main's poll loop could read a
 *     register-cached tail forever and never notice the ISR moved it.
 * Disabling interrupts would only be necessary with a second writer on
 * either index (a second producer/consumer, or a compound RMW spanning
 * both indices), neither of which applies here.
 */

/*
 * Full/empty: one slot is sacrificed deliberately, rather than keeping a
 * separate count. head == tail means empty; the buffer is treated as
 * full when advancing head would make it equal tail. Simpler than a
 * count variable and avoids a second piece of state that could
 * desynchronize from head/tail.
 */

void USART2_IRQHandler(void)
{
    uint32_t sr = USART2_SR;

    /*
     * ORE (RM0368 19.6.1) only ever sets alongside RXNE on this family
     * (an overrun means the previous byte was never read, so RXNE was
     * already 1 when the new byte arrived and got discarded). Reading
     * SR then DR is the documented clear sequence for ORE, and this
     * branch already does exactly that as a side effect of the normal
     * receive path -- no separate ORE handling required, or the
     * peripheral would wedge after the first overrun.
     */
    if (sr & USART_SR_RXNE) {
        uint8_t c = (uint8_t)USART2_DR;
        uint32_t next = (head + 1) & (RB_SIZE - 1);
        if (next != tail) {
            rb[head] = c;
            head = next;
        }
        /* else: buffer full, incoming byte dropped (not overwritten) --
         * keeps already-buffered, not-yet-echoed bytes intact and in
         * order rather than corrupting them. */
    }
}

int main(void)
{
    RCC_AHB1ENR |= (1 << 0);   /* GPIOAEN */
    (void)RCC_AHB1ENR;

    RCC_APB1ENR |= (1 << 17);  /* USART2EN */
    (void)RCC_APB1ENR;

    /* PA2 -> AF7 (USART2_TX) */
    GPIOA_MODER &= ~(0x3 << (2 * 2));
    GPIOA_MODER |=  (0x2 << (2 * 2));
    GPIOA_AFRL  &= ~(0xF << (2 * 4));
    GPIOA_AFRL  |=  (0x7 << (2 * 4));

    /* PA3 -> AF7 (USART2_RX) */
    GPIOA_MODER &= ~(0x3 << (3 * 2));
    GPIOA_MODER |=  (0x2 << (3 * 2));
    GPIOA_AFRL  &= ~(0xF << (3 * 4));
    GPIOA_AFRL  |=  (0x7 << (3 * 4));

    /* PA5 -> general-purpose output (LD2 heartbeat) */
    GPIOA_MODER &= ~(0x3 << (LD2_PIN * 2));
    GPIOA_MODER |=  (0x1 << (LD2_PIN * 2));

    /*
     * fCK = 16 MHz (HSI, APB1 prescaler /1 at reset).
     * USARTDIV = 16e6 / (16 * 115200) = 8.6806
     * mantissa = 8, fraction = round(0.6806 * 16) = 11 (0xB)
     */
    USART2_BRR = (8 << 4) | 0xB;

    USART2_CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;

    NVIC_ISER1 = USART2_IRQn_BIT;

    uint8_t led_state = 0;

    while (1) {
        if (tail != head) {
            uint8_t c = rb[tail];
            tail = (tail + 1) & (RB_SIZE - 1);

            while (!(USART2_SR & USART_SR_TXE)) { }
            USART2_DR = c;

            if (led_state) {
                GPIOA_BSRR = (1 << (LD2_PIN + 16));
            } else {
                GPIOA_BSRR = (1 << LD2_PIN);
            }
            led_state = !led_state;
        }
    }
}
