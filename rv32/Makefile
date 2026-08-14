# Bare-metal RV32 build of the tiny386 i386 core, run under qemu.
#
#   make          - build milestone.elf
#   make run      - build and run under qemu-system-riscv32
#   make payload  - re-assemble test386.asm -> test386.h (needs nasm)
#   make clean
#
# The generated test386.h is committed, so nasm is only needed to CHANGE
# the payload.

CROSS   ?= riscv64-unknown-elf-
CC       = $(CROSS)gcc
SIZE     = $(CROSS)size
QEMU    ?= qemu-system-riscv32

# picolibc supplies the C library; its semihosting crt0 provides
# stdout/stderr/_exit, so printf debugging works under qemu for free.
PICOINC ?= /usr/lib/picolibc/riscv64-unknown-elf/include

# rv32im, not rv32imc: compressed instructions make the emulator 25%
# smaller but cost ~40% of Fmax on ECP5 (see ../PORT-ANALYSIS.md), which
# is the wrong trade for a throughput-bound interpreter.
ARCH    ?= rv32im
ABI     ?= ilp32

# qemu 'virt' RAM starts at 0x80000000. Sized generously -- the harness
# statically allocates a 1 MB phys_mem.
LDSYMS   = -Wl,--defsym=__flash=0x80000000 -Wl,--defsym=__flash_size=0x200000 \
           -Wl,--defsym=__ram=0x80200000  -Wl,--defsym=__ram_size=0x600000

CFLAGS   = -march=$(ARCH) -mabi=$(ABI) -Os -DNDEBUG \
           -DI386_DISABLE_FPU -DTINY386_STATIC_ALLOC \
           --specs=picolibc.specs --crt0=semihost \
           -isystem $(PICOINC) -I.. -I.
LDLIBS   = -Wl,--start-group -lc -lsemihost -Wl,--end-group
SRCS     = harness.c ../i386.c ../baremetal_stubs.c

all: milestone.elf

milestone.elf: $(SRCS) test386.h ../i386.h
	$(CC) $(CFLAGS) $(LDSYMS) -o $@ $(SRCS) $(LDLIBS)
	@$(SIZE) $@

run: milestone.elf
	$(QEMU) -machine virt -bios none -kernel $< -semihosting \
	        -nographic -monitor none -serial none

payload: test386.asm
	nasm -f bin test386.asm -o test386.bin
	{ echo "/* Generated from test386.asm -- do not edit. */"; \
	  echo "static const unsigned char test386[] = {"; \
	  od -An -v -tx1 test386.bin | tr -s ' ' '\n' | grep -v '^$$' \
	    | awk '{printf "0x%s,",$$1; if(NR%12==0) printf "\n"}'; \
	  echo; echo "};"; } > test386.h
	rm -f test386.bin

clean:
	rm -f milestone.elf test386.bin

.PHONY: all run payload clean
