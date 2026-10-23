// Empty function tables for the bootstrap build of the host runtime (port/harness/Makefile, OBJDIR=obj_boot).
// With no lifted code linked in, every guest address misses the table and the dispatcher hands control to the x86
// interpreter (port/work/interp). setup.sh uses this runtime to run the packer up to the entry point and to dump the
// unpacked image that the lifter reads; it is also the "--interp-only" runtime.
#include <stdint.h>
typedef void (*GuestFn0)(void *);
const struct { uint32_t a; GuestFn0 f; } rt_table[1] = {{0, 0}};
const unsigned rt_table_n = 0;
const struct { uint32_t a; GuestFn0 f; } rt_table_x[1] = {{0, 0}};
const unsigned rt_table_x_n = 0;
