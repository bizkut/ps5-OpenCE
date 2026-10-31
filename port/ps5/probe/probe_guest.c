/* The M0 probe's guest: x32 code (64-bit instructions, 32-bit pointers)
linked at PROBE_IMAGE_BASE, as the game's guest image will be
(docs/ps5_port_plan.md, D1). The host (probe.c) loads it, fills its import
table and calls the entries of its header. No C library: the host is its
only service. */

#include "probe_abi.h"

/* probe_guest_stubs.S: each jumps through its import slot */
int64_t host_sum(int32_t a, int32_t *pointer, float single, int64_t q);
uint64_t host_ticks(void);
void host_nop(void);
void host_report(uint32_t seed, uint64_t first);
uint64_t host_upper_bits(const void *pointer);
int32_t host_dereference(const int32_t *pointer);
extern uint64_t probe_imports[PROBE_IMPORT_COUNT];

static uint64_t entry_sum(uint64_t argument)
{
	volatile int32_t local = 7;
	int32_t other = 1000;
	(void)argument;
	/* -3 + 7 + (int64_t)(2.5f * 2) + 2^40, and 1000 read back by the host */
	return (uint64_t)host_sum(-3, (int32_t *)&local, 2.5f, 1ll << 40) + (uint64_t)host_dereference(&other);
}

static uint64_t entry_upper_bits(uint64_t argument)
{
	volatile int32_t local = 0;
	(void)argument;
	return host_upper_bits((const void *)&local);
}

static uint64_t entry_call_rate(uint64_t argument)
{
	uint64_t start = host_ticks();
	uint64_t call;

	for (call = 0; call < argument; call++)
		host_nop();
	return host_ticks() - start;
}

static uint64_t entry_spin(uint64_t argument)
{
	uint32_t seconds = (uint32_t)(argument >> 32);
	uint32_t seed = (uint32_t)argument;
	uint64_t end = host_ticks() + (uint64_t)seconds * 1000000000ull;
	uint64_t first = probe_work(seed, PROBE_WORK_ROUNDS);
	uint32_t rounds = 1, mismatches = 0;

	host_report(seed, first);
	while (host_ticks() < end)
	{
		if (probe_work(seed, PROBE_WORK_ROUNDS) != first)
			mismatches++;
		rounds++;
	}
	return (uint64_t)rounds << 32 | mismatches;
}

static uint64_t entry_write(uint64_t argument)
{
	volatile uint32_t *address = (volatile uint32_t *)(uintptr_t)(uint32_t)(argument >> 32);

	*address = (uint32_t)argument;
	return *address;
}

static uint64_t entry_mxcsr(uint64_t argument)
{
	uint32_t mxcsr;

	(void)argument;
	__asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
	return mxcsr;
}

static uint64_t entry_stack_address(uint64_t argument)
{
	volatile uint32_t local = (uint32_t)argument;
	volatile uintptr_t address = (uintptr_t)&local;

	return (uint32_t)address;
}

__attribute__((section(".probe_header"), used))
const struct probe_guest_header probe_guest_header =
{
	PROBE_GUEST_MAGIC,
	PROBE_GUEST_VERSION,
	(uint32_t)(uintptr_t)probe_imports,
	PROBE_IMPORT_COUNT,
	(uint32_t)(uintptr_t)entry_sum,
	(uint32_t)(uintptr_t)entry_upper_bits,
	(uint32_t)(uintptr_t)entry_call_rate,
	(uint32_t)(uintptr_t)entry_spin,
	(uint32_t)(uintptr_t)entry_write,
	(uint32_t)(uintptr_t)entry_mxcsr,
	(uint32_t)(uintptr_t)entry_stack_address,
};
