# stm32-drivers

A bare-metal driver stack for the STM32F401RE (ARM Cortex-M4), written in C with no
HAL, no CubeMX-generated code, and no vendor libraries. Every register write is derived
from RM0368, the F401xE datasheet, and — for the core peripherals — PM0214.

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
| 6 | SysTick millisecond timebase | Tick period measured on a logic analyzer; rollover arithmetic reasoned through |
| 7 | Clock tree — HSI through the PLL to 84 MHz | Every clock-derived constant re-measured on hardware, not recompiled and assumed |
| 8 | DMA-driven UART TX (DMA1 stream 6, channel 4) | Bit-for-bit waveform comparison against the polled path, decoded off the wire |

---

## Hardware and toolchain

- **Board:** NUCLEO-F401RE (MB1136 rev C), STM32F401RET6, Cortex-M4 — 16 MHz HSI at
  reset, clocked to its 84 MHz maximum through the PLL in milestone 7
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
if (i2c1_wait(I2C_SR1_ADDR, "ADDR (read)") < 0) return -1;  /* helper has already named the stalled flag */
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

## Milestone 6 — a real timebase

Milestone 5 timed the BME280 sample period by counting main-loop iterations — an
uncalibrated proxy (~1.62 µs per pass, heavier than milestone 2's bare `delay()` loop
because this one also reads the volatile ring-buffer indices every time), tuned until
the gap between samples sat near the 1.5 s that keeps the humidity breath-test
resolving as a curve rather than two blurred points. SysTick replaces it with an
actual clock.

**Configuration.** SysTick is part of the Cortex-M4 core (System Control Space,
PM0214 §4.5), not a peripheral — there is no `RCC` clock-enable bit, it is always
powered. `SYST_CSR` / `SYST_RVR` / `SYST_CVR` at `0xE000E010`. `CLKSOURCE` is set to
the core clock (not core / 8), `TICKINT` for the interrupt, `ENABLE` to start.

**The reload value.** The counter reloads the cycle *after* it reaches zero, so a
period of N cycles needs N − 1 loaded. At 16 MHz, 1 ms is 16000 cycles → `RVR = 15999`.
Getting the − 1 wrong doesn't fail loudly: load 16000 and every "1 ms" tick is one
cycle too long, the clock runs slow by ~0.006%, invisible in testing and wrong
forever. Writing any value to `SYST_CVR` clears it and `COUNTFLAG` — start from a
known state rather than whatever reset left behind.

**The counter.** `SysTick_Handler` increments a `volatile uint32_t ticks`; `millis()`
reads it. Same `volatile`-for-the-compiler-not-the-hardware reasoning as milestone 4's
ring-buffer indices — it stops `ticks` being cached in a register across a poll loop,
nothing more.

**Rollover.** `ticks` wraps every ~49.7 days (2³² ms). `delay_ms()` spins on
`(uint32_t)(millis() - start) < ms`: unsigned subtraction wraps the same way `ticks`
does, so `(now - start)` stays correct across the rollover for any interval up to
~2³¹ ms. The intuitive `now >= start + ms` is **not** safe — `start + ms` can itself
wrap and break the comparison at the boundary. The main loop's sample scheduling uses
the same `(now - last)` form, checked non-blocking alongside the RX echo so a slow
sample never stalls byte echo.

**Verification.** Temporarily toggle PA5 in `SysTick_Handler` and capture on the
analyzer (reverted afterward — it is a diagnostic, not a feature): **995.45 µs**
average tick against 1000 µs nominal, ~0.455% fast, inside the HSI's ±1% factory-trim
spec. The retired busy-wait couldn't be checked this way at all — there was no edge
to measure, only a loop count someone had tuned by eye.

---

## Milestone 7 — the clock tree

Everything so far ran on the 16 MHz HSI because that is the reset default and nothing
needed more. Reaching the part's 84 MHz maximum means the PLL — and by now three
separate constants derive from the clock and every one has to be rederived.

**Order.** RM0368 §3.4.1's "increasing the CPU frequency" procedure: raise the flash
wait states *first*, then switch SYSCLK. The other way round, the core is fetching
from flash that can't keep up before the latency catches it.

**Flash.** 2 wait states (`FLASH_ACR.LATENCY`), RM0368 Table 6, for 60 < HCLK ≤ 84 MHz
at 2.7–3.6 V (the board measures ~3.25 V). Wrong here is an immediate hard fault —
flash physically can't supply instructions fast enough and the core runs off into
garbage — not a graceful slowdown.

**PLL** (`RCC_PLLCFGR`). `PLLM = 8` → 16 / 8 = 2 MHz at the VCO input (§6.3.2: "select
2 MHz to limit PLL jitter"). `PLLN = 168` → 2 × 168 = 336 MHz VCO output, which must
land in 192–432 MHz. `PLLP = /4` (encoded `01`) → 336 / 4 = **84 MHz** SYSCLK.
`PLLQ = 7` → 48 MHz, unused (no USB / SDIO / RNG) but set to a real valid value rather
than a reserved encoding. `PLLSRC` left 0 = HSI; this board has never had an HSE.

**Voltage scaling.** `PWR_CR.VOS` = Scale 2 (§3.4.1: good to 84 MHz). It needs
`RCC_APB1ENR.PWREN` set first, like any APB1 register, and it is also the reset
default — but §5.1.3 says the regulator is forced to Scale 3 whenever the PLL is off,
so the write only takes effect once `PLLON` is set, and no `VOSRDY` poll is needed
because the value never actually transitions.

**Bus prescalers** (`RCC_CFGR`). APB1 caps at 42 MHz (§6.2) → `PPRE1 = /2`. APB2 and
AHB both reach 84 MHz → `PPRE2` and `HPRE` stay at `/1`. `PPRE1` and `SW` are written
together, so APB1 is already dividing by 2 in the same cycle SYSCLK becomes 84 MHz —
no window where APB1 briefly sees undivided 84 MHz.

Every value was checked against the RM0368 PDF text, then cross-checked against the
disassembled binary.

**Rederived constants.**

| Constant | 16 MHz | 84 MHz | Derivation |
|---|---|---|---|
| `USART2_BRR` | `0x8B` | `0x16D` | USARTDIV = 42e6 / (16 × 115200) = 22.7865 → nearest 1/16 is 22 + 13/16 → `(22 << 4) \| 0xD` |
| `I2C1` `CR2` / `CCR` / `TRISE` | 16 / 80 / 17 | 42 / 210 / 43 | `FREQ` = PCLK1 MHz; `CCR` = PCLK1 / (2 × 100 kHz); `TRISE` = PCLK1 MHz + 1 |
| SysTick `RVR` | 15999 | 83999 | 84e6 / 1000 − 1 |

**Verification** — recompiled-and-assumed is not the same as checked:

| Signal | Target | Measured | Note |
|---|---|---|---|
| BME280 sample cadence | 1.500 s | 1.494–1.495 s | a stale 16 MHz reload would show ~286 ms |
| I2C1 SCL period | 10 µs | ~9.89 µs | triggered on SDA falling edge — I2C is ~1 ms of activity every 1.5 s, an untriggered capture mostly catches idle |
| UART TX bit period | 8.69 µs | ~8.55 µs | against the real 22 + 13/16 `BRR` rounding, not the naive division |
| SysTick period @ 84 MHz | 1000 µs | 984.8 µs | temporary PA5 toggle again, then reverted |

### The drift, and what it isn't

All three clock-derived signals — SysTick, I2C SCL, UART TX — measure **1.1–1.6% fast**
after the switch, up from the ~0.455% on SysTick alone at 16 MHz.

What's solid: PCLK1 and HCLK are exact integer divisions of the same SYSCLK (confirmed
in the disassembly), so they cannot carry genuinely different trim error at one
instant. A consistent shift across all three is upstream of any single peripheral —
one cause, not three bugs.

What isn't: the shift is **consistent with** the HSI (an on-die RC oscillator, not a
crystal) drifting under the higher self-heating at 84 MHz — but that was never tested
against the alternatives. The distinguishing experiment is a capture immediately after
a cold power-on versus one after ~10 minutes running: growing drift points to thermal,
a constant offset from the first second points elsewhere — PLL jitter, or the logic
analyzer's own uncharacterized timebase (a cheap crystal, never itself measured
against a reference). That experiment hasn't been run. And 1.1–1.6% sits *outside* the
HSI's own ±1% spec, so an out-of-spec part is a third live possibility. Unresolved,
and flagged in-code near `clock_init()` rather than buried in history.

Fixed while here: two comments where the shipped value was right but the derivation
shown wasn't — the milestone-6 SysTick note had the off-by-one *direction* backwards
(a too-large reload makes the clock slow, not fast, since period = `RVR` + 1), and the
`USART2_BRR` comment stated 22.8125 as the raw division when that is the post-rounding
register value (true division 22.7865, which rounds to the same step).

---

## Milestone 8 — DMA for UART TX

Milestone 3's transmit polls `TXE` before every byte — the CPU hand-carries each one.
DMA moves the bytes from memory into `USART2_DR` on the peripheral's own request line;
the CPU sets the transfer up once and takes one interrupt when all N are done. The claim
to prove is that the wire sees exactly what it saw before.

**The mapping is not a choice.** RM0368 Table 28 (DMA1 request mapping) fixes it in
silicon: USART2_TX is **DMA1, Stream 6, Channel 4** — the only cell in the table for
it. (USART2_RX would be Stream 5, same channel.) Pick the wrong stream and the
peripheral's request line never reaches it; the transfer silently never starts. That
is the single most common DMA bug and it looks like nothing at all.

**`CHSEL` is the load-bearing field.** `CHSEL[2:0]` in `DMA1_S6CR` selects which of the
8 request lines multiplexed onto Stream 6 is live. Channel 4 = `100`. Left at the reset
`000`, Stream 6 listens to **SPI3_TX** instead — `DMAT` set or not, USART2's request is
not routed in and nothing transmits. Worth knowing what the wrong value selects, not
just that it is wrong.

**Placement.** DMA1 is on **AHB1** (`RCC_AHB1ENR` bit 21), even though it only ever
serves APB1 / APB2 peripherals. IRQ: **DMA1_Stream6 = IRQ 17** (RM0368 Table 38,
position 17; confirmed against CMSIS `stm32f401xe.h`). 17 < 32, so NVIC enable goes to
`ISER[0]` bit 17 — unlike USART2's IRQ 38, which needed `ISER[1]` (milestone 4).

**Configuration sequence** — order matters more than anywhere prior:

1. `RCC_AHB1ENR.DMA1EN`.
2. Disable the stream (`EN` = 0 in `SxCR`) and **poll it back to 0**. Most of `SxCR` is
   write-protected while `EN` = 1, and `EN` does not drop synchronously — an in-flight
   AHB beat retires first. Writing config through that window silently does nothing.
3. Clear Stream 6's flags in `DMA_HIFCR` (`CTCIF6` / `CHTIF6` / `CTEIF6` / `CDMEIF6` /
   `CFEIF6`, bits 21 / 20 / 19 / 18 / 16). RM0368 §9.5.5 requires it before re-enabling;
   a stale `TCIF6` from the previous transfer trips the completion handler the instant
   the next one starts.
4. `SxPAR` = `&USART2_DR` (`0x4000 4404`, fixed — `PINC` = 0), `SxM0AR` = source,
   `SxNDTR` = byte count.
5. `SxCR`: `CHSEL` = 4, `DIR` = `01` (memory-to-peripheral), `MINC` = 1, `PSIZE` /
   `MSIZE` = `00` (byte), `TCIE` = 1, `EN` = 1 — `EN` in the same write, since every
   other field is now in place.
6. `USART2_CR3.DMAT` — the USART has to be told to emit DMA requests on `TXE`. Separate
   from anything in the DMA controller, and the second-most-common omission. Set once,
   in init.

**What the completion flag means.** `dma_tx_busy` is set by `uart_write_dma()` and
cleared in the TC handler. It means *"the DMA engine still owns the caller's buffer"* —
**not** *"the last byte has left the pin."* `TCIF6` fires when the final byte is written
into `DR`; up to two more character times of shifting follow. Buffer reuse is safe then;
powering down the USART is not — that needs USART `TC`, the same `TXE`-vs-`BTF`
distinction milestone 5's I2C teardown draws. `uart_write_dma()` also spins on
`dma_tx_busy` at entry, so back-to-back calls are safe but per-buffer synchronous — a
truly non-blocking version would return a status or hold a queue, unneeded for one
boot-time caller (noted as a tradeoff in-code).

No cache-coherency step is required: the F401 has no data cache (unlike the F407 / F7).
The source string lives in `.rodata` in flash, which DMA1's memory port reaches
(RM0368 §2.1.4) — only memory-to-memory transfers are DMA2-only. The polled
`uart_putc` / `uart_puts` path is unchanged; it must not run concurrently with a DMA
transfer (both write `DR`), but sequential hand-off is fine because `uart_putc` waits
for `TXE`.

### Verification

The boot line `USART2 TX now on DMA1 stream 6 / channel 4\r\n` is sent once, over DMA.
Everything after it — the polled `chip id:`, the sample loop — is itself proof the
transfer completed: `while (uart_tx_busy())` only releases when `DMA1_Stream6_IRQHandler`
clears the flag, and that handler only runs if IRQ 17's vector slot is right. A wrong
`CHSEL` or a wrong vector entry hangs the board with no output at all.

Captured at CN3-RX (PA2), decoded with `sigrok-cli`:

| Check | Result |
|---|---|
| DMA line + polled BME280 output, same pin | both byte-exact, **zero frame errors** |
| Bit period, DMA-sourced segments | 8.564–8.568 µs |
| Bit period, polled segments | 8.565–8.567 µs |
| Difference | ~3 ns — below the 41.7 ns sample period at 24 MHz |
| Longest edge interval, either stream | 6 bit-times, no more |

The two bit-period populations are **identical to within instrument resolution** —
there is nothing to distinguish because there is nothing different. Same USART, same
`BRR`, same PCLK1; only the master filling `DR` changed, and that does not touch the
bit clock. The 8.566 µs is ~1.44% fast of the 8.690 µs programmed rate — the
milestone-7 drift, not anything DMA introduced.

The stronger result is the **absence** of inter-byte stretch. Every edge interval in
the DMA-sourced line is a clean 1–6× multiple of the bit period, exactly like the
polled stream — the DMA controller kept `DR` fed continuously and was never once late
to a `TXE`. That rules out arbitration or bus-contention stalls, which is the thing
that would actually differ between a CPU-driven and a DMA-driven transmit. Checking for
what should be absent, not only confirming what should be present.

### Incidental — the floating-pin boot byte

The decode shows one junk byte immediately before the first real frame, every boot.
Between reset and the `MODER` / `AFRL` write, PA2 is a floating input — the line rests
wherever the pull-up and stray capacitance leave it, and the receiver decodes a
spurious start bit off the transition when the USART finally drives the pin. Every UART
on this board does it. It reads as a firmware bug to anyone who hasn't accounted for
pin state before initialization.

### Bench note — catching boot output

`openocd`'s SWD `reset` perturbs the ST-LINK's USB composite device and drops the
virtual COM port for ~1–2 s — long enough to lose everything the firmware prints in
that window, including a one-shot startup banner. Steady-state VCP capture is fine. For
boot output, use the analyzer on PA2 (a separate signal path) or the B2/NRST button
rather than the `reset` command.

---

## Verification approach

The through-line across all eight milestones: a correct-looking output is not evidence
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
- Confirming a rederived constant recompiled cleanly doesn't prove the clock changed
  under it — **re-measure every derived timing on hardware.**
- Confirming a replacement path emits the right bytes doesn't prove it behaves like what
  it replaced — **measure both on the wire, and check for what should be absent**, not
  only what should be present.

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
- Loading the SysTick reload value too high makes the clock run fast. *(It runs slow —
  the period is `RVR + 1`, so a too-large reload lengthens every tick. The shipped
  value was right; the comment explaining it wasn't.)*
- The post-PLL timing drift is the HSI self-heating at 84 MHz. *(Consistent with it, but
  never tested against PLL jitter or the analyzer's own uncharacterized timebase — and
  1.1–1.6% is outside the HSI's rated ±1%. Still open, and the commit that first stated
  it as resolved had to be walked back.)*
- Which DMA stream carries a peripheral's requests is something you configure. *(It's
  fixed in silicon — RM0368 Table 28. USART2_TX is DMA1 stream 6, channel 4, and
  nothing else routes it.)*