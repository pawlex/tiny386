# Port analysis — project-specific notes

The port itself, its measurements, and the qemu smoke test are documented
in **[rv32/README.md](rv32/README.md)**, which is written to be
project-neutral and is what was contributed upstream. This file holds
only what is specific to *this* project; see
[FPGA-SOFTCORE.md](FPGA-SOFTCORE.md) for why the project exists.

Summary of the port, from rv32/README.md: three conditional changes to
`i386.c` (x87 opt-out, `rdcycle`-based `get_nticks`, static allocation),
plus a `usleep()` stub. ~125 KB of `.text` and ~12 KB of writable state.
Verified executing on bare-metal RV32 under qemu.

## What the footprint means here

**~137 KB total** (code + writable state, excluding guest RAM) against
**462 KB of BRAM** on an ECP5-85F — roughly **30%**.

The architecture decision is that MCU code and data live in local BRAM
and everything outside that region leaves the MCU as 486 bus cycles, so
this figure is the MCU's whole on-chip footprint. It leaves ~70% of the
device's BRAM for everything else.

The MCU is sized for the most demanding payload, and tiny386 is it —
other payloads (chipset tests, diagnostics, bootloaders) fit beneath this
ceiling.

## Where the callbacks go

`CPU_CB` is the emulator's only outward interface, and it maps onto the
project's structure directly:

| `CPU_CB` member | routes to |
|---|---|
| `io_read8/16/32`, `io_write8/16/32` | port I/O, out through the 486 BIU |
| `io_read_string`, `io_write_string` | `INS`/`OUTS` |
| `iomem_read8/16/32`, `iomem_write8/16/32` | MMIO, out through the 486 BIU |
| `iomem_write_string` | block MMIO writes |
| `pic_read_irq` | interrupt vector fetch |

Guest RAM is *not* a callback — `phys_mem` is a flat buffer pointer, so
guest memory access is a plain load/store with no per-access callback
overhead in the emulator. Where that pointer aims, and what services
those accesses, is a property of the MCU address map and is **deferred
with the memory subsystem** (see FPGA-SOFTCORE.md).

**The `CPU_CB` implementation is the one remaining port task**, and it is
blocked on the integration decision — the callbacks' destination is
exactly what is undecided.

## Host core selection: deferred

rv32/README.md measures ten VexRiscv variants on the ECP5-85F and
recommends `Full` (3,908 LUT4, 96.55 MHz), on the reasoning that an
interpreter running from external memory needs caches.

**That reasoning does not apply here.** With MCU code and data in local
BRAM and the guest region deliberately uncached, the MCU's own caches
matter far less. The measurements stand; the recommendation does not
transfer.

Re-deriving it needs to know what sits behind the BIU, so **it is
deferred with the memory subsystem**. What holds regardless:

- `Min` and `Lite` are out — no mul/div, which the emulator uses.
- `Linux`'s MMU buys nothing; tiny386 keeps its own software TLB (the
  12 KB table).
- Every variant provides `mcycle`, so the `rdcycle`-based `RDTSC` works
  on any of them — but **`cycle` is optional in RISC-V** and a build
  without it will read zero or trap. This is unexercised so far: the
  smoke-test payload never executes `RDTSC`.

## Open, and actionable now

Nothing here depends on the memory subsystem:

- **Which BIOS** — SeaBIOS assumes some 486+ behaviour in places; the
  older Bochs legacy BIOS targets earlier hardware more directly.
- **Which BIOS shortcuts are emulator-conditional** rather than
  unconditional. Conditional ones would have tiny386 and a hard CPU
  validating different paths.
- **Storage / boot device.**
- **IRQ delivery** into the interpreter — polled in the step loop, or a
  RISC-V interrupt. The whole interrupt path is untested.
- **Extending the payload** toward real-mode and segmentation coverage,
  which is where a stock BIOS actually starts. The current payload runs
  flat 32-bit via `cpui386_reset_pm()`.

## Deferred with the memory subsystem

Listed so they read as *scheduled* rather than pending:

- host core variant re-derivation
- `phys_mem` placement and guest RAM sizing
- MCU address map, and how MMIO regions are carved out of it
