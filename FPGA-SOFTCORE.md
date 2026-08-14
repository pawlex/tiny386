# tiny386 as an FPGA soft CPU — design notes

Fork-specific notes. Upstream tiny386 is a portable PC emulator; see
[`README.md`](README.md) for what it is and how to build it normally.
This document records why *this* fork exists and how it is intended to be
used, and nothing here changes the emulator's behaviour as a host
application.

## Design goal

**A small x86 core that runs both in simulation and on hardware, slowly.**

tiny386's i386 emulator is retargeted to run on a **VexRiscv** soft core
inside a **Lattice ECP5** FPGA, as the CPU for a PC motherboard project
whose primary sockets take a real 386SX or 486DX. The soft-CPU path
exists for the configuration where no hard CPU is fitted, and for board
bring-up and chipset validation.

Speed is explicitly not a goal. Correct, observable, debuggable
behaviour is.

## Why this instead of a hardware x86 core

Two properties a synthesised x86 core does not have:

1. **It is a functional model.** It runs on a workstation today, is
   instrumentable, and can be diffed against RTL. A real 386SX/486DX in
   a socket cannot be simulated at all, which puts it in the critical
   path for every question it could answer. A soft core in an FPGA can
   be halted, single-stepped and traced.
2. **It is small.** The emulator is software; the FPGA cost is only the
   RISC-V that runs it, leaving the device budget for the chipset,
   cache/coherency block and instrumentation.

Measured on **LFE5U-85F**, CABGA381, speed 7, out-of-context, with
yosys 0.68 + nextpnr-ecp5 (no vendor tools):

| core | LUT4 | % of 85F | Fmax |
|---|---:|---:|---:|
| PicoRV32 | 1,782 | 2.1% | 111.30 MHz |
| VexRiscv Lite | 3,000 | 3.6% | 102.01 MHz |
| VexRiscv Debug (JTAG+GDB) | 3,595 | 4.3% | 99.06 MHz |
| ao486 (hardware 486 core) | 37,895 | 45% | 32.87 MHz |

The configurations above are **cacheless**; a build carrying the
emulator wants I- and D-cache, so budget roughly **5–8k LUT4** — still
around a tenth of what a hardware x86 core costs.

## Scope: the CPU core only

Only the **i386 emulator** is used. `i386.c` (~6K LOC), `i386ins.def`,
and optionally `fpu.c` for x87.

The peripherals tiny386 ports from TinyEMU/QEMU — 8259 PIC, 8254 PIT,
8042, CMOS RTC, VGA, IDE, NE2000, DMA, PC speaker, Adlib, SB16 — are
**not compiled in**. Those exist as real logic in the FPGA chipset. The
emulator's I/O and memory callbacks are routed outward to that hardware
instead of to software models.

This is the main structural difference from the ESP32 port, which brings
the whole PC with it.

## Memory architecture

**Guest memory is FPGA external memory, reached through the L2/L3 cache
controller and coherency units** — not held inside the emulator's own
address space.

```
tiny386 i386 core  (running on VexRiscv)
        |
        |  memory read/write callbacks
        v
  L2 / L3 cache controller + coherency units
        |
        v
  FPGA external memory (DRAM)

  I/O port callbacks -> chipset peripheral logic
  VGA aperture / MMIO -> chipset, through the same coherency path
```

Consequences worth keeping in view:

- The emulator does **not** need to hold 640 KB+ of guest RAM locally.
  Its own footprint is code (~80–150 KB of RV32 text, estimated) plus a
  few tens of KB of CPU state. That is the single biggest departure from
  the ESP32 build and what makes the FPGA target comfortable.
- Guest accesses go through the cache hierarchy, so the **cache is doing
  real work** and is itself exercised by anything the emulator runs.
  Cache and coherency bugs surface as guest misbehaviour.
- **Do not route ordinary guest RAM access directly out as bus cycles.**
  If every emulated access became a bus transaction (~100–300 ns), at
  2–4 accesses per instruction the interpreter would sit near 1 µs per
  instruction and performance would collapse. The cache hierarchy is
  what makes this viable; only I/O and apertures should reach the
  peripheral bus.

## Performance expectation

Rough, with wide error bars. Upstream boots Windows 9x on an ESP32-S3 at
240 MHz. VexRiscv at ~100 MHz with a simpler pipeline should land
**2.5–4× slower**, i.e. very roughly a **1–4 MHz equivalent 386**. Treat
that as an order-of-magnitude figure; it could be off by 2× either way,
and it depends heavily on cache behaviour on the path above.

That is slow for interactive use and entirely adequate for the intended
jobs: exercising the chipset, validating the cache and coherency block,
and providing a reference the RTL can be diffed against.

## Dual role

The same source serves two purposes, and the host build is useful
immediately, before any FPGA work:

1. **Host-side functional model** — develop and debug BIOS and test code
   before hardware exists; run the same ROM image in tiny386 and in the
   RTL testbench and compare the resulting bus transactions.
2. **FPGA soft CPU** — the same emulator on VexRiscv, driving the real
   chipset.

Keeping both paths building from one source is a design constraint, not
an accident: divergence between them destroys the value of the first.

## Port analysis

Measured footprint, the exact changes required, and the first milestone
are in **[PORT-ANALYSIS.md](PORT-ANALYSIS.md)**. Summary: the integer
core is ~140 KB of text and ~13 KB of working state, has zero floating
point, and its entire libc surface is 14 `assert`, 2 `malloc`,
2 `fprintf` and 2 `abort`.

## Known risks

- **Interrupt latency** will be poor if the interpreter polls. Hardware
  peripherals raising IRQs may need a different mechanism than the
  software-model path assumes.
- **No cycle-accurate bus timing.** A software emulator will not
  reproduce 486 bus timing. For validation this is partly a feature —
  transactions are issued deliberately rather than as a side effect of
  prefetch — but timing-sensitive chipset paths cannot be tested this
  way.
- **Unimplemented features.** Upstream notes hardware task switching,
  some permission checks, and debug registers are missing. Fine for the
  intended use; worth knowing before assuming full 386 semantics.
