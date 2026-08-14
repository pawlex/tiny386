# Bare-metal RV32 build

Runs tiny386's i386 core on a bare-metal 32-bit RISC-V target — no OS, no
heap, no host libc beyond a small embedded one. Verified under
`qemu-system-riscv32`.

```sh
make run        # build and run the smoke test under qemu
make            # build only
make payload    # re-assemble test386.asm -> test386.h (needs nasm)
```

`test386.h` is generated but committed, so `nasm` is only required to
change the payload.

Needs `gcc-riscv64-unknown-elf`, `picolibc-riscv64-unknown-elf` and
`qemu-system-riscv32` (all packaged on Debian).

## What it prints

```
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
```

Every emitted value is **computed** rather than constant, so the output
cannot be produced by anything except real execution. The squares need
`IMUL` and a decrementing `ECX` via `LOOP`; `0xa5a5a5a5` survives a store,
an `XOR EAX,EAX` clobber and a reload through `phys_mem`; `0x42` exercises
byte-width access and `MOVZX`.

## Changes to the core

Three, all conditional. **Default builds are unaffected** — with no new
macros defined and on a non-RISC-V target, `i386.c` compiles exactly as
before.

| change | guard | why |
|---|---|---|
| x87 becomes opt-*out* | `-DI386_DISABLE_FPU` | The integer core uses no floating point at all (`fpu.c` has the project's only 51 FP references), so dropping it avoids pulling soft-float into targets without an FPU. Stubs for the disabled case already existed. |
| `get_nticks()` reads `rdcycle`/`rdcycleh` | `#if defined(__riscv) && __riscv_xlen == 32` | Bare metal has no `clock_gettime`. This feeds the emulated `RDTSC`, and counting CPU cycles is arguably closer to real x86 TSC semantics than the host build's nanoseconds. |
| two `malloc`s become static | `-DTINY386_STATIC_ALLOC` | No heap. `CPUI386` is 512 B and the TLB table 12 KB; a single CPU instance is assumed. |

Plus `baremetal_stubs.c`, providing `usleep()` — called on an idle path
at `i386.c:5030`. A no-op is fine for a functional test; a real embedded
integration would make it a `WFI` or a yield.

Nothing else was required. picolibc's semihosting crt0 supplies
`stdout`, `stderr` and `_exit`, so no `fprintf`/`abort` stubs are needed
and `printf` debugging works under qemu for free.

## Footprint

`-Os -DNDEBUG`, FPU disabled, picolibc:

| target | `.text` |
|---|---:|
| `rv32im` | 159,361 B |
| `rv32imc` | 119,917 B |
| x86-64 (host reference, with x87) | 140,422 B |

Writable state is small: `sizeof(CPUI386)` 512 B plus a 12,288 B TLB
table (512 entries × 24 B), so **~12 KB excluding guest RAM**. A fully
linked image with picolibc and the semihosting crt0 is ~125 KB of
`.text` — the C library adds only ~5.5 KB, since little of it is used.

## Why `rv32im` and not `rv32imc`

Compressed instructions make the emulator **25% smaller**, which looks
decisive until you measure the core running it.

Tested against **VexRiscv** (the pre-generated LiteX variants) on a
**Lattice ECP5 LFE5U-85F**, CABGA381, speed grade 7, out-of-context,
with yosys 0.68 + nextpnr-ecp5:

| variant | LUT4 | Fmax | RVC |
|---|---:|---:|---|
| `VexRiscv_Full` | 3,908 | **96.55 MHz** | no |
| `VexRiscv_IMAC` | 4,533 | **57.43 MHz** | yes |

The RVC decompressor lands on the critical path and costs **40% of the
clock**, plus 16% more logic. Netting it out for an interpreter:

| | `Full` | `IMAC` |
|---|---:|---:|
| clock | 96.55 MHz | 57.43 MHz |
| emulator `.text` | 159,361 B | 119,917 B |
| relative throughput | **1.68×** | 1.0× |

Smaller code improves I-cache hit rate, but would have to be worth 68% to
break even. For a throughput-bound interpreter it will not be — so the
Makefile defaults to `rv32im`. Override with `make ARCH=rv32imc` if your
target has cheap compressed-instruction support.

Other variants measured, for sizing: `Min` 1,656 LUT4 @ 108 MHz (no
mul/div — the emulator needs multiply, so this is not usable), `Lite`
3,000 @ 102 MHz (also no mul/div), `Debug` 3,595 @ 99.06 MHz,
`FullDebug` 3,952 @ 84.79 MHz (adds JTAG/GDB), `Linux` 5,085 @ 74.44 MHz
(adds an MMU, which buys nothing here since tiny386 keeps its own
software TLB).

All variants provide the `mcycle` CSR, so the `rdcycle`-based `RDTSC`
works on any of them — but note **`cycle` is optional in RISC-V**. A core
without it will read zero or trap.

## Not covered by this test

- **`rdcycle` is unexercised** — the `RDTSC` path compiles but the
  payload never executes `RDTSC`.
- **Interrupts are untouched** — `pic_read_irq` returns −1 and nothing
  raises an IRQ.
- **Real mode and segmentation** — the payload uses
  `cpui386_reset_pm()`, i.e. flat 32-bit with all segment bases zero.
