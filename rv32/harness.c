/* Smoke test for the bare-metal RV32 build: run the i386 core on RISC-V
 * and verify it executes. The CPU_CB stubs print only; a real
 * integration would route them at devices. */
#include <stdio.h>
#include <string.h>
#include "i386.h"
#include "test386.h"

#define PHYS_MEM_SIZE (1u << 20)      /* 1 MB is plenty for this test */
#define LOAD_ADDR     0x1000

static char phys_mem[PHYS_MEM_SIZE];
static int  io_writes;
static unsigned last_io;

/* ---- CPU_CB stubs ------------------------------------------------- */
static int  pic_read_irq (void *o)                  { (void)o; return -1; }
static u8   io_read8     (void *o, int p)           { (void)o; printf("   IN8   port=0x%03x -> 0\n", p); return 0; }
static u16  io_read16    (void *o, int p)           { (void)o; printf("   IN16  port=0x%03x -> 0\n", p); return 0; }
static u32  io_read32    (void *o, int p)           { (void)o; printf("   IN32  port=0x%03x -> 0\n", p); return 0; }
static void io_write8    (void *o, int p, u8  v)    { (void)o; io_writes++; last_io=v; printf("   OUT8  port=0x%03x <= 0x%02x\n",  p, v); }
static void io_write16   (void *o, int p, u16 v)    { (void)o; io_writes++; last_io=v; printf("   OUT16 port=0x%03x <= 0x%04x\n",  p, v); }
static void io_write32   (void *o, int p, u32 v)    { (void)o; io_writes++; last_io=v; printf("   OUT32 port=0x%03x <= 0x%08lx\n", p, (unsigned long)v); }
static int  io_read_str  (void *o, int p, uint8_t *b, int sz, int n) { (void)o;(void)p;(void)b;(void)sz; return n; }
static int  io_write_str (void *o, int p, uint8_t *b, int sz, int n) { (void)o;(void)p;(void)b;(void)sz; return n; }
static u8   iomem_read8  (void *o, uword a)         { (void)o; printf("   MMIO r8  0x%08lx\n", (unsigned long)a); return 0; }
static u16  iomem_read16 (void *o, uword a)         { (void)o; printf("   MMIO r16 0x%08lx\n", (unsigned long)a); return 0; }
static u32  iomem_read32 (void *o, uword a)         { (void)o; printf("   MMIO r32 0x%08lx\n", (unsigned long)a); return 0; }
static void iomem_write8 (void *o, uword a, u8  v)  { (void)o; printf("   MMIO w8  0x%08lx <= 0x%02x\n", (unsigned long)a, v); }
static void iomem_write16(void *o, uword a, u16 v)  { (void)o; printf("   MMIO w16 0x%08lx <= 0x%04x\n", (unsigned long)a, v); }
static void iomem_write32(void *o, uword a, u32 v)  { (void)o; printf("   MMIO w32 0x%08lx <= 0x%08lx\n", (unsigned long)a, (unsigned long)v); }
static bool iomem_write_str(void *o, uword a, uint8_t *b, int n) { (void)o;(void)a;(void)b;(void)n; return true; }

int main(void)
{
    CPU_CB *cb;
    CPUI386 *cpu;
    long c0, c1;

    printf("tiny386 on RV32 -- milestone test\n");
    printf("phys_mem = %u bytes, payload %u bytes @ 0x%x\n\n",
           (unsigned)PHYS_MEM_SIZE, (unsigned)sizeof(test386), LOAD_ADDR);

    cpu = cpui386_new(3, phys_mem, PHYS_MEM_SIZE, &cb);
    if (!cpu) { printf("FAIL: cpui386_new returned NULL\n"); return 1; }

    cb->pic = 0;      cb->pic_read_irq = pic_read_irq;
    cb->io = 0;
    cb->io_read8  = io_read8;  cb->io_write8  = io_write8;
    cb->io_read16 = io_read16; cb->io_write16 = io_write16;
    cb->io_read32 = io_read32; cb->io_write32 = io_write32;
    cb->io_read_string = io_read_str; cb->io_write_string = io_write_str;
    cb->iomem = 0;
    cb->iomem_read8  = iomem_read8;  cb->iomem_write8  = iomem_write8;
    cb->iomem_read16 = iomem_read16; cb->iomem_write16 = iomem_write16;
    cb->iomem_read32 = iomem_read32; cb->iomem_write32 = iomem_write32;
    cb->iomem_write_string = iomem_write_str;

    memcpy(phys_mem + LOAD_ADDR, test386, sizeof(test386));
    cpui386_reset_pm(cpu, LOAD_ADDR);

    c0 = cpui386_get_cycle(cpu);
    cpui386_step(cpu, 4000);
    c1 = cpui386_get_cycle(cpu);

    printf("\ncycles executed : %ld\n", c1 - c0);
    printf("io writes       : %d\n", io_writes);
    printf("last io value   : 0x%08lx\n", (unsigned long)last_io);

    if (c1 == c0)            { printf("RESULT: FAIL -- no instructions executed\n"); return 1; }
    if (io_writes != 8)      { printf("RESULT: FAIL -- expected 8 OUTs (5 loop + 3), got %d\n", io_writes); return 1; }
    if (last_io != 0xDEADBEEFu) { printf("RESULT: FAIL -- end marker wrong\n"); return 1; }
    printf("RESULT: PASS\n");
    return 0;
}
