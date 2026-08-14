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

Compiled standalone, `gcc -Os -DNDEBUG`, x86-64 host, FPU disabled:

| | measured | note |
|---|---:|---|
| `.text` | **140,272 B** | RV32 estimate **~140–180 KB** (unmeasured) |
| `.data` | 2,176 B | |
| `.bss` | 4 B | |
| `sizeof(CPUI386)` | 512 B | currently `malloc`ed |
| TLB table | **12,288 B** | 512 entries × 24 B, currently `malloc`ed |
| **working RAM** | **~13 KB** | excludes guest RAM |

Guest RAM is external DRAM and not part of this budget — the single
biggest departure from the ESP32 build, which carries the whole PC.

Code size is the only number that matters. At ~140–180 KB it is ~40% of
the ECP5-85F's entire block RAM (3.7 Mbit ≈ 460 KB), so **the emulator
should run from external DRAM behind an I-cache**, not from on-chip
memory — particularly with BRAM and distributed RAM wanted for cache ways
elsewhere.

Enabling the x87 hooks costs only ~256 B inside `i386.c`, but `fpu.c` is
a further 22.5 KB of source and is the only part of the project that uses
floating point (51 references; the integer core has **zero**). Leaving it
out avoids pulling in soft-float entirely, which is why the first port
should build without it.

## What has to change

Small, and the embedded hooks largely exist already:

1. **Disable x87.** `I386_ENABLE_FPU` is `#define`d unconditionally at
   `i386.c:18`, but clean stubs are already written for the disabled case
   (`fpu_new(...) NULL`, `fpu_exec1(...) false`). One line.
2. **`get_nticks()`** (`i386.c:3713`) uses `clock_gettime(CLOCK_MONOTONIC)`
   via `<time.h>`. Replace with a read of the RISC-V `mcycle` CSR.
3. **Two `malloc`s** — `CPUI386` (`i386.c:5120`) and the TLB table
   (`i386.c:5129`). Replace with static buffers: 512 B + 12 KB.
4. **Stub `fprintf` and `abort`** (2 calls each, error paths only).
   `-DNDEBUG` removes the 14 `assert`s.
5. **Provide a `CPU_CB`.** Stubs first, then routed to the bus adapter.

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

- **Real RV32 text size** — the 140 KB figure is x86-64. Measure it.
- **Interrupt latency.** `cpui386_raise_irq` plus `pic_read_irq` implies
  the interpreter notices interrupts at its own cadence. Acceptable
  latency against real hardware peripherals is unquantified.
- **`phys_mem` window size and placement** in the RISC-V address map, and
  how MMIO regions are carved out of it.
- **Cache configuration** for the VexRiscv build — the measured
  Lite/Debug configs are cacheless and would be a poor fit; a
  cache-carrying build is estimated at 5–8k LUT4 but has not been
  measured.
