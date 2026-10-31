/* PS5 low-address guest mappings and write tracking. */
#include "host.h"
#include "host_watch_hash.h"
#include <ps5platform/kernel.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 0x10000ULL
#define KPAGE 0x4000ULL
#define LOW_LIMIT 0x100000000ULL
#define WINDOW_BASE 0x80000000ULL
#define WINDOW_FLOOR 0x80120000ULL
#define WINDOW_END 0xa0000000ULL
#define IMAGE_BASE 0x7c000000ULL
#define IMAGE_END 0x80000000ULL
#define POOL_SIZE 0x10000000ULL
#define POOL_PAGES (POOL_SIZE / PAGE)
#define MAXIMUM_POOLS 12
#define GUEST_MAP_FIXED 0x10
#define GUEST_MAP_ANONYMOUS 0x20
#define GUEST_MAP_FIXED_NOREPLACE 0x100000

struct pool
{
	uint64_t base;
	uint32_t free_pages;
	uint8_t used[POOL_PAGES];
	uint8_t mapped[POOL_PAGES];
};
#include "host_pool.h"
static struct pool *pools[MAXIMUM_POOLS];
static int pool_count, fixed_error, fixed_reserved;
static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;
/* Each committed direct-memory unit is independently allocated and released. */
static int64_t physical[LOW_LIMIT / PAGE];
static uint8_t committed[LOW_LIMIT / PAGE];
static uint64_t image_end;
static struct
{
	uint64_t base, end;
} fixed_extra[64];
static uint8_t window_live[(WINDOW_END - WINDOW_FLOOR) / 0x1000];
static int image_live;

static uint64_t up(uint64_t v, uint64_t a)
{
	return (v + a - 1) & ~(a - 1);
}
static int inside(uint64_t a, uint64_t n, uint64_t lo, uint64_t hi)
{
	return a >= lo && a <= hi && n <= hi - a;
}
static int range_free(uint64_t address, uint64_t size)
{
	uint64_t p;
	for (p = address; p < address + size; p += KPAGE)
	{
		void *first = NULL, *last = NULL;
		uint32_t prot = 0;
		if (!sceKernelQueryMemoryProtection((void *)(uintptr_t)p, &first, &last, &prot) && (uintptr_t)first <= p &&
			(uintptr_t)last > p)
			return 0;
	}
	return 1;
}
static int reserve(uint64_t address, uint64_t size)
{
	void *at = (void *)(uintptr_t)address;
	if (!range_free(address, size))
		return -1;
	if (sceKernelReserveVirtualRange(&at, size, PS5_KERNEL_MAP_FIXED, 0))
		return -1;
	if ((uintptr_t)at != address)
	{
		sceKernelMunmap(at, size);
		return -1;
	}
	return 0;
}
/* Partial native pages may gain access but never lose a neighbour's access. */
static int protect_partial(uint64_t page, int wanted)
{
	void *first = NULL, *last = NULL;
	uint32_t current = 0;
	if (sceKernelQueryMemoryProtection((void *)(uintptr_t)page, &first, &last, &current) || (uintptr_t)first > page ||
		(uintptr_t)last <= page)
		return -1;
	wanted |= current;
	if ((uint32_t)wanted == current)
		return 0;
	return sceKernelMprotect((void *)(uintptr_t)page, KPAGE, wanted);
}
static int protect(uint64_t address, uint64_t size, int prot)
{
	uint64_t first = address & ~(KPAGE - 1), end = up(address + size, KPAGE);
	uint64_t whole_first, whole_end;
	int wanted = (prot & PROT_READ ? PS5_KERNEL_PROT_CPU_READ : 0) |
				 (prot & PROT_WRITE ? PS5_KERNEL_PROT_CPU_WRITE : 0) |
				 (prot & PROT_EXEC ? PS5_KERNEL_PROT_CPU_EXEC : 0);
	if (!size || address + size < address || end > LOW_LIMIT)
		return -1;
	if (first < WINDOW_FLOOR && end > WINDOW_BASE)
		return -1;
	whole_first = up(address, KPAGE);
	whole_end = (address + size) & ~(KPAGE - 1);
	if (first < whole_first && protect_partial(first, wanted))
		return -1;
	if (whole_end > whole_first && sceKernelMprotect((void *)(uintptr_t)whole_first, whole_end - whole_first, wanted))
		return -1;
	if (whole_end < address + size && whole_end >= whole_first && protect_partial(whole_end, wanted))
		return -1;
	return 0;
}
static int commit(uint64_t address, uint64_t size)
{
	uint64_t p, end = up(address + size, PAGE);
	for (p = address & ~(PAGE - 1); p < end; p += PAGE)
	{
		uint64_t index = p / PAGE;
		int64_t offset;
		void *at = (void *)(uintptr_t)p;
		int rc;
		if (committed[index])
			continue;
		rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), PAGE, PS5_KERNEL_DIRECT_ALIGNMENT,
										   PS5_KERNEL_DIRECT_TYPE_CPU, &offset);
		if (rc)
		{
			host_logf(HOST_LOG_ERROR, "direct allocation at %llx: %x", (unsigned long long)p, rc);
			return -1;
		}
		rc = sceKernelMapDirectMemory(&at, PAGE, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
									  PS5_KERNEL_MAP_FIXED, offset, 0);
		if (rc || (uintptr_t)at != p)
		{
			sceKernelReleaseDirectMemory(offset, PAGE);
			host_logf(HOST_LOG_ERROR, "direct mapping at %llx: %x", (unsigned long long)p, rc);
			return -1;
		}
		/* Direct memory is not guaranteed to arrive zeroed like anonymous mmap. */
		memset(at, 0, PAGE);
		physical[index] = offset;
		committed[index] = 1;
	}
	return 0;
}
static void release(uint64_t address, uint64_t size)
{
	uint64_t p, end = address + size;
	for (p = address; p < end; p += PAGE)
	{
		uint64_t i = p / PAGE;
		if (!committed[i])
			continue;
		sceKernelMunmap((void *)(uintptr_t)p, PAGE);
		sceKernelReleaseDirectMemory(physical[i], PAGE);
		committed[i] = 0;
		/* Restore the reservation after unmapping the direct backing. */
		if (reserve(p, PAGE))
			host_fatal("cannot restore guest reservation at %llx", (unsigned long long)p);
	}
}
void host_memory_reserve_early(void)
{
	if (fixed_reserved || fixed_error)
		return;
	if (reserve(IMAGE_BASE, IMAGE_END - IMAGE_BASE))
	{
		fixed_error = 1;
		return;
	}
	if (reserve(WINDOW_FLOOR, WINDOW_END - WINDOW_FLOOR))
	{
		sceKernelMunmap((void *)(uintptr_t)IMAGE_BASE, IMAGE_END - IMAGE_BASE);
		fixed_error = 1;
		return;
	}
	fixed_reserved = 1;
	host_logf(HOST_LOG_INFO, "opence: reserved image %llx-%llx window %llx-%llx", (unsigned long long)IMAGE_BASE,
			  (unsigned long long)IMAGE_END, (unsigned long long)WINDOW_FLOOR, (unsigned long long)WINDOW_END);
}
int host_memory_fixed_unavailable(void)
{
	return fixed_error;
}
int host_memory_initialize(uint32_t base, uint32_t size)
{
	host_memory_reserve_early();
	if (fixed_error || base != IMAGE_BASE || size > IMAGE_END - IMAGE_BASE)
		return -1;
	image_end = base + up(size, PAGE);
	return 0;
}
void host_memory_report_low_mappings(FILE *file)
{
	uint64_t p;
	for (p = 0; p < LOW_LIMIT; p += KPAGE)
	{
		void *first = NULL, *last = NULL;
		uint32_t prot = 0;
		if (!sceKernelQueryMemoryProtection((void *)(uintptr_t)p, &first, &last, &prot) && (uintptr_t)first <= p &&
			(uintptr_t)last > p)
		{
			host_logf(HOST_LOG_INFO, "low mapping %p-%p protection %x", first, last, prot);
			if (file)
				fprintf(file, "%p-%p protection %x\n", first, last, prot);
			p = (uintptr_t)last - KPAGE;
		}
	}
}
static struct pool *pool_of(uint64_t address, uint64_t size)
{
	int i;
	for (i = 0; i < pool_count; i++)
		if (inside(address, size, pools[i]->base, pools[i]->base + POOL_SIZE))
			return pools[i];
	return NULL;
}
static struct pool *pool_new(void)
{
	uint64_t p;
	struct pool *pool;
	if (pool_count == MAXIMUM_POOLS)
		return NULL;
	for (p = 0xa0000000ULL; p + POOL_SIZE <= LOW_LIMIT; p += POOL_SIZE)
		if (!reserve(p, POOL_SIZE))
			goto found;
	for (p = 0x10000000ULL; p + POOL_SIZE <= IMAGE_BASE; p += POOL_SIZE)
		if (!reserve(p, POOL_SIZE))
			goto found;
	return NULL;
found:
	pool = calloc(1, sizeof(*pool));
	if (!pool)
	{
		sceKernelMunmap((void *)(uintptr_t)p, POOL_SIZE);
		return NULL;
	}
	pool->base = p;
	pool->free_pages = POOL_PAGES;
	pools[pool_count++] = pool;
	host_logf(HOST_LOG_INFO, "guest memory pool at %llx", (unsigned long long)p);
	return pool;
}
void *host_low_map(size_t size, int protection)
{
	uint64_t pages = up(size, PAGE) / PAGE;
	void *address = NULL;
	int i;
	struct pool *pool;
	if (!pages || pages > POOL_PAGES)
		return NULL;
	pthread_mutex_lock(&memory_lock);
	for (i = 0; i < pool_count && !address; i++)
		address = pool_take(pools[i], pages);
	if (!address && (pool = pool_new()))
		address = pool_take(pool, pages);
	if (address && (commit((uintptr_t)address, pages * PAGE) || protect((uintptr_t)address, pages * PAGE, protection)))
	{
		pool = pool_of((uintptr_t)address, pages * PAGE);
		if (pool)
		{
			uint64_t first = ((uintptr_t)address - pool->base) / PAGE;
			release((uintptr_t)address, pages * PAGE);
			memset(pool->used + first, 0, pages);
			pool->free_pages += pages;
		}
		address = NULL;
	}
	if (address)
	{
		uint64_t first, unit_count = up(size, KPAGE) / KPAGE, index;
		pool = pool_of((uintptr_t)address, pages * PAGE);
		first = ((uintptr_t)address - pool->base) / PAGE;
		for (index = 0; index < pages; index++)
		{
			unsigned units = unit_count > 4 ? 4 : (unsigned)unit_count;
			pool->mapped[first + index] = (1u << units) - 1;
			unit_count -= units;
		}
	}
	pthread_mutex_unlock(&memory_lock);
	return address;
}
void host_low_unmap(void *address, size_t size)
{
	uint64_t start = (uintptr_t)address & ~(KPAGE - 1), end = up((uintptr_t)address + size, KPAGE), p;
	struct pool *pool;
	pthread_mutex_lock(&memory_lock);
	pool = pool_of(start, end - start);
	if (pool)
	{
		for (p = start; p < end; p += KPAGE)
		{
			uint64_t index = (p - pool->base) / PAGE;
			unsigned bit = 1u << ((p - pool->base) % PAGE / KPAGE);
			if (!(pool->mapped[index] & bit))
				continue;
			pool->mapped[index] &= ~bit;
			if (!pool->mapped[index])
			{
				release(pool->base + index * PAGE, PAGE);
				pool->used[index] = 0;
				pool->free_pages++;
			}
		}
	}
	pthread_mutex_unlock(&memory_lock);
}
int host_low_owns(uintptr_t address, size_t size)
{
	int yes, i;
	if (inside(address, size, WINDOW_FLOOR, WINDOW_END) || inside(address, size, IMAGE_BASE, image_end))
		return 1;
	pthread_mutex_lock(&memory_lock);
	yes = pool_of(address, size) != NULL;
	for (i = 0; !yes && i < 64; i++)
		yes = fixed_extra[i].end && inside(address, size, fixed_extra[i].base, fixed_extra[i].end);
	pthread_mutex_unlock(&memory_lock);
	return yes;
}
long host_guest_mmap(uint64_t address, uint64_t size, int prot, int flags, int fd, int64_t offset)
{
	uint64_t length = up(size, KPAGE);
	void *at;
	int newly_reserved = 0;
	if (!size || length < size || length > LOW_LIMIT)
		return -EINVAL;
	/* The guest reserves the logical Xbox window, including loader pages. */
	if (address == WINDOW_BASE && length == WINDOW_END - WINDOW_BASE && prot == PROT_NONE &&
		(flags & GUEST_MAP_FIXED_NOREPLACE))
		return (long)address;
	if (flags & (GUEST_MAP_FIXED | GUEST_MAP_FIXED_NOREPLACE))
	{
		if (address >= LOW_LIMIT || size > LOW_LIMIT - address)
			return -EINVAL;
		if (!host_low_owns(address, size))
		{
			uint64_t first = address & ~(PAGE - 1), end = up(address + size, PAGE);
			int i, failed;
			if (!(flags & GUEST_MAP_FIXED_NOREPLACE) || (first < WINDOW_END && end > WINDOW_BASE))
				return -ENOMEM;
			pthread_mutex_lock(&memory_lock);
			for (i = 0; i < 64 && fixed_extra[i].end; i++)
			{
			}
			failed = i == 64 || reserve(first, end - first);
			if (!failed)
			{
				fixed_extra[i].base = first;
				fixed_extra[i].end = end;
				newly_reserved = 1;
			}
			pthread_mutex_unlock(&memory_lock);
			if (failed)
				return -ENOMEM;
		}
		at = (void *)(uintptr_t)address;
		pthread_mutex_lock(&memory_lock);
		if (inside(address, size, WINDOW_FLOOR, WINDOW_END))
		{
			uint64_t first = (address - WINDOW_FLOOR) / 0x1000;
			uint64_t last = up(address + size - WINDOW_FLOOR, 0x1000) / 0x1000, i;
			if (flags & GUEST_MAP_FIXED_NOREPLACE)
				for (i = first; i < last; i++)
					if (window_live[i])
					{
						pthread_mutex_unlock(&memory_lock);
						return -EEXIST;
					}
			for (i = first; i < last; i++)
				window_live[i] = 1;
		}
		else if (inside(address, size, IMAGE_BASE, image_end))
		{
			if ((flags & GUEST_MAP_FIXED_NOREPLACE) && image_live)
			{
				pthread_mutex_unlock(&memory_lock);
				return -EEXIST;
			}
			image_live = 1;
		}
		else
		{
			struct pool *pool = pool_of(address, size);
			if (pool)
			{
				uint64_t p, end = up(address + size, KPAGE);
				for (p = address & ~(KPAGE - 1); p < end; p += KPAGE)
				{
					uint64_t index = (p - pool->base) / PAGE;
					unsigned bit = 1u << ((p - pool->base) % PAGE / KPAGE);
					if ((flags & GUEST_MAP_FIXED_NOREPLACE) && (pool->mapped[index] & bit))
					{
						pthread_mutex_unlock(&memory_lock);
						return -EEXIST;
					}
				}
				for (p = address & ~(KPAGE - 1); p < end; p += KPAGE)
				{
					uint64_t index = (p - pool->base) / PAGE;
					unsigned bit = 1u << ((p - pool->base) % PAGE / KPAGE);
					if (!pool->used[index])
					{
						pool->used[index] = 1;
						pool->free_pages--;
					}
					pool->mapped[index] |= bit;
				}
			}
			else if (!newly_reserved && (flags & GUEST_MAP_FIXED_NOREPLACE))
			{
				pthread_mutex_unlock(&memory_lock);
				return -EEXIST;
			}
		}
		/* A fresh no-access reservation needs no backing. An existing
		   mapping must be replaced with zeroed, inaccessible pages. */
		if (prot == PROT_NONE && (flags & GUEST_MAP_ANONYMOUS))
		{
			uint64_t p, end = up(address + size, PAGE);
			int backed = 0;
			for (p = address & ~(PAGE - 1); p < end; p += PAGE)
				backed |= committed[p / PAGE];
			if (!backed)
			{
				pthread_mutex_unlock(&memory_lock);
				return (long)address;
			}
		}
		if (commit(address, size) || protect(address, size, PROT_READ | PROT_WRITE))
		{
			pthread_mutex_unlock(&memory_lock);
			return -ENOMEM;
		}
		memset(at, 0, size);
		if (!(flags & GUEST_MAP_ANONYMOUS) && pread(fd, at, size, offset) < 0)
		{
			int e = errno;
			pthread_mutex_unlock(&memory_lock);
			return -e;
		}
		if (protect(address, size, prot))
		{
			pthread_mutex_unlock(&memory_lock);
			return -EACCES;
		}
		pthread_mutex_unlock(&memory_lock);
		return (long)address;
	}
	at = host_low_map(length, PROT_READ | PROT_WRITE);
	if (!at)
		return -ENOMEM;
	memset(at, 0, length);
	if (!(flags & GUEST_MAP_ANONYMOUS) && pread(fd, at, size, offset) < 0)
	{
		int e = errno;
		host_low_unmap(at, length);
		return -e;
	}
	if (protect((uintptr_t)at, length, prot))
	{
		host_low_unmap(at, length);
		return -EACCES;
	}
	return (long)(uintptr_t)at;
}
long host_guest_munmap(uint64_t address, uint64_t size)
{
	uint64_t length = up(size, KPAGE);
	int i;
	if (!size || address >= LOW_LIMIT || length > LOW_LIMIT - address)
		return -EINVAL;
	if (inside(address, length, WINDOW_FLOOR, WINDOW_END))
	{
		uint64_t first = (address - WINDOW_FLOOR) / 0x1000;
		uint64_t last = up(address + size - WINDOW_FLOOR, 0x1000) / 0x1000;
		pthread_mutex_lock(&memory_lock);
		memset(window_live + first, 0, last - first);
		pthread_mutex_unlock(&memory_lock);
		return 0;
	}
	if (inside(address, length, IMAGE_BASE, IMAGE_END))
		return -EINVAL;
	pthread_mutex_lock(&memory_lock);
	for (i = 0; i < 64; i++)
	{
		if (fixed_extra[i].end && address == fixed_extra[i].base && up(address + length, PAGE) == fixed_extra[i].end)
		{
			uint64_t bytes = fixed_extra[i].end - fixed_extra[i].base;
			release(address, bytes);
			sceKernelMunmap((void *)(uintptr_t)address, bytes);
			fixed_extra[i].end = 0;
			pthread_mutex_unlock(&memory_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&memory_lock);
	if (host_low_owns(address, length))
	{
		host_low_unmap((void *)(uintptr_t)address, length);
		return 0;
	}
	return -EINVAL;
}
long host_guest_mprotect(uint64_t address, uint64_t size, int prot)
{
	if (!host_low_owns(address, size))
		return -EINVAL;
	if (prot != PROT_NONE)
	{
		int result;
		pthread_mutex_lock(&memory_lock);
		result = commit(address, size);
		pthread_mutex_unlock(&memory_lock);
		if (result)
			return -ENOMEM;
	}
	return protect(address, size, prot) ? -EACCES : 0;
}

/* Hash tracking needs no signal handler and never changes page protections. */
#define WATCH_COUNT (HALO_GUEST_WINDOW_SIZE / WATCH_HASH_PAGE_SIZE)
static uint8_t watched[WATCH_COUNT];
static uint64_t hashes[WATCH_COUNT];
static uint32_t hashed_frame[WATCH_COUNT], generation[WATCH_COUNT];
static volatile uint32_t serial = 1;
static int watch_active;
static struct watch_hash watch = {.base = (const uint8_t *)(uintptr_t)HALO_GUEST_WINDOW_BASE,
								  .page_count = WATCH_COUNT,
								  .watched = watched,
								  .hash = hashes,
								  .hashed_frame = hashed_frame,
								  .generation = generation,
								  .current_generation = &serial,
								  .frame = 1};
static int watch_range(uint32_t address, uint32_t size, uint32_t *first, uint32_t *last)
{
	uint64_t begin = address, end = begin + size;
	if (!size || end <= WINDOW_FLOOR || begin >= WINDOW_END)
		return 0;
	if (begin < WINDOW_FLOOR)
		begin = WINDOW_FLOOR;
	if (end > WINDOW_END)
		end = WINDOW_END;
	*first = (uint32_t)((begin - WINDOW_BASE) / WATCH_HASH_PAGE_SIZE);
	*last = (uint32_t)((end - 1 - WINDOW_BASE) / WATCH_HASH_PAGE_SIZE);
	return 1;
}
void host_memory_watch_initialize(void)
{
	watch_active = 1;
}
void host_memory_watch_use_hashes(void)
{
}
void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	uint32_t first, last;
	if (watch_active && watch_range(address, size, &first, &last))
		watch_hash_protect(&watch, first, last);
}
uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
	uint32_t first, last;
	return watch_range(address, size, &first, &last) ? watch_hash_generation(&watch, first, last) : 0;
}
uint32_t host_memory_watch_serial(void)
{
	return serial;
}
void host_memory_watch_prepare_write(uint32_t address, uint32_t size)
{
	uint32_t first, last;
	if (watch_range(address, size, &first, &last))
		watch_hash_prepare_write(&watch, first, last);
}
void host_memory_watch_forget(uint32_t address, uint32_t size)
{
	uint32_t first, last;
	if (watch_range(address, size, &first, &last))
		watch_hash_forget(&watch, first, last);
}
void host_memory_watch_begin_frame(void)
{
	watch_hash_begin_frame(&watch);
}
