# tiny386 as an FPGA soft CPU — design notes

Fork-specific notes. Upstream tiny386 is a portable PC emulator; see
[`README.md`](README.md) for what it is and how to build it normally.
This document records why *this* fork exists and how it is intended to be
used, and nothing here changes the emulator's behaviour as a host
application.

> **Blocked:** how the emulator reaches the chipset is **not decided** —
> see [OPEN DECISION](#open-decision--settle-before-any-integration-work).
> The CPU-core port is unaffected and can proceed; integration work
> should not.

## Design goal

**A small x86 core that runs both in simulation and on hardware, slowly.**

tiny386's i386 emulator is retargeted to run on a **VexRiscv** soft core
inside a **Lattice ECP5** FPGA, part of a PC motherboard project whose
primary sockets take a real 386SX or 486DX. The soft-CPU path exists for
the configuration where no hard CPU is fitted, and for board bring-up and
chipset validation.

Speed is explicitly not a goal. Correct, observable, debuggable
behaviour is.

### tiny386 is a payload, not the architecture

The FPGA carries a general-purpose **MCU core (VexRiscv)**. tiny386 is
**one payload** it can run — not a fixed role, and **not** a replacement
for the 486 BIU interface bridge, which is independent RTL that payloads
talk *through* rather than implement.

Other payloads are equally valid on the same core: chipset test and
validation code, diagnostics, bootloaders, anything that fits the memory
region allocated for MCU code and data.

Two consequences:

- **Switching payloads is loading a different binary, not rebuilding the
  bitstream.** There is one MCU instance, not a per-role FPGA
  configuration.
- **The MCU is sized for the most demanding payload**, which is tiny386
  (~125 KB code, ~12 KB writable state, wants I- and D-cache). Everything
  else fits beneath that ceiling. This also makes the debug-enabled
  variant more attractive, since JTAG/GDB serves every payload rather
  than just this one.

## Why an emulated x86 rather than a hardware x86 core

Two properties a synthesised x86 core does not have (and note this
compares *payload* against *hard core* — the MCU itself is useful
regardless of which payload it runs):

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
**not compiled in**.

Every peripheral lives in **FPGA logic**, not software. Some are the real
chipset implementation; others are FPGA modules standing in for a period
device (video, for instance). Either way they are RTL, and the emulator's
I/O and memory callbacks route outward to them rather than to software
models.

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

## OPEN DECISION — settle before any integration work

**How the emulator reaches the chipset is not decided.** Nothing below
the CPU-core port should be built against an assumed answer, because the
answer changes the interfaces, the performance model, and what the
soft-CPU configuration is able to validate.

### It decomposes into three paths, not one

| path | frequency | status |
|---|---|---|
| guest RAM | very high | **decided** — L2/L3 + coherency to external DRAM |
| MMIO (VGA aperture) | high in graphics code | **OPEN** |
| port I/O | low | **OPEN** |

Guest RAM settles itself: on a real 486 system the CPU talks to L2 and
L2 talks to DRAM, so the soft CPU joining at the **L2 interface** is
faithful from L2 downward and skips only the 486 bus protocol. Hard CPU
and soft CPU converge at L2 and share everything beneath it.

### The two candidate shapes for I/O and MMIO

**A — internal fabric.** Callbacks reach chipset peripheral registers
over an internal bus, bypassing the 486 front end. Fast: a peripheral
access costs a few cycles.

**B — through the 486 front end ("virtual socket").** Callbacks drive an
internal adapter that generates 486 local-bus cycles into the same front
end the physical socket drives. The bus is entirely internal to the FPGA
— no pins, no level shifters, no timing closure against real silicon.
Costs roughly 100–300 ns per access.

### The argument for B

The chipset needs a 486 front end **regardless**, because the hard-CPU
configuration requires it. Given that it exists either way:

- **One path to validate instead of two.** Under A the soft CPU
  exercises a path the hard CPU never uses, and *cannot* exercise the
  path the hard CPU does — the opposite of what a validation vehicle
  should do.
- **No duplicate peripheral interfaces.** Peripherals get one bus-facing
  side rather than an internal one and a bus one.

The resulting shape: **one 486 front end, two possible masters** — the
physical socket or the internal soft-CPU adapter, selected by
configuration.

Port I/O is rare enough that the per-access cost is noise.

### The part that is genuinely undecided: the VGA aperture

This is the one high-traffic case, and it is a real trade rather than an
oversight:

- Through the front end: a 320x200 full-screen fill is 64,000 byte
  writes at ~200 ns ≈ **13 ms**. Tolerable. 640x480 or heavy blitting is
  proportionally worse, and this is where "slow" starts to be felt.
- Framebuffer in DRAM behind the cache, video module reading from there:
  writes run at cached-DRAM speed and the video module DMAs. Much
  faster — but the aperture then exercises **no** bus front end,
  reintroducing a second path for the highest-traffic peripheral.

Which is right depends on what the soft-CPU configuration is *for*:
primarily validation (route through the bus) or primarily a usable
no-hard-CPU product (DRAM-backed framebuffer).

### Also unresolved

**Adapter fidelity: cycle-accurate 486 timing, or protocol-correct
only?** Cycle-accurate allows validating timing-sensitive chipset paths;
protocol-correct is far easier and sufficient for functional validation.
A software emulator cannot produce authentic timing regardless, which
limits what fidelity buys here.

### Why this blocks integration

The `CPU_CB` callback implementations, the adapter (if any), the chipset
peripheral interfaces, and the performance model all follow from this
choice. Porting the CPU core itself is unaffected and can proceed — it
is the same code either way.

## Performance expectation

Rough, with wide error bars. Upstream boots Windows 9x on an ESP32-S3 at
240 MHz. VexRiscv at ~100 MHz with a simpler pipeline should land
**2.5–4× slower**, i.e. very roughly a **1–4 MHz equivalent 386**. Treat
that as an order-of-magnitude figure; it could be off by 2× either way,
and it depends heavily on cache behaviour on the path above.

That is slow for interactive use and entirely adequate for the intended
jobs: exercising the chipset, validating the cache and coherency block,
and providing a reference the RTL can be diffed against.

## Possible second role (not a requirement)

Upstream tiny386 runs on a workstation, so the same emulator could serve
as a host-side functional model — developing BIOS and test code before
hardware exists, or diffing bus transactions against the RTL.

Whether the host build is kept working is **an open question, not a
decided constraint.** Keeping it costs `#ifdef`s at each divergence and
buys the model role; abandoning it allows simpler bare-metal source. The
port changes made so far are conditional, so both remain possible.

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
