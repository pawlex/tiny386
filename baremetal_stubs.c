/* Bare-metal stubs for the RV32 build. */
#include <unistd.h>
/* i386.c calls usleep() on an idle/HLT path. On the FPGA target this
 * should become a WFI or a yield to the peripheral service loop; a
 * no-op is correct for a functional test. */
int usleep(useconds_t usec) { (void)usec; return 0; }
