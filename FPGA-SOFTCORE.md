# tiny386 as an FPGA payload — design notes

Fork-specific notes. Upstream tiny386 is a portable PC emulator; see
[`README.md`](README.md) for what it is and how to build it normally.
This document records why *this* fork exists and how it is intended to be
used. Nothing here changes the emulator's behaviour as a host
application.

- **[rv32/README.md](rv32/README.md)** — the port itself, its
  measurements, and the qemu smoke test. Project-neutral; this is what
  was contributed upstream.
- **[PORT-ANALYSIS.md](PORT-ANALYSIS.md)** — what the port means for
  *this* project, and what is still open.

## What this is for

**A validation and enabling vehicle. Nothing more.**

The project is a PC motherboard whose primary sockets take a real 386SX
or 486DX. tiny386 runs as a software payload on a **VexRiscv MCU** inside
a **Lattice ECP5** FPGA, and exists to exercise the 486 bus interface and
everything behind it — **in Verilator, where every signal is
observable.** On real hardware it would not be.

It is explicitly *not* a product CPU option, and it is not judged on
being a pleasant machine to use. It is judged on whether it drives the
hardware faithfully and observably.

Consequences of that framing:

- **Performance is not a design input.** It matters only where it makes
  iteration painful (see the simulation-depth note below).
- **Realistic access patterns are the point.** A synthetic bus exerciser
  could poke the chipset, but only a real x86 workload produces the
  traffic a real CPU will produce.

### tiny386 is a payload, not the architecture

The FPGA carries a general-purpose **MCU core (VexRiscv)**. tiny386 is
**one payload** it can run — not a fixed role, and **not** a replacement
for the 486 BIU interface bridge, which is independent RTL that payloads
talk *through* rather than implement.

Other payloads are equally valid on the same core: chipset test and
validation code, diagnostics, bootloaders — anything that fits the memory
region allocated for MCU code and data.

- **Switching payloads is loading a different binary**, not rebuilding
  the bitstream. One MCU instance, not a per-role FPGA configuration.
- **The MCU is sized for the most demanding payload**, which is tiny386
  (~125 KB code, ~12 KB writable state). Everything else fits beneath
  that ceiling, and debug capability serves every payload rather than
  just this one.

## Scope: DOS

**The target is DOS.** Windows is a long-stretch goal, not a requirement.

What that settles:

- **x87 stays out.** DOS does not need it. `-DI386_DISABLE_FPU` avoids
  22.5 KB of `fpu.c` and soft-float entirely.
- **Upstream's unimplemented features stop mattering.** Hardware task
  switching, some permission checks and debug registers are missing;
  DOS uses none of them. They become the stretch goal's problem.
- **Graphics-mode performance stops mattering.** DOS text mode is
  80x25x2 = 4,000 bytes at `0xB8000` with small incremental updates, so
  aperture traffic is low. (A 320x200 graphics fill would be 64,000
  writes — that case is out of scope.)
- **Guest RAM is modest.** 640 KB conventional plus a few MB extended;
  4–16 MB covers it.

## Architecture: everything external goes out as 486 bus cycles

**Decided.** Accesses that are not to local BRAM-based code/data leave
the MCU **looking like native 486 local-bus cycles**, through the 486 BIU
bridge.

```
  VexRiscv MCU
      |
      |-- local: BRAM code + data ....... stays inside
      |
      `-- everything else --> 486 BIU ==>| MEMORY SUBSYSTEM   |
                                         | *** UNDEFINED ***  |
                                         |                    |
                                         | peripheral decode  |
                                         | (RTL, register-    |
                                         |  compatible)       |
```

Everything to the right of the BIU is **out of scope for this document
and deliberately undefined** — see "The memory subsystem is undefined"
below.

The split is **by memory region**, not by access type. Simpler to
implement, simpler to reason about, and the emulator does not have to
classify accesses at all.

### Why

Observability is the entire reason for a soft CPU. Routing external
access through the BIU means **the BIU and everything behind it are
exercised in Verilator**, with every signal visible. Bypassing it to save cycles would trade away the thing that
cannot be recovered on hardware for a thing that has been declared not to
matter.

It also guarantees the test payload and the emulator payload exercise the
*same* path — otherwise the tester validates something the emulator never
uses.

It is also faithful to what a real 486 does: guest RAM access leaves the
CPU as bus cycles. What services those cycles on the far side is the
memory subsystem's business, not the emulator's.

### Implication: the guest region should be uncached in the MCU

Following from the above. A cache *inside the MCU* would absorb guest
traffic **before it reaches the BIU** — hiding exactly what the BIU
exists to expose, and doing so invisibly. Whatever caching the system
has belongs on the far side of the boundary, where it is observable.

Note this reasoning does not depend on what the memory subsystem looks
like: it holds whether or not there is a cache out there, because the
argument is about where traffic becomes visible.

Knock-on: with MCU code/data in BRAM and guest accesses deliberately
uncached, **the MCU's own caches matter far less** than the variant
analysis in rv32/README.md assumed — that recommendation was built on
"the interpreter runs from external memory and needs caches." Deferred
with the memory subsystem; see PORT-ANALYSIS.md.

### Accepted cost

Every guest memory access becomes a BIU transaction and pays the bus
handshake, regardless of what services it on the far side. A deliberate
trade, not an oversight.

## The memory subsystem is undefined

**Treated as undefined, deliberately, and nothing here should assume a
design for it.** It is being specified last, on purpose.

What *is* defined is the **contract at the BIU boundary**: native 486
local-bus cycles in, whatever the subsystem chooses out. That constrains
what the subsystem must accept, not how it works. Ways, coherency
protocol, fill and evict policy, hierarchy depth, DRAM scheduling,
address mapping — all open.

Not assumed by anything above:

- that a cache exists at all, or how many levels
- any particular coherency scheme
- where DRAM sits, or that DRAM is the backing store
- any latency, hit rate or bandwidth figure
- that the MCU can address guest memory directly rather than only
  through the BIU

**Convention:** if a design decision anywhere expects a particular
memory-subsystem design, flow or path, it gets flagged and hashed out
rather than assumed. Statements about the *MCU side* of the boundary
(BRAM-resident payload, uncached guest region) are not
memory-subsystem assumptions — they describe what the MCU does before
the BIU.

## BIOS and peripherals

**SeaBIOS or the Bochs BIOS.** Both are written against standard PC
hardware at the register level — 8259 PIC pair, 8254 PIT, 8042, CMOS/RTC
at `0x70`/`0x71`, VGA at the usual ports and aperture. Neither is
parameterised for custom peripherals.

That decides the peripheral specification: **register-compatible period
devices.** The alternative is forking a BIOS. Compatibility is also what
a real 386SX/486DX in the socket requires, so building the peripherals
any other way would mean validating hardware the hard CPU can never use.

Every peripheral lives in **FPGA logic**, not software — some the real
chipset implementation, others FPGA modules standing in for a period
device (video, for instance). Either way they are RTL, and the emulator's
`io_*` / `iomem_*` callbacks route outward to them.

### Take every emulation shortcut the BIOS offers

**Decided.** Memory size from CMOS rather than probing, skip the memory
test, fast A20 via port `0x92` rather than the keyboard-controller dance,
minimal device enumeration.

Each is both less RTL to write and less simulation time to burn — and
the second reason is the sharper one. **Simulation is three layers deep:**
Verilator running VexRiscv running tiny386 running the BIOS. Verilator
manages a few MHz on a core this size; tiny386 costs on the order of 100
host instructions per guest instruction; so the guest runs at tens of
kHz. A POST executing a few million instructions is minutes. A POST that
memory-tests 4 MB is tens of millions of accesses and becomes half an
hour or worse. Shortcuts are the difference between an iterable loop and
an overnight run.

**One distinction to keep straight:** universal BIOS shortcuts are free,
because the real CPU gets them too. Shortcuts *conditional on detecting
an emulator* are not — tiny386 would take the easy branch and a real
386SX/486DX would take the other, validating a path the hard CPU never
uses. Same class of error as bypassing the BIU. Which shortcuts are
unconditional versus detection-gated is **unverified**.

## Acceptance criterion

**An unmodified stock BIOS completes POST against the FPGA peripherals,
in Verilator.**

Sharper and more testable than "the chipset works", and it exercises the
BIU, the peripherals, the cache path and the emulator together — a
stronger signal than any synthetic bus test. Worth freezing as a
regression.

## Accepted limitations

- **Timing-dependent paths are not validated in simulation.** A software
  emulator will not reproduce 486 bus timing, so wait states, refresh
  interaction and tight I/O recovery cannot be proven this way. **This
  gap is accepted**; close it on hardware later.
- **Interrupt latency** will be poor if the interpreter polls. Hardware
  peripherals raising IRQs may need a different mechanism than the
  software-model path assumes.
- **Upstream feature gaps** — hardware task switching, some permission
  checks, debug registers. Out of scope for DOS; in scope for Windows.

## Possible second role (not a requirement)

Upstream tiny386 runs on a workstation, so the same emulator could serve
as a host-side functional model. Whether the host build is kept working
is **an open question, not a decided constraint** — keeping it costs
`#ifdef`s at each divergence and buys the model role. The port changes
made so far are conditional, so both remain possible.

## Open questions

- **Which BIOS.** SeaBIOS assumes some 486+ behaviour in places; the
  older Bochs legacy BIOS targets earlier hardware more directly.
  Unverified which suits a 386-class target better. tiny386 already ships
  with SeaBIOS, whose expectations are known-good against tiny386's
  *software* peripherals — a useful reference when the RTL ones
  misbehave.
- **Which BIOS shortcuts are emulator-conditional** rather than
  unconditional (see above).
- **Storage / boot device.** DOS needs one. Not yet addressed.
- **IRQ delivery** from hardware peripherals into the interpreter —
  polled in the step loop, or a RISC-V interrupt.
- **Host core variant**, re-evaluated for BRAM-resident code and an
  uncached guest region.
- **Guest RAM size and address-map placement**, and how MMIO regions are
  carved out of it.
