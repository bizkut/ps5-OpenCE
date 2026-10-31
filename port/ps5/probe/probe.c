/* OpenCE's M0 probe (docs/ps5_port_plan.md, "M0. Feasibility probes on the
console"): one run answers S1 to S4 for the PS5, and the same program built
for Linux checks the probe itself.

  S1  the fixed addresses: [0x7c000000, 0x88000000), the guest image's room
      and the Xbox memory, reserved and backed, and pools of 512 MiB or more
      elsewhere below 4 GB;
  S2  x32 code loaded at 0x7c000000 runs: it calls host functions with each
      kind of argument and computes what x86-64 code computes;
  S3  guest threads on stacks below 4 GB run x32 code for a minute;
  S4  a write to a read-only page of the window, from the host and from the
      guest, reaches a handler that unprotects the page and resumes.

Every line goes to standard output (the console's klog) and to
opence_probe.txt in the title's folder (on Linux, the current folder). A
check that fails says so and the probe goes on; the last line counts the
failures. */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "probe_abi.h"

#ifdef __PROSPERO__
#include <ps5platform/kernel.h>
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
#define LOG_PATH "/app0/opence_probe.txt"
#define GUEST_PATH "/app0/probe_guest.elf"
#else
#define LOG_PATH "opence_probe.txt"
#define GUEST_PATH "probe_guest.elf"
#endif

#define MIB 0x100000ull
/* the console's page (16 KiB); a multiple of Linux's */
#define PAGE 0x4000ull
/* the console's direct memory comes in 64 KiB units */
#define DIRECT_UNIT 0x10000ull
#define FOUR_GB 0x100000000ull

#define THREADS 8
#define GUEST_STACK_SIZE MIB
#define POOL_COMMIT (64 * MIB)
#define POOL_WANTED (512 * MIB)

uint64_t probe_call_guest(uint64_t (*entry)(uint64_t), uint64_t argument, void *stack_top);

typedef uint64_t (*guest_entry)(uint64_t);

static FILE *log_file;
static int failures;

static void say(const char *format, ...)
{
	char line[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof line, format, arguments);
	va_end(arguments);
	printf("opence probe: %s\n", line);
	fflush(stdout);
	if (log_file)
	{
		fprintf(log_file, "%s\n", line);
		fflush(log_file);
	}
}

static void verdict(const char *probe, int passed, const char *what)
{
	say("%s %s: %s", probe, passed ? "PASS" : "FAIL", what);
	if (!passed)
		failures++;
}

static uint64_t ticks(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

/* ---------- memory: the console's calls, or Linux's */

enum { PROTECT_READ = 1, PROTECT_WRITE = 2, PROTECT_EXECUTE = 4 };

/* the mapping that holds `address` (reserved addresses too): 1 with its
bounds and the platform's protection bits, or 0 */
static int mapping_at(uint64_t address, uint64_t *start, uint64_t *end, uint32_t *protection)
{
#ifdef __PROSPERO__
	void *first = NULL, *last = NULL;
	uint32_t bits = 0;

	if (sceKernelQueryMemoryProtection((void *)(uintptr_t)address, &first, &last, &bits) != 0
		|| (uint64_t)(uintptr_t)first > address || (uint64_t)(uintptr_t)last <= address)
		return 0;
	*start = (uint64_t)(uintptr_t)first;
	*end = (uint64_t)(uintptr_t)last;
	*protection = bits;
	return 1;
#else
	FILE *maps = fopen("/proc/self/maps", "r");
	char line[512];
	int found = 0;

	while (!found && maps && fgets(line, sizeof line, maps))
	{
		unsigned long long first, last;
		char permissions[8];

		if (sscanf(line, "%llx-%llx %7s", &first, &last, permissions) == 3 && first <= address && address < last)
		{
			*start = first;
			*end = last;
			*protection = (permissions[0] == 'r' ? PROTECT_READ : 0) | (permissions[1] == 'w' ? PROTECT_WRITE : 0)
				| (permissions[2] == 'x' ? PROTECT_EXECUTE : 0);
			found = 1;
		}
	}
	if (maps)
		fclose(maps);
	return found;
#endif
}

/* 1 if nothing, not even a reservation, is mapped in [address, address + size) */
static int range_free(uint64_t address, uint64_t size)
{
#ifdef __PROSPERO__
	uint64_t page, start, end;
	uint32_t protection;

	for (page = address & ~(PAGE - 1); page < address + size; page += PAGE)
	{
		if (mapping_at(page, &start, &end, &protection))
			return 0;
	}
	return 1;
#else
	FILE *maps = fopen("/proc/self/maps", "r");
	char line[512];
	int free = 1;

	while (free && maps && fgets(line, sizeof line, maps))
	{
		unsigned long long first, last;

		if (sscanf(line, "%llx-%llx", &first, &last) == 2 && first < address + size && last > address)
			free = 0;
	}
	if (maps)
		fclose(maps);
	return free;
#endif
}

/* the lowest address at or above `address` that no mapping holds, rounded
up to the direct-memory unit */
static uint64_t first_unmapped(uint64_t address)
{
	uint64_t start, end;
	uint32_t protection;

	while (mapping_at(address, &start, &end, &protection))
		address = end;
	return (address + DIRECT_UNIT - 1) & ~(DIRECT_UNIT - 1);
}

/* reserve() found something mapped in the range */
#define RESERVE_OCCUPIED (-2)

/* the addresses only, no memory: 0, RESERVE_OCCUPIED, or the error. The
console's fixed reservation replaces whatever is mapped there, as run1's did
to the title's libc.prx (port/ps5/FINDINGS.md), so the range is checked
first, on Linux too (where MAP_FIXED_NOREPLACE would also refuse). */
static int reserve(uint64_t address, uint64_t size)
{
	void *at = (void *)(uintptr_t)address;

	if (!range_free(address, size))
		return RESERVE_OCCUPIED;
#ifdef __PROSPERO__
	{
		int result = sceKernelReserveVirtualRange(&at, size, PS5_KERNEL_MAP_FIXED, 0);

		if (result)
			return result;
	}
	if (at != (void *)(uintptr_t)address)
	{
		sceKernelMunmap(at, size);
		return -1;
	}
#else
	at = mmap(at, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
	if (at == MAP_FAILED)
		return errno == EEXIST ? RESERVE_OCCUPIED : errno;
	if (at != (void *)(uintptr_t)address)
	{
		munmap(at, size);
		return -1;
	}
#endif
	return 0;
}

/* readable and writable memory at reserved addresses; 0 or the error */
static int commit(uint64_t address, uint64_t size)
{
#ifdef __PROSPERO__
	int64_t physical = 0;
	void *at = (void *)(uintptr_t)address;
	int result = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size,
		PS5_KERNEL_DIRECT_ALIGNMENT, PS5_KERNEL_DIRECT_TYPE_CPU, &physical);

	if (result)
		return result;
	result = sceKernelMapDirectMemory(&at, size, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
		PS5_KERNEL_MAP_FIXED, physical, 0);
	if (result == 0 && at != (void *)(uintptr_t)address)
		result = -1;
	if (result)
		sceKernelReleaseDirectMemory(physical, size);
	return result;
#else
	return mprotect((void *)(uintptr_t)address, size, PROT_READ | PROT_WRITE) ? errno : 0;
#endif
}


static int protect(uint64_t address, uint64_t size, int protection)
{
#ifdef __PROSPERO__
	return sceKernelMprotect((void *)(uintptr_t)address, size,
		(protection & PROTECT_READ ? PS5_KERNEL_PROT_CPU_READ : 0)
		| (protection & PROTECT_WRITE ? PS5_KERNEL_PROT_CPU_WRITE : 0)
		| (protection & PROTECT_EXECUTE ? PS5_KERNEL_PROT_CPU_EXEC : 0));
#else
	return mprotect((void *)(uintptr_t)address, size,
		(protection & PROTECT_READ ? PROT_READ : 0)
		| (protection & PROTECT_WRITE ? PROT_WRITE : 0)
		| (protection & PROTECT_EXECUTE ? PROT_EXEC : 0)) ? errno : 0;
#endif
}

/* writes every page; 1 if each reads back */
static int touch(uint64_t address, uint64_t size)
{
	volatile uint8_t *bytes = (volatile uint8_t *)(uintptr_t)address;
	uint64_t offset;

	for (offset = 0; offset < size; offset += PAGE)
		bytes[offset] = (uint8_t)(offset >> 14 | 1);
	for (offset = 0; offset < size; offset += PAGE)
		if (bytes[offset] != (uint8_t)(offset >> 14 | 1))
			return 0;
	return 1;
}

/* ---------- S1: the fixed addresses and the pools */

/* the mappings below 4 GB, each once */
static void survey_low_addresses(const char *when)
{
#ifdef __PROSPERO__
	uint64_t address = 0;
	int mappings = 0;

	while (address < FOUR_GB)
	{
		void *start = NULL, *end = NULL;
		uint32_t protection = 0;

		if (sceKernelQueryMemoryProtection((void *)(uintptr_t)address, &start, &end, &protection) == 0
			&& (uint64_t)(uintptr_t)end > address)
		{
			say("S1 %s: mapped [%#llx, %#llx) protection %#x", when, (unsigned long long)(uintptr_t)start,
				(unsigned long long)(uintptr_t)end, protection);
			mappings++;
			address = ((uint64_t)(uintptr_t)end + PAGE - 1) & ~(PAGE - 1);
		}
		else
			address += 0x10000;
	}
	say("S1 %s: %d mappings below 4 GB", when, mappings);
#else
	FILE *maps = fopen("/proc/self/maps", "r");
	char line[512];
	int mappings = 0;

	while (maps && fgets(line, sizeof line, maps))
	{
		unsigned long long start, end;

		if (sscanf(line, "%llx-%llx", &start, &end) == 2 && start < FOUR_GB)
		{
			line[strcspn(line, "\n")] = 0;
			say("S1 %s: %s", when, line);
			mappings++;
		}
	}
	if (maps)
		fclose(maps);
	say("S1 %s: %d mappings below 4 GB", when, mappings);
#endif
}

/* the largest reservation from `base` towards `end` in halves down to
16 MiB; its size, or 0 */
static uint64_t reserve_largest(uint64_t base, uint64_t end)
{
	uint64_t size = end - base;

	while (size >= 16 * MIB)
	{
		int result = reserve(base, size);

		if (result == 0)
			return size;
		say("S1 reserve [%#llx, %#llx): %#x", (unsigned long long)base, (unsigned long long)(base + size),
			(unsigned)result);
		size /= 2;
	}
	return 0;
}

static uint64_t pool_low_size, pool_high_size;
/* the window's usable part is [window_floor, its end): the loader's pages
lie below window_floor */
static uint64_t window_floor;
static int window_ready;

static void s1(void)
{
	const uint64_t window_end = PROBE_WINDOW_BASE + PROBE_WINDOW_SIZE;
	uint64_t start, loader_start = 0, loader_end = 0, after_start = 0, after_end = 0;
	uint32_t loader_protection = 0, after_protection = 0;
	int result, window_ok = 0, image_ok = 0, pools_ok, loader_found;

#ifndef __PROSPERO__
	/* PROBE_SIMULATE_LOADER=1: a stand-in for the console's libc.prx at the
	window's base */
	if (getenv("PROBE_SIMULATE_LOADER") && atoi(getenv("PROBE_SIMULATE_LOADER")) == 1
		&& mmap((void *)(uintptr_t)PROBE_WINDOW_BASE, PROBE_LOADER_STAND_IN, PROT_READ | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != (void *)(uintptr_t)PROBE_WINDOW_BASE)
		say("S1 the loader's stand-in at %#x: %s", PROBE_WINDOW_BASE, strerror(errno));
#endif

#ifdef __PROSPERO__
	{
		size_t flexible = 0, configured = 0, available = 0;
		int64_t direct_total = sceKernelGetDirectMemorySize(), direct_start = 0;

		sceKernelAvailableFlexibleMemorySize(&flexible);
		sceKernelConfiguredFlexibleMemorySize(&configured);
		sceKernelAvailableDirectMemorySize(0, direct_total, PS5_KERNEL_DIRECT_ALIGNMENT, &direct_start, &available);
		say("S1 memory: flexible %zu of %zu MiB free; direct %lld MiB, largest free %zu MiB", flexible / MIB,
			configured / MIB, (long long)(direct_total / (int64_t)MIB), available / MIB);
	}
#endif
	{
		int local;
		void *heap = malloc(64);

		say("S1 the host: code %p, stack %p, heap %p", (void *)s1, (void *)&local, heap);
		free(heap);
	}
	survey_low_addresses("before");

	result = reserve(PROBE_IMAGE_BASE, PROBE_IMAGE_RESERVE);
	if (result == 0)
	{
		result = commit(PROBE_IMAGE_BASE, PROBE_IMAGE_COMMIT);
		image_ok = result == 0 && touch(PROBE_IMAGE_BASE, PROBE_IMAGE_COMMIT);
	}
	say("S1 reserve the image's room [%#x, %#x) and back its first %u MiB: %#x", PROBE_IMAGE_BASE,
		PROBE_IMAGE_BASE + PROBE_IMAGE_RESERVE, PROBE_IMAGE_COMMIT / (unsigned)MIB, (unsigned)result);
	verdict("S1", image_ok, "the image's room reserved at its fixed address, every backed page usable");

	loader_found = mapping_at(PROBE_WINDOW_BASE, &loader_start, &loader_end, &loader_protection);
	window_floor = first_unmapped(PROBE_WINDOW_BASE);
	if (loader_found)
		say("S1 the window's base is mapped already: [%#llx, %#llx) protection %#x; free from %#llx",
			(unsigned long long)loader_start, (unsigned long long)loader_end, loader_protection,
			(unsigned long long)window_floor);
	verdict("S1", window_floor <= PROBE_WINDOW_FIXED_LOW,
		"the window is free from below the tag cache's fixed address (0x803a6000) up");
	result = window_floor <= PROBE_WINDOW_FIXED_LOW ? reserve(window_floor, window_end - window_floor) : RESERVE_OCCUPIED;
	start = ticks();
	if (result == 0)
	{
		result = commit(window_floor, window_end - window_floor);
		window_ok = result == 0 && touch(window_floor, window_end - window_floor);
	}
	window_ready = window_ok;
	say("S1 reserve and back [%#llx, %#llx), %llu KiB: %#x, every page written and read back: %s, %.1f ms",
		(unsigned long long)window_floor, (unsigned long long)window_end,
		(unsigned long long)((window_end - window_floor) >> 10), (unsigned)result, window_ok ? "yes" : "no",
		(ticks() - start) / 1e6);
	verdict("S1", window_ok, "the window above the loader's pages reserved and backed, every page usable");
	if (loader_found)
	{
		int kept = mapping_at(PROBE_WINDOW_BASE, &after_start, &after_end, &after_protection)
			&& after_start == loader_start && after_end == loader_end && after_protection == loader_protection;

		verdict("S1", kept, "the loader's mapping at the window's base left as it was");
	}

	pool_low_size = reserve_largest(PROBE_POOL_LOW_BASE, PROBE_POOL_LOW_END);
	pool_high_size = reserve_largest(PROBE_POOL_HIGH_BASE, PROBE_POOL_HIGH_END);
	say("S1 pools: %llu MiB at %#x, %llu MiB at %#x", (unsigned long long)(pool_low_size / MIB), PROBE_POOL_LOW_BASE,
		(unsigned long long)(pool_high_size / MIB), PROBE_POOL_HIGH_BASE);
	pools_ok = pool_low_size + pool_high_size >= POOL_WANTED;
	if (pool_low_size >= POOL_COMMIT)
	{
		result = commit(PROBE_POOL_LOW_BASE, POOL_COMMIT);
		pools_ok = pools_ok && result == 0 && touch(PROBE_POOL_LOW_BASE, POOL_COMMIT);
		say("S1 commit 64 MiB of the low pool: %#x", (unsigned)result);
	}
	else
		pools_ok = 0;
	verdict("S1", pools_ok, "512 MiB or more of pools below 4 GB, the first 64 MiB backed");
	survey_low_addresses("after");
}

/* ---------- S2: the guest image */

struct elf32_header
{
	unsigned char ident[16];
	uint16_t type, machine;
	uint32_t version, entry, program_headers, section_headers, flags;
	uint16_t header_size, program_header_size, program_header_count;
	uint16_t section_header_size, section_header_count, section_names;
};

struct elf32_program_header
{
	uint32_t type, offset, address, physical, file_size, memory_size, flags, alignment;
};

enum { ELF_PT_LOAD = 1, ELF_PF_X = 1, ELF_EM_X86_64 = 62, ELF_ET_EXEC = 2 };

static uint64_t reported[THREADS + 1];
static volatile int reported_count;

static int64_t host_sum(int32_t a, uint32_t pointer, float single, int64_t q)
{
	return a + *(const int32_t *)(uintptr_t)pointer + (int64_t)(single * 2) + q;
}

static uint64_t host_ticks(void)
{
	return ticks();
}

static void host_nop(void)
{
	__asm__ volatile("");
}

static void host_report(uint32_t seed, uint64_t first)
{
	if (seed <= THREADS)
	{
		reported[seed] = first;
		__sync_fetch_and_add(&reported_count, 1);
	}
}

static uint64_t host_upper_bits(uint64_t value)
{
	return value >> 32;
}

static int32_t host_dereference(const int32_t *pointer)
{
	return *pointer;
}

static struct probe_guest_header *guest;

/* loads probe_guest.elf into the committed image room, fills its imports
and makes its code executable; the header, or NULL */
static struct probe_guest_header *load_guest(void)
{
	FILE *file = fopen(GUEST_PATH, "rb");
	unsigned char *bytes = NULL;
	long size = 0;
	struct elf32_header header;
	struct probe_guest_header *loaded = NULL;
	uint64_t *imports;
	int index;

	if (!file)
	{
		say("S2 %s: %s", GUEST_PATH, strerror(errno));
		return NULL;
	}
	if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) > (long)sizeof header && fseek(file, 0, SEEK_SET) == 0)
	{
		bytes = malloc((size_t)size);
		if (bytes && fread(bytes, 1, (size_t)size, file) != (size_t)size)
		{
			free(bytes);
			bytes = NULL;
		}
	}
	fclose(file);
	if (!bytes)
	{
		say("S2 %s: unreadable", GUEST_PATH);
		return NULL;
	}
	memcpy(&header, bytes, sizeof header);
	if (memcmp(header.ident, "\177ELF\001\001", 6) != 0 || header.machine != ELF_EM_X86_64
		|| header.type != ELF_ET_EXEC || header.program_header_size != sizeof(struct elf32_program_header)
		|| header.program_headers + (uint64_t)header.program_header_count * header.program_header_size > (uint64_t)size)
	{
		say("S2 %s: not an x32 executable", GUEST_PATH);
		goto done;
	}
	for (index = 0; index < header.program_header_count; index++)
	{
		struct elf32_program_header segment;

		memcpy(&segment, bytes + header.program_headers + index * sizeof segment, sizeof segment);
		if (segment.type != ELF_PT_LOAD)
			continue;
		if (segment.address < PROBE_IMAGE_BASE || segment.file_size > segment.memory_size
			|| (uint64_t)segment.address + segment.memory_size > PROBE_IMAGE_BASE + PROBE_IMAGE_COMMIT
			|| (uint64_t)segment.offset + segment.file_size > (uint64_t)size)
		{
			say("S2 segment %d [%#x, +%#x) is outside the image room", index, segment.address, segment.memory_size);
			goto done;
		}
		memcpy((void *)(uintptr_t)segment.address, bytes + segment.offset, segment.file_size);
		memset((void *)(uintptr_t)(segment.address + segment.file_size), 0, segment.memory_size - segment.file_size);
	}
	loaded = (struct probe_guest_header *)(uintptr_t)PROBE_IMAGE_BASE;
	if (loaded->magic != PROBE_GUEST_MAGIC || loaded->version != PROBE_GUEST_VERSION
		|| loaded->import_count != PROBE_IMPORT_COUNT)
	{
		say("S2 the image's header is not the probe's");
		loaded = NULL;
		goto done;
	}
	imports = (uint64_t *)(uintptr_t)loaded->imports;
	imports[PROBE_IMPORT_SUM] = (uint64_t)(uintptr_t)host_sum;
	imports[PROBE_IMPORT_TICKS] = (uint64_t)(uintptr_t)host_ticks;
	imports[PROBE_IMPORT_NOP] = (uint64_t)(uintptr_t)host_nop;
	imports[PROBE_IMPORT_REPORT] = (uint64_t)(uintptr_t)host_report;
	imports[PROBE_IMPORT_UPPER_BITS] = (uint64_t)(uintptr_t)host_upper_bits;
	imports[PROBE_IMPORT_DEREFERENCE] = (uint64_t)(uintptr_t)host_dereference;
	for (index = 0; index < header.program_header_count; index++)
	{
		struct elf32_program_header segment;
		uint64_t first, last;
		int result;

		memcpy(&segment, bytes + header.program_headers + index * sizeof segment, sizeof segment);
		if (segment.type != ELF_PT_LOAD || !(segment.flags & ELF_PF_X))
			continue;
		first = segment.address & ~(PAGE - 1);
		last = (segment.address + segment.memory_size + PAGE - 1) & ~(PAGE - 1);
		result = protect(first, last - first, PROTECT_READ | PROTECT_EXECUTE);
		say("S2 code [%#llx, %#llx) read and execute: %#x", (unsigned long long)first, (unsigned long long)last,
			(unsigned)result);
		if (result)
			loaded = NULL;
	}
done:
	free(bytes);
	return loaded;
}

static uint8_t *main_guest_stack_top;

static uint64_t call_guest(uint32_t entry, uint64_t argument)
{
	return probe_call_guest((guest_entry)(uintptr_t)entry, argument, main_guest_stack_top);
}

static void s2(void)
{
	uint64_t result, expected, upper, address, nanoseconds, mxcsr;
	uint64_t calls = 10000000;

	if (pool_low_size < POOL_COMMIT)
	{
		verdict("S2", 0, "no guest stack: the low pool is missing (S1)");
		return;
	}
	/* the first megabyte of the low pool is the main thread's guest stack */
	main_guest_stack_top = (uint8_t *)(uintptr_t)(PROBE_POOL_LOW_BASE + GUEST_STACK_SIZE);
	guest = load_guest();
	verdict("S2", guest != NULL, "the x32 image loaded at 0x7c000000 and its code made executable");
	if (!guest)
		return;

	result = call_guest(guest->sum, 0);
	expected = (uint64_t)(-3 + 7 + 5 + (1ll << 40)) + 1000;
	say("S2 sum: %#llx, expected %#llx", (unsigned long long)result, (unsigned long long)expected);
	verdict("S2", result == expected, "x32 code calls the host with int, pointer, float and long long arguments");

	upper = call_guest(guest->upper_bits, 0);
	say("S2 a pointer argument's register upper half, as the x32 caller leaves it: %#llx", (unsigned long long)upper);

	address = call_guest(guest->stack_address, 0);
	say("S2 a guest local at %#llx (stack [%#llx, %#llx))", (unsigned long long)address,
		(unsigned long long)PROBE_POOL_LOW_BASE, (unsigned long long)(uintptr_t)main_guest_stack_top);
	verdict("S2", address >= PROBE_POOL_LOW_BASE && address < (uint64_t)(uintptr_t)main_guest_stack_top,
		"the guest runs on the guest stack below 4 GB");

	mxcsr = call_guest(guest->mxcsr, 0);
	say("S2 MXCSR in the guest: %#llx, its controls %#llx (IEEE defaults, no flush to zero: 0x1f80)",
		(unsigned long long)mxcsr, (unsigned long long)(mxcsr & ~0x3full));

	nanoseconds = call_guest(guest->call_rate, calls);
	say("S2 %llu guest-to-host calls: %.2f ns each", (unsigned long long)calls, (double)nanoseconds / calls);

	reported_count = 0;
	call_guest(guest->spin, 0);
	expected = probe_work(0, PROBE_WORK_ROUNDS);
	say("S2 probe_work: guest %#llx, host %#llx", (unsigned long long)reported[0], (unsigned long long)expected);
	verdict("S2", reported_count == 1 && reported[0] == expected, "x32 and x86-64 arithmetic agree bit for bit");
}

/* ---------- S3: guest threads */

struct guest_thread
{
	pthread_t thread;
	uint32_t seed;
	uint32_t seconds;
	uint8_t *stack;
	uint64_t local_address;
	uint64_t result;
};

static void *guest_thread_main(void *argument)
{
	struct guest_thread *self = argument;
	int local;

	self->local_address = (uint64_t)(uintptr_t)&local;
	/* already on a stack below 4 GB: the guest is called directly */
	self->result = ((guest_entry)(uintptr_t)guest->spin)((uint64_t)self->seconds << 32 | self->seed);
	return NULL;
}

static void s3(void)
{
	struct guest_thread threads[THREADS];
	const char *environment = getenv("PROBE_SPIN_SECONDS");
	uint32_t seconds = environment ? (uint32_t)strtoul(environment, NULL, 10) : 60;
	int index, created = 0, on_stack = 0, agreed = 0;
	uint64_t rounds = 0, mismatches = 0;

	if (!guest)
	{
		verdict("S3", 0, "no guest image (S2)");
		return;
	}
	reported_count = 0;
	memset(threads, 0, sizeof threads);
	for (index = 0; index < THREADS; index++)
	{
		pthread_attr_t attributes;
		int result;

		threads[index].seed = (uint32_t)index + 1;
		threads[index].seconds = seconds;
		/* the pool's second megabyte onwards: one each */
		threads[index].stack = (uint8_t *)(uintptr_t)(PROBE_POOL_LOW_BASE + (uint64_t)(index + 1) * GUEST_STACK_SIZE);
		pthread_attr_init(&attributes);
		result = pthread_attr_setstack(&attributes, threads[index].stack, GUEST_STACK_SIZE);
		if (result == 0)
			result = pthread_create(&threads[index].thread, &attributes, guest_thread_main, &threads[index]);
		pthread_attr_destroy(&attributes);
		if (result)
		{
			say("S3 thread %d: %d", index, result);
			break;
		}
		created++;
	}
	say("S3 %d threads run x32 code for %u s", created, seconds);
	for (index = 0; index < created; index++)
	{
		struct guest_thread *thread = &threads[index];

		pthread_join(thread->thread, NULL);
		if (thread->local_address >= (uint64_t)(uintptr_t)thread->stack
			&& thread->local_address < (uint64_t)(uintptr_t)thread->stack + GUEST_STACK_SIZE)
			on_stack++;
		if (reported[thread->seed] == probe_work(thread->seed, PROBE_WORK_ROUNDS))
			agreed++;
		rounds += thread->result >> 32;
		mismatches += (uint32_t)thread->result;
		say("S3 thread %d: local at %#llx, %llu rounds, %u mismatches", index,
			(unsigned long long)thread->local_address, (unsigned long long)(thread->result >> 32),
			(unsigned)(uint32_t)thread->result);
	}
	verdict("S3", created == THREADS && on_stack == THREADS,
		"8 threads ran on the stacks given them below 4 GB");
	verdict("S3", created == THREADS && agreed == THREADS && mismatches == 0 && rounds >= THREADS,
		"their x32 results matched the host's and each other's for the whole run");
}

/* ---------- S4: write faults */

static volatile uint64_t watched_page;
static volatile int faults;
static volatile uint64_t fault_address;

static void on_fault(int signal_number, siginfo_t *information, void *context)
{
	uint64_t address = (uint64_t)(uintptr_t)information->si_addr;

	(void)context;
	fault_address = address;
	if (watched_page && address >= watched_page && address < watched_page + PAGE)
	{
		faults++;
		protect(watched_page, PAGE, PROTECT_READ | PROTECT_WRITE);
		return; /* the store runs again */
	}
	signal(signal_number, SIG_DFL);
}

static void s4(void)
{
	struct sigaction action;
	uint64_t page = window_floor + MIB;
	volatile uint32_t *word = (volatile uint32_t *)(uintptr_t)page;
	int result, host_ok, guest_ok = 0;

	if (!window_ready)
	{
		verdict("S4", 0, "no window (S1)");
		return;
	}
	memset(&action, 0, sizeof action);
	action.sa_sigaction = on_fault;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);

	watched_page = page;
	result = protect(page, PAGE, PROTECT_READ);
	faults = 0;
	*word = 0x1234;
	say("S4 host write: protect %#x, faults %d at %#llx, value %#x", (unsigned)result, faults,
		(unsigned long long)fault_address, *word);
	host_ok = result == 0 && faults == 1 && *word == 0x1234;
	verdict("S4", host_ok, "a host write to a read-only page of the window is caught and resumed");

	if (guest)
	{
		uint64_t value;

		result = protect(page, PAGE, PROTECT_READ);
		faults = 0;
		value = call_guest(guest->write, page << 32 | 0x5678);
		say("S4 guest write: protect %#x, faults %d at %#llx, value %#llx", (unsigned)result, faults,
			(unsigned long long)fault_address, (unsigned long long)value);
		guest_ok = result == 0 && faults == 1 && value == 0x5678;
	}
	verdict("S4", guest_ok, "a guest write to a read-only page of the window is caught and resumed");
	watched_page = 0;
}

int main(void)
{
	log_file = fopen(LOG_PATH, "w");
	say("OpenCE PS5 M0 probe (docs/ps5_port_plan.md), log %s", LOG_PATH);
	s1();
	s2();
	s3();
	s4();
	say("done: %d failed check(s)", failures);
	if (log_file)
		fclose(log_file);
#ifdef __PROSPERO__
	/* a title leaves through the system's shell: exit() faults in a title
	(PS5_vkQuake's findings) */
	sceSystemServiceLoadExec("exit", NULL);
	for (;;)
		usleep(100000);
#endif
	return failures != 0;
}
