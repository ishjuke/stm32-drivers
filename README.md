# stm32-drivers

A bare-metal driver stack for the STM32F401RE (ARM Cortex-M4), written in C with no
HAL, no CubeMX-generated code, and no vendor libraries. Every register write is derived
from RM0368 and the F401xE datasheet.

The point wasn't to make an LED blink. It was to build the layer underneath the layer
most embedded projects start from — the linker script, the vector table, the C runtime —
and then to prove each piece works rather than assume it does because the output looked
right.

---

## What's here

| Milestone | What it does | How it was verified |
|---|---|---|
| 1 | Linker script, startup code, own C runtime | GDB register/memory reads + poison-and-restore negative control |
| 2 | Register-level GPIO blink (PA5) | Register decomposition in GDB; timing measured on a logic analyzer |
| 3 | UART TX on USART2 (PA2, AF7) | Disassembly of the flashed ELF + protocol decode off the physical wire |
| 4 | Interrupt-driven UART RX with a ring buffer | Three scripted tests with predicted vs. measured byte counts |

---

## Hardware and toolchain

- **Board:** NUCLEO-F401RE (MB1136 rev C), STM32F401RET6, Cortex-M4 @ 16 MHz HSI
- **Host:** Raspberry Pi 5 over SSH — build, flash, and debug all run on the Pi with the
  Nucleo attached by USB
- **Toolchain:** `arm-none-eabi-gcc`, `arm-none-eabi-objcopy`, OpenOCD, `gdb-multiarch`,
  `make`
- **Instruments:** 8-channel USB logic analyzer (fx2lafw), driven with `sigrok-cli`

```
make            # compile, link, objcopy to .bin, report size
make flash      # program and verify over SWD via OpenOCD
make clean
```

Build flags: `-mcpu=cortex-m4 -mthumb -mfloat-abi=soft -O0 -g -Wall -ffreestanding
-nostdlib -ffunction-sections -fdata-sections`, linked with `--gc-sections` against a
hand-written script.

---

## Milestone 1 — the runtime that doesn't exist until you write it

Before `main()` can run, something has to load the stack pointer, copy initialized
globals from flash into RAM, and zero the uninitialized ones. On a hosted system that's
already done for you. Here it isn't.

**`linker.ld`** declares the memory map and places sections:

```
FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 96K
```

`.data` is placed in SRAM but loaded from flash via `AT>`, with `_sidata` capturing the
load address so the startup code knows where to copy from. Every section boundary is
`ALIGN(4)`, because the copy loops walk `uint32_t *` and unaligned boundaries would mean
a byte count that isn't a multiple of four.

`KEEP()` on `.isr_vector` is load-bearing. Nothing in the program references the vector
table array — the hardware reads it directly — so `--gc-sections` would otherwise discard
it entirely and leave a board that resets into garbage.

**`startup.c`** defines the vector table as an array of function pointers placed in
`.isr_vector`. Word 0 is the initial stack pointer; word 1 is `Reset_Handler`. Exception
handlers are declared `__attribute__((weak, alias("Default_Handler")))` so anything
unhandled traps in a known infinite loop instead of running off into undefined memory.

### The negative control

Confirming `data_test == 0xDEADBEEF` after reset only proves the value is correct, not
that the copy loop caused it. So the loops were tested by poisoning SRAM and disabling
them:

| Loop state | `data_test` | `bss_test` |
|---|---|---|
| Copy loop disabled, poisoned `0x12345678` | stays `0x12345678` | — |
| Zero loop disabled, poisoned `0xcafebabe` | — | stays `0xcafebabe` |
| Both restored, poisoned `0x11111111` / `0x22222222` | becomes `0xdeadbeef` | becomes `0x00000000` |

This works because SRAM retains its contents across a warm reset (`monitor reset halt`
doesn't cut power), so the poison survives into the next run and the startup code has to
actually overwrite it. On a cold power-on the contents would be indeterminate and the
test would prove nothing.

Result: the loops provably overwrite arbitrary garbage with the correct values, and
provably fail to when disabled.

---

## Milestone 2 — GPIO at the register level

Enable the GPIOA clock in `RCC_AHB1ENR`, set PA5 to general-purpose output in `MODER`,
drive it through `BSRR`.

Two details worth recording:

**The RCC read-back.** After setting a peripheral clock-enable bit, the write takes a
couple of cycles to land, and an immediate access to that peripheral can be silently
dropped. A dummy read of `RCC_AHB1ENR` forces completion before the first `MODER` write.
At `-O0` you'd likely get away without it; at `-O2` you wouldn't.

**`GPIOA_MODER` reads `0xa8000400` after configuration.** The reset value is
`0xA8000000` — bits 31:30, 29:28, and 27:26 hold `10` for PA15, PA14, and PA13, which
default to alternate-function mode because they carry JTDI, SWCLK, and SWDIO. Those are
the debug pins; reconfiguring PA13 or PA14 as GPIO would sever SWD mid-program and
require BOOT0 or connect-under-reset to recover.

The write added exactly `0x400` — bit 10 set, bit 11 clear — which is `01` on PA5 alone,
disturbing nothing else.

**Timing check.** The busy-wait delay was modelled at roughly 7 cycles per iteration:
`delay(500000)` at 16 MHz predicts 218.75 ms per edge. Measured on the logic analyzer:
217.6 ms average across 26 consecutive edges, ±0.05 ms. Within 0.5% of prediction.

---

## Milestone 3 — UART TX

Three things have to be right at once, and they fail in distinguishable ways.

**Clock.** USART2 sits on APB1. With no PLL configured the chip runs on the 16 MHz HSI,
and the APB1 prescaler is `/1` at reset, so 16 MHz reaches the peripheral.

**Baud divisor.** `USARTDIV = 16e6 / (16 × 115200) = 8.6806`. Mantissa 8, fraction
`round(0.6806 × 16) = 11`, giving `BRR = 0x8B`. Actual resulting baud is ~115,108 —
0.08% error, far inside tolerance.

**Pin routing.** PA2 to alternate-function mode in `MODER` (`10`, not `01`), then AF7
selected in `AFRL`. The AF number comes from the datasheet's alternate-function table,
not the reference manual.

Transmission polls `TXE` in `SR` before each write to `DR`.

### Verification

**Machine code.** Terminal output proves the source *looked* right; it doesn't prove what
the compiler emitted. Disassembling the flashed ELF:

```
8000134:  ldr  r3, [pc, #40]   @ r3 = 0x40004404  (USART2_DR)
8000136:  movs r2, #85          @ r2 = 0x55  ('U')
8000138:  str  r2, [r3, #0]     @ DR = 0x55
```

`BRR` is written as `movs r2, #139` (`0x8B`), `CR1` as `movw r2, #8200` (`0x2008` =
`TE | UE`), and the TXE poll compiles to a real conditional branch — not elided, which is
what `volatile` on the register accessor is preventing.

**Physical layer.** `0x55` is an alternating bit pattern, and a mismatched link aliases
into `0x55`, `0x00`, and `0xFF` unusually often — so clean `U`s in a terminal are weaker
evidence than they look. Captured at 4 MHz on the logic analyzer and decoded:

```
uart-1: Start bit
uart-1: 1 0 1 0 1 0 1 0
uart-1: 55
uart-1: Stop bit
```

(LSB-first, which is why the bit sequence reads backwards from the byte.)

Cross-check: `0x55` plus a start bit gives five low bit-periods per frame. At 4 MHz one
bit is ~34.7 samples, so ~174 low samples per frame; 14 frames predicts ~2,430. Measured
2,414 — 0.7% off. And 14 frames in 3 s is ~4.7 Hz, matching the 217 ms loop period
measured independently on PA5.

### The solder bridge

Probing PA2 produced nothing. Not noise, not framing errors — a flat line, on the pin
UM1724's Table 29 correctly identifies as CN10 pin 35.

The cause is in Table 10: with **SB62 and SB63 OFF** (the shipped default), PA2 and PA3
are disconnected from D1/D0 on the ARDUINO connector CN9 **and from the ST morpho
connector CN10**. The morpho header is not an unconditional breakout of every MCU pin.
Those two bridges route PA2/PA3 to the on-board ST-LINK instead, which is what makes the
virtual COM port work over the same USB cable.

The signal is reachable at **CN3's RX pin** — labelled from the ST-LINK's perspective, so
the target's transmit arrives on the pin marked receive.

Roughly three hours went into this before the board manual got read properly. Every
measurement taken during that time was correct; the assumption underneath them wasn't.

---

## Milestone 4 — interrupt-driven RX with a ring buffer

The first milestone with two execution contexts sharing memory.

**Configuration.** PA3 to AF7 for USART2_RX. `RE` (bit 2) and `RXNEIE` (bit 5) in `CR1`.
USART2 is IRQ 38 — confirmed against ST's CMSIS `stm32f401xe.h` rather than counted by
hand, since positions 19–22 are reserved on this part (no CAN) and a miscount anywhere in
the table shifts every entry after it. NVIC enable therefore goes to `ISER[1]`
(`0xE000E104`) bit `38 - 32 = 6`, not `ISER[0]`.

**Buffer.**

```c
#define RB_SIZE 64
static volatile uint8_t  rb[RB_SIZE];
static volatile uint32_t head;  /* written only by the ISR */
static volatile uint32_t tail;  /* written only by main    */
```

Design decisions, each deliberate:

- **Power-of-two size** so `(head + 1) & (RB_SIZE - 1)` replaces a modulo.
- **Sacrifice one slot** for full-vs-empty rather than keeping a separate count — one
  fewer piece of state that can desynchronize.
- **Drop the incoming byte when full**, rather than overwriting the oldest. Preserves
  already-buffered, not-yet-consumed data in order.

**No critical section, and the reason matters.** Single producer, single consumer, one
writer per index — neither side ever writes the other's variable, so there's no
read-modify-write race. Naturally-aligned 32-bit words on a Cortex-M4 are single
indivisible bus transactions, so a reader sees a fully-old or fully-new value, never a
torn one. `volatile` is still required, but purely to stop the compiler caching an index
in a register across loop iterations — it is doing a compiler job here, not a
hardware-atomicity job. A second writer on either index, or a compound update spanning
both, would change this.

**ORE.** Overrun requires the previous byte to be unread, so `ORE` only ever sets
alongside `RXNE` on this family. Reading `SR` then `DR` is RM0368 §19.6.1's documented
clear sequence, and the normal receive path already performs exactly that — so no
separate handling is needed. Noted as a conscious tradeoff: if that reasoning were wrong,
the failure mode is an interrupt storm that starves `main` and looks like a dead board.

### Tests

| Test | Predicted | Measured | Result |
|---|---|---|---|
| Line rate — 100 bytes, fast drain | 100 | 100 | No drops |
| Forced overflow — ~65 ms/byte stall | ~64 | 64 | Buffer caps as designed |
| Negative control — which bytes survive | front of stream | `0123456789…0123`, cut mid-7th repetition | Drop-newest confirmed |

The third test is the one that matters. Counting bytes shows the buffer has a limit;
looking at *which* bytes survived distinguishes drop-newest from overwrite-oldest, which
would have produced the tail of the stream instead. Six full repetitions (60 bytes) plus
`0123` is exactly 64 — the content and the count agree, derived independently.

Final build: **744 text / 0 data / 72 bss**. The 72 bytes are `rb[64]` plus two 32-bit
indices, exactly — nothing unaccounted for.

---

## Verification approach

The through-line across all four milestones: a correct-looking output is not evidence
that the mechanism producing it is correct.

- Confirming a value is right doesn't prove your code set it — **disable the code and
  confirm the value goes wrong.**
- Confirming the source says X doesn't prove the silicon does X — **read the flashed
  binary.**
- Confirming a terminal shows the expected character doesn't prove the wire carries it —
  **decode the signal.**
- Confirming a buffer has a limit doesn't prove your overflow policy — **check which data
  survived.**

Each of those caught something, or would have if it had been wrong.

## Things that turned out to be false

- The ST morpho header is a direct breakout of every MCU pin. *(It isn't — SB62/SB63.)*
- A measurement that returns nothing means the instrument or the wiring is broken. *(The
  target was fine the whole time — confirmed repeatedly over serial while the analyzer
  read nothing. The signal genuinely wasn't present at the pin being probed.)*
- `bss_test == 0` proves the zero loop ran. *(SRAM could already have been zero.)*
- CN3's TX pin carries the target's transmit. *(Labels are from the ST-LINK's
  perspective.)*