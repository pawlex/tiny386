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
which on VexRiscv goes through its cache hierarchy natively — no
per-access callback overhead on the hot path. `phys_mem` points at the
external DRAM window in the RISC-V address map.

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

Guest RAM is external DRAM and outside this budget — the single biggest
departure from the ESP32 build, which carries the whole PC.

At ~125 KB the code is ~27% of the ECP5-85F's block RAM (3.7 Mbit ≈
460 KB), so it *could* live on-chip, but **running from external DRAM
behind an I-cache is the better use of the device** given BRAM and
distributed RAM are wanted for cache ways. The writable working set
(~12 KB + stack) is small enough to keep in BRAM or TCM if the hot state
should be fast.

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

**`Full`** (3,908 LUT4, 96.55 MHz), or **`FullDebug`** (3,952 LUT4,
84.79 MHz) for JTAG/GDB — 44 more LUT4 but 12% less clock.

Note the core is a general-purpose MCU running **payloads**, of which
tiny386 is one; it is sized for the most demanding payload, and the
debug capability serves all of them rather than just the emulator, which
makes `FullDebug` easier to justify than it would be for a single-purpose
core.

Ruled out:

- **`Min`/`MinDebug`** — no cache and no mul/div. An interpreter fetching
  from DRAM uncached would be crippled, and the emulator uses multiply.
- **`Lite`** — I-cache but a *simple* D-bus, so every guest memory access
  goes uncached to DRAM. That is precisely the failure mode called out in
  FPGA-SOFTCORE.md.
- **`Linux`** — the MMU buys nothing; tiny386 maintains its own TLB in
  software (the 12 KB table above). Costs 30% more LUT4 and 23% of the
  clock versus `Full`.

## First milestone

Cross-compile for bare-metal RV32 and run under `qemu-riscv32`, stepping
hand-assembled x86 instructions out of a fake `phys_mem` array with
printing `CPU_CB` stubs.

A complete vertical slice — build, run, verify — with no FPGA and no
hardware, answering the three open questions: does it compile clean for
RV32, what is the real text size, and does it execute correctly when
built that way.

Toolchain: `gcc-riscv64-unknown-elf` (Debian 12.2.0). `qemu-riscv32` is
already present.

## Open questions

- **Interrupt latency.** `cpui386_raise_irq` plus `pic_read_irq` implies
  the interpreter notices interrupts at its own cadence. Acceptable
  latency against real hardware peripherals is unquantified.
- **`phys_mem` window size and placement** in the RISC-V address map, and
  how MMIO regions are carved out of it.
- **Cache configuration** for the VexRiscv build — the measured
  Lite/Debug configs are cacheless and would be a poor fit; a
  cache-carrying build is estimated at 5–8k LUT4 but has not been
  measured.
