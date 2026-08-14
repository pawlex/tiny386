# Port analysis: i386 core → bare-metal RV32 (VexRiscv)

Measured, not estimated, except where marked. Companion to
[FPGA-SOFTCORE.md](FPGA-SOFTCORE.md), which covers *why*; this covers
*what it costs and what has to change*.

## Verdict

The port is small. The emulator's public interface is already the right
shape for this target, the integer core has no floating point, and its
entire libc surface is four functions. Nothing found so far argues
against the approach.

## The integration surface is already correct

`i386.h` is the whole public interface, and it splits exactly the way an
FPGA target wants:

```c
CPUI386 *cpui386_new(int gen, char *phys_mem, long phys_mem_size, CPU_CB **cb);
void     cpui386_step(CPUI386 *cpu, int stepcount);
void     cpui386_raise_irq(CPUI386 *cpu);
```

**Physical memory is a flat buffer pointer, not a callback.** Guest RAM
access is therefore a plain load/store from the emulator's point of view,
with no per-access callback overhead in the emulator itself. Where
`phys_mem` points, and what services those loads and stores, is a
property of the MCU address map and the memory subsystem — **not decided
here** (see FPGA-SOFTCORE.md). The architecture decision is that such
accesses leave the MCU through the 486 BIU.

Only the rare paths are callbacks, and those are precisely the ones that
should reach real hardware:

| `CPU_CB` member | routes to |
|---|---|
| `io_read8/16/32`, `io_write8/16/32` | chipset port I/O, via the bus adapter |
| `io_read_string`, `io_write_string` | `INS`/`OUTS` |
| `iomem_read8/16/32`, `iomem_write8/16/32` | MMIO — VGA aperture etc. |
| `iomem_write_string` | block MMIO writes |
| `pic_read_irq` | interrupt vector fetch from the hardware PIC |

This is the same split the design notes assume, so no restructuring of
upstream code is needed to get it.

## Footprint

**Measured on the real target.** `riscv64-unknown-elf-gcc 12.2.0`,
`-Os -DNDEBUG -DI386_DISABLE_FPU -DTINY386_STATIC_ALLOC`, picolibc.

Core object alone:

| target | `.text` | vs x86-64 |
|---|---:|---:|
| `rv32im` | 159,361 B | +14% |
| `rv32imc` | **119,917 B** | −15% |
| x86-64 (host, with x87) | 140,422 B | — |

Fully linked image (`rv32imc`, semihosted crt0, static allocation):

| section | bytes |
|---|---:|
| `.text` (emulator + picolibc + crt0) | 125,468 |
| `.data` | 152 |
| `.bss` | 75,412 *(incl. 65,536 test-harness `phys_mem`)* |
| `.stack` | 2,048 |

Subtracting the harness's fake guest RAM, **the emulator's own baseline
is ~125 KB code and ~12 KB writable state** (TLB 12,288 B + `CPUI386`
512 B + libc). picolibc and the semihosting layer cost only ~5.5 KB on
top of the core, because so little of it is used.

Guest RAM lives outside the MCU and is not part of this budget — the
single biggest departure from the ESP32 build, which carries the whole
PC in its own address space.

At ~125 KB the code is ~27% of the ECP5-85F's block RAM (3.7 Mbit ≈
460 KB), and **the architecture decision is that MCU code and data live
in local BRAM** — see FPGA-SOFTCORE.md. Everything outside that region
leaves the MCU as 486 bus cycles. ~137 KB total (code + writable state)
leaves roughly 70% of the device's BRAM for everything else.

Note `rv32imc` is 25% smaller than `rv32im` — but see the variant
section: compressed instructions cost 40% of the clock on ECP5, so
`rv32im` on a `Full` core is the faster combination despite the larger
binary.

x87 is excluded. The hooks cost only ~256 B inside `i386.c`, but `fpu.c`
is a further 22.5 KB of source and is the only part of the project using
floating point (51 references; the integer core has **zero**), so leaving
it out avoids soft-float entirely.

## What has to change

Four of five are **done** and in-tree, all conditional so no existing
build changes behaviour:

1. ~~**Disable x87.**~~ **DONE** — `I386_ENABLE_FPU` is now opt-*out* via
   `-DI386_DISABLE_FPU`. Host builds keep x87 by default.
2. ~~**`get_nticks()`**~~ **DONE** — under
   `#if defined(__riscv) && __riscv_xlen == 32` it reads the 64-bit cycle
   counter with `rdcycle`/`rdcycleh` (retry loop guards the hi/lo carry).
   This feeds the emulated `RDTSC`, and counting CPU cycles is closer to
   real x86 TSC semantics than the host build's nanoseconds.
   **Caveat: the `cycle` CSR is optional in RISC-V.** The VexRiscv build
   must provide it — all measured LiteX variants do (`CsrPlugin_mcycle`).
3. ~~**Two `malloc`s**~~ **DONE** — static under
   `-DTINY386_STATIC_ALLOC` (512 B + 12 KB).
4. ~~**Stub `fprintf` and `abort`**~~ **NOT NEEDED** — picolibc's
   semihosting crt0 (`--crt0=semihost -lsemihost`) provides `stderr`,
   `stdout` and `_exit`, which also gives working printf debugging under
   qemu for free. One genuine stub *was* required and is not in the
   original list: **`usleep()`**, called on an idle path at
   `i386.c:5030`. A no-op suffices for functional testing; on the FPGA it
   should become a `WFI` or a yield to the peripheral service loop.
5. **Provide a `CPU_CB`.** Still open — and blocked on the integration
   decision in FPGA-SOFTCORE.md, since the callbacks' destination is
   exactly what is undecided.

Precedent already in-tree: `#ifdef BUILD_ESP32` with `IRAM_ATTR` /
`DRAM_ATTR` at `i386.c:7`, and an `__wasm__` conditional at line 15. The
codebase already expects to be built for constrained targets.

### Full libc surface of the integer core

```
14x assert     -> compiled out with -DNDEBUG
 2x malloc     -> static
 2x fprintf    -> stub
 2x abort      -> stub
```

That is the complete list. No file I/O, no string formatting in the hot
path, no dynamic allocation beyond startup.

## Host core: which VexRiscv variant

All 19 pre-generated LiteX variants were characterised; 10 were measured.
LFE5U-85F, CABGA381, speed 7, out-of-context, yosys 0.68 + nextpnr-ecp5,
`--freq 100`.

| variant | LUT4 | %85F | DFF | Fmax | I$ | D$ | RVC | MMU | mul/div |
|---|---:|---:|---:|---:|---|---|---|---|---|
| `Min` | 1,656 | 1% | 856 | 108.10 MHz | simple | simple | no | no | no |
| `MinDebug` | 1,712 | 2% | 902 | 108.25 MHz | simple | simple | no | no | no |
| `Lite` | 3,000 | 3% | 1,090 | 102.01 MHz | cached | simple | no | no | no |
| `VexRiscv` | 3,304 | 3% | 1,527 | 86.83 MHz | cached | cached | no | no | yes |
| `Debug` | 3,595 | 4% | 1,638 | 99.06 MHz | cached | cached | no | no | yes |
| **`Full`** | **3,908** | 4% | 1,703 | **96.55 MHz** | cached | cached | no | no | yes |
| `FullDebug` | 3,952 | 4% | 1,814 | 84.79 MHz | cached | cached | no | no | yes |
| `IMAC` | 4,533 | 5% | 1,768 | 57.43 MHz | cached | cached | **yes** | no | yes |
| `IMACDebug` | 4,712 | 5% | 1,880 | 56.87 MHz | cached | cached | **yes** | no | yes |
| `Linux` | 5,085 | 6% | 2,342 | 74.44 MHz | cached | cached | no | **yes** | yes |

Every cached variant uses **10 BRAM of 208 (4%)** and **zero RAM LUTs** —
the caches sit entirely in block RAM and consume none of the distributed
RAM pool. All variants provide `CsrPlugin_mcycle`, so the `rdcycle`-based
`RDTSC` works on any of them.

### RVC is not worth it on ECP5

The emulator is 25% smaller built with compressed instructions
(119,917 B vs 159,361 B), which initially looked decisive. Measurement
inverted it: `IMAC` runs at **57.43 MHz against `Full`'s 96.55** — a 40%
Fmax loss, plus 16% more LUT4. The decompressor lands on the critical
path.

| | `Full` | `IMAC` |
|---|---:|---:|
| clock | 96.55 MHz | 57.43 MHz |
| emulator `.text` | 159,361 B | 119,917 B |
| relative throughput | **1.68x** | 1.0x |

Smaller code improves I-cache hit rate, but it would have to be worth
**68%** to break even. For a throughput-bound interpreter it will not be.

### Recommendation

> **Superseded premise.** The comparison below assumed the interpreter
> runs from external memory and therefore needs caches. The architecture
> decision is now BRAM-resident MCU code/data with an *uncached* guest
> region reached over the 486 BIU, which weakens that reasoning
> considerably. The measurements stand; the recommendation should be
> re-derived against the real configuration. `Lite` and `Min` remain out
> on mul/div grounds regardless.


**`Full`** (3,908 LUT4, 96.55 MHz), or **`FullDebug`** (3,952 LUT4,
84.79 MHz) for JTAG/GDB — 44 more LUT4 but 12% less clock.

Note the core is a general-purpose MCU running **payloads**, of which
tiny386 is one; it is sized for the most demanding payload, and the
debug capability serves all of them rather than just the emulator, which
makes `FullDebug` easier to justify than it would be for a single-purpose
core.

Ruled out:

- **`Min`/`MinDebug`** — no mul/div, which the emulator uses. This holds
  regardless of the memory architecture.
- **`Lite`** — no mul/div either. Its cacheless D-bus mattered under the
  superseded premise; the mul/div gap rules it out either way.
- **`Linux`** — the MMU buys nothing; tiny386 maintains its own TLB in
  software (the 12 KB table above). Costs 30% more LUT4 and 23% of the
  clock versus `Full`.

## First milestone — REACHED

**The i386 core executes correctly on bare-metal RV32.** Full vertical
slice — cross-compile, link, run, verify — with no FPGA and no hardware.
Reproduce with `make -C rv32 run`.

    tiny386 on RV32 -- milestone test
    phys_mem = 1048576 bytes, payload 59 bytes @ 0x1000

       OUT32 port=0x080 <= 0x00000019     25 = 5^2
       OUT32 port=0x080 <= 0x00000010     16 = 4^2
       OUT32 port=0x080 <= 0x00000009      9 = 3^2
       OUT32 port=0x080 <= 0x00000004      4 = 2^2
       OUT32 port=0x080 <= 0x00000001      1 = 1^2
       OUT32 port=0x080 <= 0xa5a5a5a5     memory round-trip
       OUT32 port=0x080 <= 0x00000042     byte store + movzx
       OUT32 port=0x080 <= 0xdeadbeef     end marker
    RESULT: PASS

Every emitted value is **computed**, so none of it can come from
anywhere but real execution — a constant would prove nothing. The squares
require `IMUL` and a decrementing `ECX` via `LOOP`; `0xa5a5a5a5` survives
a store, an `XOR EAX,EAX` clobber and a reload through `phys_mem`; `0x42`
exercises byte-width access and `MOVZX`.

Toolchain: `gcc-riscv64-unknown-elf` 12.2.0 + `picolibc` (Debian),
`qemu-system-riscv32`, `--crt0=semihost`. picolibc's semihosting crt0
supplies `stdout`/`stderr`/`_exit`, so printf debugging works under qemu
for free.

The `CPU_CB` stubs **print only** — nothing is routed anywhere, which
keeps the milestone clear of the undefined memory and peripheral paths.

### Not covered by this milestone

- **`rdcycle` is unexercised.** The `RDTSC` path is compiled in but the
  payload never executes `RDTSC`. qemu implements the CSR; a VexRiscv
  build might not (it is optional in RISC-V).
- **Interrupts are untouched.** `pic_read_irq` returns -1 and nothing
  raises an IRQ.
- **`rv32imc` is not the build default.** `rv32im` is, because compressed
  instructions cost ~40% of Fmax on ECP5 — see the variant section.

## Open questions

- **Interrupt latency.** `cpui386_raise_irq` plus `pic_read_irq` implies
  the interpreter notices interrupts at its own cadence. Acceptable
  latency against real hardware peripherals is unquantified.
- **`phys_mem` placement and guest RAM sizing** — deferred with the
  memory subsystem, not blocking the core port.
- **Cache configuration** for the VexRiscv build — the measured
  Lite/Debug configs are cacheless and would be a poor fit; a
  cache-carrying build is estimated at 5–8k LUT4 but has not been
  measured.
