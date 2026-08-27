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
| 5 | I2C driver for a BME280 sensor, with compensation | Chip ID readback, physical stimulus response, cross-check against a local weather station |

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

## Milestone 5 — I2C and a real sensor

The first peripheral with a slave on the other end. UART transmits into the
void and doesn't care whether anything is listening; I2C is a handshake, and
every step can fail in a way the peripheral reports only as "still waiting."

**Configuration.** PB8 (SCL) and PB9 (SDA) as AF4 for I2C1, and — new versus
every previous milestone — **open-drain** output via `OTYPER`. I2C is a
wired-AND bus where devices pull low and pull-ups restore high; a push-pull
driver would fight the pull-ups and could damage something. `CR2` FREQ is set
to the APB1 clock in MHz (16), then `CCR = 16e6 / (2 × 100e3) = 80` and
`TRISE = (1000ns / 62.5ns) + 1 = 17` for 100 kHz standard mode. `PE` is
enabled last, because `CCR` and `TRISE` are write-protected while the
peripheral is running.

**Reads.** Register access is a compound transaction: address the device for
write, send the register pointer, issue a repeated START, re-address for read,
then receive. Single-byte and multi-byte reads have genuinely different
teardowns — for one byte, ACK is disabled and STOP issued around a single
transfer; for N bytes, ACK stays on until the second-to-last byte arrives.
RM0368 §18.3.3 treats them as separate cases and they are.

**No bare spin loops.** Every flag wait is wrapped in a helper that takes a
timeout and a label:

```c
if (!i2c1_wait(I2C_SR1_ADDR, "ADDR")) { /* reports which flag never came */ }
```

An infinite `while (!(SR1 & FLAG))` tells you nothing when it hangs. A timeout
that names the stalled flag localizes the failure before you've attached a
debugger — which is what made both of the bugs below tractable.

### Bug 1 — STOP before ADDR clear

**Symptom:** the chip-ID read hung at the final receive. No error flags: SR1
was `0x0000`, so no NACK, no bus error, no arbitration loss. SR2 read `0x0003`
(MSL and BUSY set, TRA clear — still master, still mid-transaction, correctly
in receive direction). CR1 read `0x0201`: PE set, ACK correctly cleared, and
**STOP still pending**, never self-clearing.

**Reasoning:** the address phase had ACKed twice — once for write, once after
the repeated START. An ACK is the slave physically pulling SDA low on the ninth
clock, which a disconnected wire, dead sensor, or wrong address cannot fake.
That ruled out the entire hardware hypothesis without touching the bench. A
stuck STOP with no error latched pointed instead at a sequencing violation.

**Cause:** STOP was being programmed before the ADDR flag was cleared.
RM0368 §18.3.3 puts ACK-disable at EV6 *before* the ADDR clear, and STOP
*after*. Requesting a stop while the peripheral was still waiting on the
address condition wedged the state machine with no error path to report it.

**Fix:** swap the two steps. Correct on the first reflash — chip ID `0x60`,
confirming a genuine BME280 rather than the BMP280 that ships in a
depressing share of listings.

Diagnosed entirely from CR1/SR1/SR2 state. No logic analyzer capture was
needed, which is worth noting after the milestone-3 debugging session went
the opposite way.

### Bug 2 — a predicate true on both sides of the event

**Symptom:** the first burst read returned the reset-default register pattern,
which happens to coincide with the datasheet's "measurement skipped" sentinel.
The second read looked correct. Same code, different result.

**Cause:** `bme280_wait_measurement()` polled for `measuring == 0`. That
predicate is satisfied *both* before a conversion starts and after it
finishes — the poll won the race and returned instantly, so the burst grabbed
registers that had never been written.

The second read only worked by accident: two unrelated register-readback
`printf`s happened to delay the poll long enough for the conversion to begin.
Remove those debug prints and the bug returns. That is the worst shape a bug
can have — correctness contingent on unrelated timing.

**Fix:** wait for the 0→1 edge (conversion genuinely started), *then* the 1→0
edge (genuinely finished). Deterministic regardless of how fast the caller
arrives. Level-checking a flag that is asserted only transiently is never
sufficient; the edge is the event.

Verified across three consecutive resets, all returning distinct values with
`up` and `ut` climbing monotonically from board self-heating and `uh` moving
in a smaller, uncorrelated range. Coupling to a physical process is evidence
a stuck register cannot produce.

### Compensation

Calibration coefficients live in two non-contiguous blocks (0x88–0xA1 and
0xE1–0xE7) and are a mix of signed and unsigned 16-bit values, with `dig_H4`
and `dig_H5` bit-packed into a shared byte at 0xE5. Getting a signedness wrong
produces output that still looks plausible — the worst failure mode, since
nothing crashes and nothing is obviously out of range.

The compensation routines are ported **verbatim** from datasheet §4.2.3 —
same variable names, same shift amounts, same operation order — deliberately.
This is exactly the class of fixed-point arithmetic where a tidier rewrite
would look correct and be silently wrong.

`t_fine` is computed as a side effect of temperature compensation and consumed
by both pressure and humidity, so temperature must run first. The ordering is
load-bearing, not stylistic.

### Toolchain: the 64-bit divide

The pressure path needs a 64-bit division, which Cortex-M4 has no instruction
for. GCC emits a call to `__aeabi_ldivmod`, and the link failed:

```
undefined reference to `__aeabi_ldivmod'
```

`-nostdlib` excludes libgcc along with libc. But libgcc is not the C standard
library — it's the compiler's own runtime for operations the ISA can't perform
natively, the same category as the compiler deciding which instructions to
emit. Linking it doesn't compromise the no-HAL, no-vendor-library constraint.

Adding `-lgcc` to `LDFLAGS` did not fix it. The linker processes archives left
to right and pulls only members that resolve symbols it has *already* seen
unresolved; with `-lgcc` ahead of the object files, nothing needed
`__aeabi_ldivmod` yet, so the archive was scanned and discarded before the
reference existed. It has to come **after** the objects on the link line.

### Verification

| Check | Result |
|---|---|
| Chip ID (0xD0) | `0x60` — genuine BME280 |
| Steady-state output | 30.14 °C / 97523 Pa / 38.48 %RH |
| Reset-to-reset stability | Values drift plausibly; nothing pinned |
| Temperature vs. phone | 30.1 °C measured vs. 29 °C reported — the ~1 °C offset is the expected direction for a sensor beside a powered MCU |
| Humidity step response | 49% baseline → 78.83% peak on breath → decay back to ~49% over ~13 s |
| Pressure vs. weather station | See below |

The humidity step response is the strongest single piece of evidence here. A
spike followed by exponential decay back to the original baseline is a shape
no stuck register, sentinel value, or scale error can produce. It exercises
the full path — bus, burst read, compensation math — against a known physical
stimulus.

**Pressure cross-check.** Region of Waterloo International Airport reported
101.3 kPa (sea-level corrected). Kitchener sits at roughly 336 m, and the
station-pressure correction is about 12 Pa/m, so the expected reading at this
elevation is roughly 101.3 − 4.0 = **97.3 kPa**. Measured: **97.52 kPa**. A
~0.2 kPa gap against a linear approximation of a relationship that is actually
exponential and temperature-dependent, with the elevation known only
approximately.

An earlier version of this check compared against Kingston's ~90 m elevation
and appeared to show a 3 kPa error. The measurement was correct; the reference
point was wrong. Worth recording, because "the reading disagrees with
expectation" and "the reading is wrong" are not the same claim, and the
cheaper thing to check is usually the expectation.

---

## Verification approach

The through-line across all five milestones: a correct-looking output is not evidence
that the mechanism producing it is correct.

- Confirming a value is right doesn't prove your code set it — **disable the code and
  confirm the value goes wrong.**
- Confirming the source says X doesn't prove the silicon does X — **read the flashed
  binary.**
- Confirming a terminal shows the expected character doesn't prove the wire carries it —
  **decode the signal.**
- Confirming a buffer has a limit doesn't prove your overflow policy — **check which data
  survived.**
- Confirming a value is plausible doesn't prove the pipeline works — **drive the input
  and confirm the output follows.**

Each of those caught something, or would have if it had been wrong.

## Things that turned out to be false

- The ST morpho header is a direct breakout of every MCU pin. *(It isn't — SB62/SB63.)*
- A measurement that returns nothing means the instrument or the wiring is broken. *(The
  target was fine the whole time — confirmed repeatedly over serial while the analyzer
  read nothing. The signal genuinely wasn't present at the pin being probed.)*
- `bss_test == 0` proves the zero loop ran. *(SRAM could already have been zero.)*
- CN3's TX pin carries the target's transmit. *(Labels are from the ST-LINK's
  perspective.)*
- A stalled bus means bad wiring. *(The slave had ACKed twice — it was a
  sequencing bug in my own code.)*
- Waiting for `measuring == 0` means waiting for the measurement.
  *(It's also true before the measurement starts.)*
- The sensor disagreed with expected pressure, so the compensation math is
  wrong. *(Wrong city's elevation.)*