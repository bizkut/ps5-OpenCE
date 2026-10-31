/* What the M0 probe's host (probe.c, x86-64) and guest (probe_guest.c, x32)
agree on: the addresses of docs/ps5_port_plan.md (D4), the guest image's
header and the host functions the guest imports. Both compilers read this
file: fixed-width types only. */

#ifndef OPENCE_PS5_PROBE_ABI_H
#define OPENCE_PS5_PROBE_ABI_H

#include <stdint.h>

/* the guest image, then the Xbox contiguous memory (port/linux/src/platform.h)
right above it: one reservation. Android links its image above the window
(port/android/include/halo_android_abi.h); x32 code cannot. It follows the
x86-64 small code model, in which every symbol lies below 2 GB: an indexed
access to a global is an addr32 instruction with a sign-extended 32-bit
displacement (R_X86_64_32S), which ld.lld refuses for 0x88000000. */
#define PROBE_IMAGE_BASE 0x7c000000u
#define PROBE_IMAGE_RESERVE 0x04000000u
#define PROBE_WINDOW_BASE 0x80000000u
#define PROBE_WINDOW_SIZE 0x08000000u
/* how much of the image's room the probe backs with memory */
#define PROBE_IMAGE_COMMIT 0x00400000u

/* The window's lowest address the game needs at a fixed place: the tag
cache, which cache files are linked to (source/cache/physical_memory_map.c);
the game state is above it (port/linux/include/halo_port_capacity.h), and
everything else the window hands out goes anywhere, top-down
(port/linux/src/xbox_memory.c). The console's loader maps the title's
sce_module/libc.prx at the window's base before main (run1); the window
starts above it, as the Xbox's own kernel kept the lowest physical pages. */
#define PROBE_WINDOW_FIXED_LOW 0x803A6000u
/* the Linux build's stand-in for that libc.prx (PROBE_SIMULATE_LOADER=1):
the size run1 measured */
#define PROBE_LOADER_STAND_IN 0x00118000u

/* the guest pools' candidate ranges, below and above the fixed addresses */
#define PROBE_POOL_LOW_BASE 0x10000000u
#define PROBE_POOL_LOW_END PROBE_IMAGE_BASE
#define PROBE_POOL_HIGH_BASE (PROBE_WINDOW_BASE + PROBE_WINDOW_SIZE)
#define PROBE_POOL_HIGH_END 0x100000000ull

#define PROBE_GUEST_MAGIC 0x45434f50u /* 'POCE' */
#define PROBE_GUEST_VERSION 1

/* at PROBE_IMAGE_BASE; every entry is uint64_t entry(uint64_t argument) */
struct probe_guest_header
{
	uint32_t magic;
	uint32_t version;
	uint32_t imports; /* the import table: PROBE_IMPORT_COUNT 8-byte slots */
	uint32_t import_count;
	uint32_t sum;        /* the arguments of each kind through host_sum */
	uint32_t upper_bits; /* the upper half of a pointer argument's register */
	uint32_t call_rate;  /* argument: calls of host_nop; result: their nanoseconds */
	uint32_t spin;       /* argument: seconds << 32 | seed; result: rounds << 32 | mismatches */
	uint32_t write;      /* argument: address << 32 | value; result: the value read back */
	uint32_t mxcsr;      /* result: MXCSR */
	uint32_t stack_address; /* result: the address of a local variable */
};

/* the import table's slots */
#define PROBE_IMPORT_SUM 0        /* int64_t (int32_t, uint32_t guest pointer, float, int64_t) */
#define PROBE_IMPORT_TICKS 1      /* uint64_t (void): nanoseconds */
#define PROBE_IMPORT_NOP 2        /* void (void) */
#define PROBE_IMPORT_REPORT 3     /* void (uint32_t seed, uint64_t first result of probe_work) */
#define PROBE_IMPORT_UPPER_BITS 4 /* uint64_t (uint64_t register): its upper 32 bits */
#define PROBE_IMPORT_DEREFERENCE 5 /* int32_t (const int32_t *): through a zero-extending stub */
#define PROBE_IMPORT_COUNT 6

/* probe_work's rounds for one result (probe_work.c) */
#define PROBE_WORK_ROUNDS 200000u

uint64_t probe_work(uint32_t seed, uint32_t rounds);

#endif
