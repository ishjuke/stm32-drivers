#include <stdint.h>

#define RCC_BASE     0x40023800UL
#define RCC_CR       (*(volatile uint32_t *)(RCC_BASE + 0x00))
#define RCC_PLLCFGR  (*(volatile uint32_t *)(RCC_BASE + 0x04))
#define RCC_CFGR     (*(volatile uint32_t *)(RCC_BASE + 0x08))
#define RCC_AHB1ENR  (*(volatile uint32_t *)(RCC_BASE + 0x30))
#define RCC_APB1ENR  (*(volatile uint32_t *)(RCC_BASE + 0x40))

#define GPIOA_BASE   0x40020000UL
#define GPIOA_MODER  (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_BSRR   (*(volatile uint32_t *)(GPIOA_BASE + 0x18))
#define GPIOA_AFRL   (*(volatile uint32_t *)(GPIOA_BASE + 0x20))

/* PB8/PB9 -> I2C1 SCL/SDA */
#define GPIOB_BASE   0x40020400UL
#define GPIOB_MODER  (*(volatile uint32_t *)(GPIOB_BASE + 0x00))
#define GPIOB_OTYPER (*(volatile uint32_t *)(GPIOB_BASE + 0x04))
#define GPIOB_AFRH   (*(volatile uint32_t *)(GPIOB_BASE + 0x24))

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

#define I2C1_BASE    0x40005400UL
#define I2C1_CR1     (*(volatile uint32_t *)(I2C1_BASE + 0x00))
#define I2C1_CR2     (*(volatile uint32_t *)(I2C1_BASE + 0x04))
#define I2C1_DR      (*(volatile uint32_t *)(I2C1_BASE + 0x10))
#define I2C1_SR1     (*(volatile uint32_t *)(I2C1_BASE + 0x14))
#define I2C1_SR2     (*(volatile uint32_t *)(I2C1_BASE + 0x18))
#define I2C1_CCR     (*(volatile uint32_t *)(I2C1_BASE + 0x1C))
#define I2C1_TRISE   (*(volatile uint32_t *)(I2C1_BASE + 0x20))

#define I2C_CR1_PE     (1U << 0)
#define I2C_CR1_START  (1U << 8)
#define I2C_CR1_STOP   (1U << 9)
#define I2C_CR1_ACK    (1U << 10)

#define I2C_SR1_SB    (1U << 0)
#define I2C_SR1_ADDR  (1U << 1)
#define I2C_SR1_BTF   (1U << 2)
#define I2C_SR1_RXNE  (1U << 6)
#define I2C_SR1_TXE   (1U << 7)

#define BME280_ADDR       0x76U   /* 7-bit; SDO tied low on the breakout */
#define BME280_ID_REG     0xD0U   /* chip-id register: reads 0x60 (BME280) or 0x58 (BMP280) */
#define BME280_STATUS_REG 0xF3U
#define BME280_CTRL_HUM_REG  0xF2U
#define BME280_CTRL_MEAS_REG 0xF4U
#define BME280_DATA_REG      0xF7U  /* press[3] temp[3] hum[2], one 8-byte burst (datasheet 5.4.7-5.4.9) */
#define BME280_DATA_LEN      8U

/* Calibration coefficients (datasheet 4.2.2, Table 16): two non-contiguous
 * blocks, calib00-calib25 and calib26-calib32 (of the calib00..calib41
 * range; only up through 0xE7/dig_H6 is needed here). */
#define BME280_CALIB1_REG 0x88U
#define BME280_CALIB1_LEN 26U
#define BME280_CALIB2_REG 0xE1U
#define BME280_CALIB2_LEN 7U

#define BME280_STATUS_MEASURING (1U << 3)  /* datasheet 5.4.4: 1 while a conversion is running */

/* Datasheet 5.4.3/5.4.5: osrs_h/osrs_t/osrs_p = 001 -> oversampling x1.
 * ctrl_meas: osrs_t[7:5] osrs_p[4:2] mode[1:0], mode = 11 -> normal. */
#define BME280_CTRL_HUM_VAL  0x01U
#define BME280_CTRL_MEAS_VAL 0x27U

#define I2C_TIMEOUT_ITERS 100000UL
#define BME280_STATUS_POLL_TRIES 200UL

/* Milestone 5 timed this by counting busy-wait loop iterations -- an
 * empirically-tuned ~1.62 us/iteration, not a real timer. SysTick now
 * gives a genuine millisecond tick, so this is a real period: 1.5 s,
 * the same target window the empirical tuning was aiming for so the
 * breath test still resolves a spike-and-decay curve instead of a
 * couple of blurred points. */
#define BME280_SAMPLE_PERIOD_MS 1500UL

/* NVIC: IRQ38 (USART2, confirmed against ST's CMSIS stm32f401xe.h) falls
 * in ISER1, which covers IRQ32..63. Bit position = 38 - 32 = 6. */
#define NVIC_ISER1      (*(volatile uint32_t *)0xE000E104UL)
#define USART2_IRQn_BIT (1U << 6)

/* Clock tree: HSI (16 MHz) through the main PLL to 84 MHz SYSCLK, this
 * part's maximum (RM0368 6.3.1/6.3.2/6.3.3). Bit positions/values below
 * are all cross-checked against RM0368 Rev 4 directly, not recalled --
 * getting the flash latency or VCO range wrong here is a hard fault,
 * not a wrong number. */
#define RCC_CR_PLLON  (1U << 24)
#define RCC_CR_PLLRDY (1U << 25)

/* RCC_PLLCFGR (RM0368 6.3.2): PLLM/PLLN/PLLQ take their divisor/
 * multiplier value directly; PLLP is 2 bits encoding {2,4,6,8} as
 * {00,01,10,11}. PLLSRC (bit 22) is left at its reset value of 0 (HSI)
 * -- this board has never had HSE configured. */
#define RCC_PLLCFGR_PLLM_POS 0
#define RCC_PLLCFGR_PLLN_POS 6
#define RCC_PLLCFGR_PLLP_POS 16
#define RCC_PLLCFGR_PLLQ_POS 24

#define RCC_CFGR_SW_MASK     (0x3U)
#define RCC_CFGR_SW_PLL      (0x2U)
#define RCC_CFGR_SWS_MASK    (0x3U << 2)
#define RCC_CFGR_SWS_PLL     (0x2U << 2)
#define RCC_CFGR_PPRE1_MASK  (0x7U << 10)
#define RCC_CFGR_PPRE1_DIV2  (0x4U << 10)

#define RCC_APB1ENR_PWREN (1U << 28)

/* PWR: RM0368 5.5, base from the memory map (5.5, "0x4000 7000 -
 * 0x4000 73FF PWR"). Needed only for the voltage-scaling write in
 * clock_init() -- its clock has to be enabled first via
 * RCC_APB1ENR_PWREN, same precondition as any other APB1 peripheral in
 * this file. */
#define PWR_BASE 0x40007000UL
#define PWR_CR   (*(volatile uint32_t *)(PWR_BASE + 0x00))
#define PWR_CR_VOS        (0x3U << 14)
#define PWR_CR_VOS_SCALE2 (0x2U << 14)  /* RM0368 3.4.1: VOS[1:0]=10 -> max fHCLK = 84 MHz */

/* Flash: RM0368 3.8, base from the memory map (3.8, "0x4002 3C00 -
 * 0x4002 3FFF"). */
#define FLASH_BASE 0x40023C00UL
#define FLASH_ACR  (*(volatile uint32_t *)(FLASH_BASE + 0x00))
#define FLASH_ACR_LATENCY     (0xFU)
#define FLASH_ACR_LATENCY_2WS (0x2U)  /* RM0368 Table 6: 2 WS for 60 < HCLK <= 84 MHz at 2.7-3.6 V */

/* SysTick: PM0214 4.5. Part of the Cortex-M4 core (System Control
 * Space), not a peripheral behind an RCC clock-enable bit. */
#define SYSTICK_BASE 0xE000E010UL
#define SYST_CSR (*(volatile uint32_t *)(SYSTICK_BASE + 0x00))
#define SYST_RVR (*(volatile uint32_t *)(SYSTICK_BASE + 0x04))
#define SYST_CVR (*(volatile uint32_t *)(SYSTICK_BASE + 0x08))

#define SYST_CSR_ENABLE    (1U << 0)
#define SYST_CSR_TICKINT   (1U << 1)
#define SYST_CSR_CLKSOURCE (1U << 2)  /* 1 = core clock, 0 = core clock / 8 */

/* fCK = 84 MHz post-clock_init() (the clock-tree extension: HSI through
 * the PLL, up from the milestone-6 16 MHz HSI-direct baseline).
 * CLKSOURCE is set below (core clock, not /8), so the reload value is
 * derived directly from SYSTICK_HZ -- the two have to change together,
 * which is exactly what happened here. */
#define SYSTICK_HZ 84000000UL

/* The counter reloads on the tick after it hits zero: reload value N
 * gives an actual period of N+1 cycles (ARM's documented behavior), so
 * a period of P cycles needs P-1 loaded, not P. Getting this wrong
 * doesn't fail loudly -- it just skews the clock by 1 cycle per period.
 * The intuitive mistake is forgetting the -1 (loading 84000 instead of
 * 83999): that makes the *period* longer, not shorter, so every "1 ms"
 * tick actually takes ~0.001% longer than 1 ms, and the clock runs
 * slow, not fast -- invisible in testing, wrong forever. */
#define SYSTICK_RELOAD_1MS (SYSTICK_HZ / 1000UL - 1UL)

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

/* Raises SYSCLK from the 16 MHz HSI reset default to 84 MHz (this
 * part's maximum) through the main PLL. Must run before anything that
 * derives a divisor from the clock -- SysTick's reload below, and
 * USART2/I2C1's BRR/CCR/TRISE in main() -- so it's the first thing
 * main() calls. Follows RM0368 3.4.1's "increasing the CPU frequency"
 * procedure: raise flash latency first, then switch. */
static void clock_init(void)
{
    /* 2 WS: RM0368 Table 6, 60 < HCLK <= 84 MHz at 2.7-3.6 V (this
     * board measures 3.25 V). Getting this wrong doesn't slow the chip
     * down -- flash can't supply instructions fast enough and the CPU
     * runs off into garbage, an immediate hard fault. */
    FLASH_ACR = (FLASH_ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_2WS;

    /* PWR_CR needs its own APB1 clock enabled before it's writable
     * (RM0368 5.1.3), the same precondition as any other APB1
     * peripheral in this file. VOS already resets to Scale 2 -- which
     * RM0368 3.4.1 lists as good for up to 84 MHz, exactly what's
     * needed here -- so this write is redundant with the reset state,
     * but it's written explicitly rather than relied on implicitly.
     * (Scale 1 doesn't exist on this part; the only other option,
     * Scale 3, tops out at 60 MHz. Per RM0368 5.1.3 the regulator is
     * forced to Scale 3 regardless of VOS whenever the PLL is off, so
     * this value only actually takes effect once PLLON is set below --
     * no VOSRDY poll needed here since the value never actually
     * transitions.) */
    RCC_APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC_APB1ENR;
    PWR_CR = (PWR_CR & ~PWR_CR_VOS) | PWR_CR_VOS_SCALE2;

    /* PLLM=8: VCO input = 16 MHz / 8 = 2 MHz, the recommended value
     * (RM0368 6.3.2: "select 2 MHz to limit PLL jitter"; valid range is
     * 1-2 MHz).
     * PLLN=168: VCO output = 2 MHz * 168 = 336 MHz (must land in
     * 192-432 MHz, RM0368 6.3.2).
     * PLLP=/4 (encoded 0b01 below): SYSCLK = 336 / 4 = 84 MHz.
     * PLLQ=7: 336 / 7 = 48 MHz -- unused (no USB/SDIO/RNG wired up
     * yet), but set to a real, valid value rather than left at a
     * "wrong configuration" encoding.
     * All of these, plus PLLSRC, can only be written while the PLL is
     * off -- true here, since it's still off from reset. */
    RCC_PLLCFGR = (8UL   << RCC_PLLCFGR_PLLM_POS)
                | (168UL << RCC_PLLCFGR_PLLN_POS)
                | (0x1UL << RCC_PLLCFGR_PLLP_POS)   /* 0b01 = /4 */
                | (7UL   << RCC_PLLCFGR_PLLQ_POS);  /* PLLSRC left at 0 = HSI */

    RCC_CR |= RCC_CR_PLLON;
    while (!(RCC_CR & RCC_CR_PLLRDY)) { }

    /* APB1 maxes out at 42 MHz (RM0368 6.2), so it needs /2 from an
     * 84 MHz HCLK; APB2 and AHB both max out at 84 MHz, so HPRE and
     * PPRE2 stay at their reset default of /1 -- untouched below.
     * PPRE1 and SW are set in the same write so APB1 is already
     * dividing by 2 in the same bus cycle SYSCLK becomes 84 MHz -- no
     * window where APB1 briefly sees undivided 84 MHz. */
    RCC_CFGR = (RCC_CFGR & ~(RCC_CFGR_PPRE1_MASK | RCC_CFGR_SW_MASK))
             | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_SW_PLL;
    while ((RCC_CFGR & RCC_CFGR_SWS_MASK) != RCC_CFGR_SWS_PLL) { }
}

/* Written only here (interrupt context), read only in main -- volatile
 * for the same reason as the ring buffer's head/tail: it stops the
 * compiler from caching a stale value in a register across loop
 * iterations, not because the read/write itself needs to be atomic. */
static volatile uint32_t ticks;

void SysTick_Handler(void)
{
    ticks++;
}

static void systick_init(void)
{
    SYST_RVR = SYSTICK_RELOAD_1MS;
    SYST_CVR = 0;   /* writing any value clears CVR and COUNTFLAG -- start from a known state, not whatever reset left behind */
    SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
}

static uint32_t millis(void)
{
    return ticks;
}

/* ticks wraps every ~49.7 days at 1 ms/tick (2^32 ms). Unsigned
 * subtraction wraps the same way ticks does, so (now - start) stays
 * correct across that rollover for any ms up to ~2^31; `now >= start +
 * ms` is not -- start + ms can itself silently wrap and compare wrong
 * right at the boundary. */
static void delay_ms(uint32_t ms)
{
    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < ms) { }
}

static void uart_putc(uint8_t c)
{
    while (!(USART2_SR & USART_SR_TXE)) { }
    USART2_DR = c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        uart_putc((uint8_t)*s++);
    }
}

static void uart_put_hex_nibbles(uint8_t b)
{
    static const char digits[] = "0123456789ABCDEF";
    uart_putc((uint8_t)digits[b >> 4]);
    uart_putc((uint8_t)digits[b & 0xF]);
}

static void uart_put_hex_byte(uint8_t b)
{
    uart_puts("0x");
    uart_put_hex_nibbles(b);
}

/*
 * Every I2C1 wait point below polls SR1 for one flag. A bare
 * `while (!(flag)) {}` hangs forever if the slave never ACKs or a wire is
 * swapped; a timeout that names the flag that never arrived turns "board
 * looks dead" into "ADDR never set -- check the address byte or wiring".
 * Ten extra lines here buys that diagnosis for every wait site below.
 */
static int i2c1_wait(uint32_t flag, const char *label)
{
    uint32_t timeout = I2C_TIMEOUT_ITERS;
    while (!(I2C1_SR1 & flag)) {
        if (--timeout == 0) {
            uint32_t sr1 = I2C1_SR1;
            uint32_t sr2 = I2C1_SR2;   /* reading SR2 here (after SR1) would normally
                                        * clear ADDR -- harmless at this point since
                                        * we're already aborting the transaction */
            uart_puts("I2C1 timeout waiting on ");
            uart_puts(label);
            uart_puts(", SR1=0x");
            uart_put_hex_nibbles((uint8_t)(sr1 >> 8));  /* BERR/ARLO/AF/OVR live here */
            uart_put_hex_nibbles((uint8_t)sr1);
            uart_puts(" SR2=0x");
            uart_put_hex_nibbles((uint8_t)(sr2 >> 8));
            uart_put_hex_nibbles((uint8_t)sr2);         /* bit1 = BUSY */
            uart_puts(" CR1=0x");
            uart_put_hex_nibbles((uint8_t)(I2C1_CR1 >> 8));
            uart_put_hex_nibbles((uint8_t)I2C1_CR1);    /* bit9 STOP: still armed, or self-cleared? */
            uart_puts("\r\n");
            return -1;
        }
    }
    return 0;
}

static void i2c1_init(void)
{
    RCC_AHB1ENR |= (1U << 1);   /* GPIOBEN */
    (void)RCC_AHB1ENR;

    RCC_APB1ENR |= (1U << 21);  /* I2C1EN */
    (void)RCC_APB1ENR;

    /* PB8/PB9 -> AF4 (I2C1 SCL/SDA). Pins 8-15 live in AFRH -- AFRL only
     * covers pins 0-7, so the pin-8 nibble is bit 0 of AFRH, not a
     * continuation of AFRL. */
    GPIOB_MODER &= ~(0x3U << (8 * 2));
    GPIOB_MODER |=  (0x2U << (8 * 2));
    GPIOB_MODER &= ~(0x3U << (9 * 2));
    GPIOB_MODER |=  (0x2U << (9 * 2));

    GPIOB_AFRH &= ~(0xFU << ((8 - 8) * 4));
    GPIOB_AFRH |=  (0x4U << ((8 - 8) * 4));
    GPIOB_AFRH &= ~(0xFU << ((9 - 8) * 4));
    GPIOB_AFRH |=  (0x4U << ((9 - 8) * 4));

    /* Open-drain, not push-pull: I2C is wired-AND. A push-pull '1' would
     * actively drive the line high and fight any device (including this
     * one) trying to pull it low. Open-drain only ever pulls low or lets
     * go; the external pull-ups on the breakout board do the rest. */
    GPIOB_OTYPER |= (1U << 8) | (1U << 9);

    /* fPCLK1 = 42 MHz post-clock_init() (SYSCLK/2; was 16 MHz HSI
     * before the clock-tree extension) -> CR2.FREQ = 42.
     * Standard mode 100 kHz: CCR = fPCLK1 / (2 * 100 kHz) = 210.
     * TRISE = fPCLK1[MHz] + 1 = 43 (1000 ns max Sm rise time / 23.8 ns
     * period at 42 MHz). CR2/CCR/TRISE must be written while PE = 0, so
     * PE goes last. */
    I2C1_CR2   = 42U;
    I2C1_CCR   = 210U;
    I2C1_TRISE = 43U;

    I2C1_CR1 = I2C_CR1_PE;
}

/* Single-byte read of an 8-bit register. Returns the byte, or -1 on
 * timeout (i2c1_wait has already printed which flag stalled). */
static int bme280_read_reg8(uint8_t reg)
{
    I2C1_CR1 |= I2C_CR1_START;
    if (i2c1_wait(I2C_SR1_SB, "SB (start)") < 0) return -1;

    I2C1_DR = (uint8_t)(BME280_ADDR << 1);        /* address + write */
    if (i2c1_wait(I2C_SR1_ADDR, "ADDR (write)") < 0) return -1;
    (void)I2C1_SR1;
    (void)I2C1_SR2;                                /* clears ADDR */

    I2C1_DR = reg;
    if (i2c1_wait(I2C_SR1_TXE, "TXE") < 0) return -1;

    I2C1_CR1 |= I2C_CR1_START;                     /* repeated start */
    if (i2c1_wait(I2C_SR1_SB, "SB (repeated start)") < 0) return -1;

    I2C1_DR = (uint8_t)((BME280_ADDR << 1) | 1U);  /* address + read */
    if (i2c1_wait(I2C_SR1_ADDR, "ADDR (read)") < 0) return -1;

    /* Single-byte reception (RM0368 18.3.3, "Closing the communication",
     * item 3): ACK disable happens during EV6 -- i.e. before ADDR is
     * cleared -- and STOP is programmed after ADDR is cleared. Reversing
     * these (STOP before the ADDR-clearing SR1/SR2 read) leaves the
     * peripheral with STOP queued but stuck, BUSY held high, and RXNE
     * never arriving -- exactly the hang this used to produce. */
    I2C1_CR1 &= ~I2C_CR1_ACK;
    (void)I2C1_SR1;
    (void)I2C1_SR2;                                /* clears ADDR */
    I2C1_CR1 |= I2C_CR1_STOP;

    if (i2c1_wait(I2C_SR1_RXNE, "RXNE") < 0) return -1;
    return (int)(uint8_t)I2C1_DR;
}

static int bme280_read_id(void)
{
    return bme280_read_reg8(BME280_ID_REG);
}

/* Writes one 8-bit register: START, addr+W, reg, value, STOP. */
static int bme280_write_reg8(uint8_t reg, uint8_t value)
{
    I2C1_CR1 |= I2C_CR1_START;
    if (i2c1_wait(I2C_SR1_SB, "SB (write)") < 0) return -1;

    I2C1_DR = (uint8_t)(BME280_ADDR << 1);
    if (i2c1_wait(I2C_SR1_ADDR, "ADDR (write)") < 0) return -1;
    (void)I2C1_SR1;
    (void)I2C1_SR2;

    I2C1_DR = reg;
    if (i2c1_wait(I2C_SR1_TXE, "TXE (reg)") < 0) return -1;

    I2C1_DR = value;
    /* Closing a master-transmitter transaction (RM0368 18.3.3, EV8_2):
     * wait for BTF, not just TXE, before setting STOP. TXE only means DR
     * is empty and the byte has started shifting out; BTF means the byte
     * and its ACK are fully done. Every prior write here has segued into
     * a repeated START instead of a STOP, where that distinction doesn't
     * matter (START is deferred by hardware until the byte completes) --
     * a real STOP has no such deferral, so ending on TXE risks cutting
     * this last byte off mid-transmission. */
    if (i2c1_wait(I2C_SR1_BTF, "BTF (value)") < 0) return -1;

    I2C1_CR1 |= I2C_CR1_STOP;
    return 0;
}

/* Polls the status register's "measuring" bit instead of guessing a
 * busy-wait delay for the datasheet's ~9.3 ms max conversion time
 * (osrs_h/t/p = x1): ask the sensor when it's done, rather than assume.
 *
 * Checking only for measuring==0 is ambiguous: that's equally true in
 * the instant right after the ctrl_meas write, before the sensor's state
 * machine has set the bit for the very first time, as it is once a real
 * conversion has finished. Landing in that gap reads back whatever
 * pre-conversion contents are still sitting in the data registers --
 * which isn't obviously wrong, it just isn't a real measurement. Waiting
 * for the 0->1 edge first, then the 1->0 edge, removes the ambiguity. */
static int bme280_wait_measurement(void)
{
    uint32_t tries;

    for (tries = 0; tries < BME280_STATUS_POLL_TRIES; tries++) {
        int status = bme280_read_reg8(BME280_STATUS_REG);
        if (status < 0) return -1;
        if (status & BME280_STATUS_MEASURING) break;
    }
    if (tries == BME280_STATUS_POLL_TRIES) {
        uart_puts("BME280: measuring bit never set\r\n");
        return -1;
    }

    for (tries = 0; tries < BME280_STATUS_POLL_TRIES; tries++) {
        int status = bme280_read_reg8(BME280_STATUS_REG);
        if (status < 0) return -1;
        if (!(status & BME280_STATUS_MEASURING)) return 0;
    }
    uart_puts("BME280: measuring bit never cleared\r\n");
    return -1;
}

/* Multi-byte read of len (>=2) consecutive registers starting at reg. */
static int bme280_burst_read(uint8_t reg, uint8_t *buf, uint32_t len)
{
    I2C1_CR1 |= I2C_CR1_START;
    if (i2c1_wait(I2C_SR1_SB, "SB (burst ptr)") < 0) return -1;

    I2C1_DR = (uint8_t)(BME280_ADDR << 1);
    if (i2c1_wait(I2C_SR1_ADDR, "ADDR (burst ptr)") < 0) return -1;
    (void)I2C1_SR1;
    (void)I2C1_SR2;

    I2C1_DR = reg;
    if (i2c1_wait(I2C_SR1_TXE, "TXE (burst ptr)") < 0) return -1;

    I2C1_CR1 |= I2C_CR1_START;                     /* repeated start into read */
    if (i2c1_wait(I2C_SR1_SB, "SB (burst read)") < 0) return -1;

    /* ACK must be back on for a multi-byte read: bme280_read_reg8 always
     * leaves it cleared (it NACKs its own single byte), and a cleared ACK
     * here would NACK -- and so terminate the transfer after -- byte 0. */
    I2C1_CR1 |= I2C_CR1_ACK;

    I2C1_DR = (uint8_t)((BME280_ADDR << 1) | 1U);
    if (i2c1_wait(I2C_SR1_ADDR, "ADDR (burst read)") < 0) return -1;
    (void)I2C1_SR1;
    (void)I2C1_SR2;

    for (uint32_t i = 0; i < len; i++) {
        if (i2c1_wait(I2C_SR1_RXNE, "RXNE (burst)") < 0) return -1;
        buf[i] = (uint8_t)I2C1_DR;

        /* RM0368 18.3.3, "Closing the communication", items 1-2 (the
         * N>1 case): ACK is cleared and STOP programmed right after
         * reading the *second-to-last* byte, not before -- so the NACK
         * lands on the byte that follows (the last one), not this one. */
        if (i == len - 2) {
            I2C1_CR1 &= ~I2C_CR1_ACK;
            I2C1_CR1 |= I2C_CR1_STOP;
        }
    }

    return 0;
}

/* Calibration coefficients (datasheet 4.2.2). Read once at startup; the
 * compensation functions below read these as globals, same as t_fine. */
static uint16_t dig_T1;
static int16_t  dig_T2, dig_T3;
static uint16_t dig_P1;
static int16_t  dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
static uint8_t  dig_H1, dig_H3;
static int16_t  dig_H2, dig_H4, dig_H5;
static int8_t   dig_H6;

static int bme280_read_calibration(void)
{
    uint8_t c1[BME280_CALIB1_LEN];
    uint8_t c2[BME280_CALIB2_LEN];

    if (bme280_burst_read(BME280_CALIB1_REG, c1, BME280_CALIB1_LEN) < 0) return -1;
    if (bme280_burst_read(BME280_CALIB2_REG, c2, BME280_CALIB2_LEN) < 0) return -1;

    dig_T1 = (uint16_t)(c1[0]  | (c1[1]  << 8));
    dig_T2 = (int16_t)(c1[2]  | (c1[3]  << 8));
    dig_T3 = (int16_t)(c1[4]  | (c1[5]  << 8));
    dig_P1 = (uint16_t)(c1[6]  | (c1[7]  << 8));
    dig_P2 = (int16_t)(c1[8]  | (c1[9]  << 8));
    dig_P3 = (int16_t)(c1[10] | (c1[11] << 8));
    dig_P4 = (int16_t)(c1[12] | (c1[13] << 8));
    dig_P5 = (int16_t)(c1[14] | (c1[15] << 8));
    dig_P6 = (int16_t)(c1[16] | (c1[17] << 8));
    dig_P7 = (int16_t)(c1[18] | (c1[19] << 8));
    dig_P8 = (int16_t)(c1[20] | (c1[21] << 8));
    dig_P9 = (int16_t)(c1[22] | (c1[23] << 8));
    dig_H1 = c1[25];   /* c1[24] is register 0xA0, reserved */

    dig_H2 = (int16_t)(c2[0] | (c2[1] << 8));
    dig_H3 = c2[2];
    /* Table 16: dig_H4[11:4] is register 0xE4 (c2[3]) in full; dig_H4[3:0]
     * and dig_H5[3:0] share register 0xE5 (c2[4]) as its low and high
     * nibble respectively; dig_H5[11:4] is register 0xE6 (c2[5]) in full.
     * Sign lives in the top byte of each 12-bit value, so that byte is
     * cast through int8_t before the left-shift; the nibble it's ORed
     * with only ever occupies the zero bits the shift just vacated. */
    dig_H4 = (int16_t)(((int8_t)c2[3] << 4) | (c2[4] & 0x0FU));
    dig_H5 = (int16_t)(((int8_t)c2[5] << 4) | (c2[4] >> 4));
    dig_H6 = (int8_t)c2[6];

    return 0;
}

/* Below: the datasheet 4.2.3 fixed-point compensation formulas, ported
 * verbatim (same variable names, same operation order, same shift
 * amounts) rather than re-derived -- this is exactly the kind of
 * arithmetic where a "simplified" rewrite would look plausible and be
 * quietly wrong. BME280_S32_t/S64_t -> int32_t/int64_t, BME280_U32_t ->
 * uint32_t, per the datasheet's own type notes. */

static int32_t t_fine;  /* set by compensate_T, consumed by compensate_P/H */

/* Returns temperature in DegC, resolution 0.01 DegC (5123 == 51.23 DegC). */
static int32_t bme280_compensate_T(int32_t adc_T)
{
    int32_t var1, var2, T;
    var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)dig_T1)) * ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) *
            ((int32_t)dig_T3)) >> 14;
    t_fine = var1 + var2;
    T = (t_fine * 5 + 128) >> 8;
    return T;
}

/* Returns pressure in Pa as Q24.8 (24 integer bits, 8 fractional bits). */
static uint32_t bme280_compensate_P(int32_t adc_P)
{
    int64_t var1, var2, p;
    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)dig_P6;
    var2 = var2 + ((var1 * (int64_t)dig_P5) << 17);
    var2 = var2 + (((int64_t)dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)dig_P3) >> 8) + ((var1 * (int64_t)dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)dig_P1) >> 33;
    if (var1 == 0) {
        return 0;  /* avoid divide-by-zero */
    }
    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)dig_P7) << 4);
    return (uint32_t)p;
}

/* Returns humidity in %RH as Q22.10 (22 integer bits, 10 fractional bits). */
static uint32_t bme280_compensate_H(int32_t adc_H)
{
    int32_t v_x1_u32r;

    v_x1_u32r = (t_fine - ((int32_t)76800));
    v_x1_u32r = (((((adc_H << 14) - (((int32_t)dig_H4) << 20) - (((int32_t)dig_H5) * v_x1_u32r)) +
                   ((int32_t)16384)) >> 15) *
                 (((((((v_x1_u32r * ((int32_t)dig_H6)) >> 10) *
                      (((v_x1_u32r * ((int32_t)dig_H3)) >> 11) + ((int32_t)32768))) >> 10) +
                    ((int32_t)2097152)) * ((int32_t)dig_H2) + 8192) >> 14));
    v_x1_u32r = (v_x1_u32r -
                 (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) * ((int32_t)dig_H1)) >> 4));
    v_x1_u32r = (v_x1_u32r < 0 ? 0 : v_x1_u32r);
    v_x1_u32r = (v_x1_u32r > 419430400 ? 419430400 : v_x1_u32r);
    return (uint32_t)(v_x1_u32r >> 12);
}

static void uart_put_udec32(uint32_t v)
{
    char buf[10];
    int i = 0;
    if (v == 0) {
        uart_putc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (i > 0) {
        uart_putc((uint8_t)buf[--i]);
    }
}

/* Prints raw/denom as decimal with 2 fractional digits. Works for any
 * fixed-point format (denom doesn't need to be a power of ten): Q24.8
 * pressure (denom=256), Q22.10 humidity (denom=1024), and plain
 * hundredths-of-a-degree temperature (denom=100) all fall out of the
 * same whole/remainder split. */
static void uart_put_q_as_decimal(uint32_t raw, uint32_t denom)
{
    uart_put_udec32(raw / denom);
    uart_putc('.');
    uint32_t hundredths = ((raw % denom) * 100U) / denom;
    if (hundredths < 10U) {
        uart_putc('0');
    }
    uart_put_udec32(hundredths);
}

/* Normal mode keeps the sensor cycling on its own, so each call just
 * waits for the next measuring 0->1->0 cycle, bursts the fresh data,
 * and prints it -- no re-write of ctrl_meas needed between samples. */
static void bme280_sample_and_print(void)
{
    if (bme280_wait_measurement() < 0) return;

    uint8_t raw[BME280_DATA_LEN];
    if (bme280_burst_read(BME280_DATA_REG, raw, BME280_DATA_LEN) < 0) return;

    uart_puts("raw:");
    for (uint32_t i = 0; i < BME280_DATA_LEN; i++) {
        uart_putc(' ');
        uart_put_hex_byte(raw[i]);
    }
    uart_puts("\r\n");

    /* Datasheet 5.4.7-5.4.9: 20-bit press/temp are msb:lsb:xlsb[7:4],
     * 16-bit hum is msb:lsb. */
    int32_t adc_P = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | (raw[2] >> 4);
    int32_t adc_T = ((int32_t)raw[3] << 12) | ((int32_t)raw[4] << 4) | (raw[5] >> 4);
    int32_t adc_H = ((int32_t)raw[6] << 8)  |  (int32_t)raw[7];

    /* Order is load-bearing: compensate_T sets t_fine as a side effect,
     * and both compensate_P and compensate_H read it. */
    int32_t T = bme280_compensate_T(adc_T);
    uint32_t P = bme280_compensate_P(adc_P);
    uint32_t H = bme280_compensate_H(adc_H);

    uart_puts("temp: ");
    if (T < 0) {
        uart_putc('-');
        T = -T;
    }
    uart_put_q_as_decimal((uint32_t)T, 100U);
    uart_puts(" C\r\n");

    uart_puts("pressure: ");
    uart_put_q_as_decimal(P, 256U);
    uart_puts(" Pa\r\n");

    uart_puts("humidity: ");
    uart_put_q_as_decimal(H, 1024U);
    uart_puts(" %RH\r\n");
}

int main(void)
{
    clock_init();
    systick_init();

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
     * fCK = 42 MHz post-clock_init() (PCLK1 = SYSCLK/2; was 16 MHz HSI
     * before the clock-tree extension).
     * USARTDIV = 42e6 / (16 * 115200) = 22.7865 (exact division, not a
     * clean fraction). Nearest 1/16 step: mantissa = 22, fraction =
     * round(0.7865 * 16) = 13 (0xD) -- giving a programmed USARTDIV of
     * 22 + 13/16 = 22.8125. That post-rounding value is what RM0368's
     * own Table 82 entry for 115.2 KBps at fPCLK=42 MHz lists (115.068
     * KBps actual, 0.11% error) -- it's the register's contents, not
     * the raw division result.
     */
    USART2_BRR = (22 << 4) | 0xD;

    USART2_CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;

    NVIC_ISER1 = USART2_IRQn_BIT;

    i2c1_init();

    int id = bme280_read_id();
    uart_puts("chip id: ");
    if (id < 0) {
        uart_puts("read failed\r\n");
    } else {
        uart_put_hex_byte((uint8_t)id);
        if (id == 0x60) {
            uart_puts(" (BME280)\r\n");
        } else if (id == 0x58) {
            uart_puts(" (BMP280)\r\n");
        } else {
            uart_puts(" (unrecognized)\r\n");
        }
    }

    /* Datasheet 5.4.3: ctrl_hum only takes effect once ctrl_meas is
     * subsequently written, so ctrl_hum must be written first. */
    int ok = (id == 0x60);
    ok = ok && (bme280_write_reg8(BME280_CTRL_HUM_REG, BME280_CTRL_HUM_VAL) == 0);
    ok = ok && (bme280_write_reg8(BME280_CTRL_MEAS_REG, BME280_CTRL_MEAS_VAL) == 0);

    if (ok) {
        int hum_rb = bme280_read_reg8(BME280_CTRL_HUM_REG);
        int meas_rb = bme280_read_reg8(BME280_CTRL_MEAS_REG);
        uart_puts("ctrl_hum=");
        uart_put_hex_byte((uint8_t)hum_rb);
        uart_puts(" ctrl_meas=");
        uart_put_hex_byte((uint8_t)meas_rb);
        uart_puts("\r\n");
    }

    ok = ok && (bme280_read_calibration() == 0);

    if (ok) {
        bme280_sample_and_print();
    }

    uint8_t led_state = 0;
    uint32_t last_sample = millis();

    while (1) {
        if (tail != head) {
            uint8_t c = rb[tail];
            tail = (tail + 1) & (RB_SIZE - 1);

            uart_putc(c);

            if (led_state) {
                GPIOA_BSRR = (1 << (LD2_PIN + 16));
            } else {
                GPIOA_BSRR = (1 << LD2_PIN);
            }
            led_state = !led_state;
        }

        /* Checked in the same loop as the RX echo above (rather than a
         * blocking delay_ms() call) so a long gap between samples
         * doesn't stall echoing bytes that arrived during it -- the ring
         * buffer would hold them either way, but this keeps the terminal
         * responsive instead of bursty. Same unsigned-subtraction
         * wraparound safety as delay_ms(): correct across the ~49-day
         * ticks rollover, where `millis() >= last_sample + PERIOD` would
         * not be. */
        if (ok && (uint32_t)(millis() - last_sample) >= BME280_SAMPLE_PERIOD_MS) {
            bme280_sample_and_print();
            last_sample = millis();
        }
    }
}
