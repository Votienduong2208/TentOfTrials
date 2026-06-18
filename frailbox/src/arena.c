#include "arena.h"
#include "buddy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define DEFAULT_ALIGNMENT 16

#ifdef USE_BUDDY_ALLOCATOR
typedef struct buddy_region {
    arena_region_t base;
    buddy_allocator_t allocator;
} buddy_region_t;

static buddy_region_t *buddy_region_cast(arena_region_t *region) {
    return (buddy_region_t *)region;
}

static const buddy_region_t *buddy_region_cast_const(const arena_region_t *region) {
    return (const buddy_region_t *)region;
}
#endif

static arena_region_t *region_alloc(size_t size, uint32_t flags) {
#ifdef USE_BUDDY_ALLOCATOR
    buddy_region_t *region = calloc(1, sizeof(buddy_region_t));
    if (!region) {
        return NULL;
    }

    if (buddy_allocator_init(&region->allocator, size, flags) != 0) {
        free(region);
        return NULL;
    }

    region->base.start = buddy_allocator_pool(&region->allocator);
    region->base.size = buddy_allocator_capacity(&region->allocator);
    region->base.used = 0;
    region->base.flags = flags;
    region->base.next = NULL;
    return &region->base;
#else
    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    int mmap_prot = PROT_READ | PROT_WRITE;

    if (flags & ARENA_HUGE_PAGES) {
        mmap_flags |= MAP_HUGETLB;
    }

    void *addr = mmap(NULL, size, mmap_prot, mmap_flags, -1, 0);
    if (addr == MAP_FAILED) {
        return NULL;
    }

    arena_region_t *region = malloc(sizeof(arena_region_t));
    if (!region) {
        munmap(addr, size);
        return NULL;
    }

    region->start = addr;
    region->size = size;
    region->used = 0;
    region->flags = flags;
    region->next = NULL;

    return region;
#endif
}

arena_t *arena_create(size_t default_region_size, uint32_t flags) {
    arena_t *arena = calloc(1, sizeof(arena_t));
    if (!arena) {
        return NULL;
    }

    if (default_region_size == 0) {
        default_region_size = 1024 * 1024 * 64;
    }

    arena->default_region_size = default_region_size;
    arena->flags = flags;

    arena_region_t *region = region_alloc(default_region_size, flags);
    if (!region) {
        free(arena);
        return NULL;
    }

    arena->regions = region;
    arena->current = region;
    arena->stats.region_count = 1;

    return arena;
}

void arena_destroy(arena_t *arena) {
    if (!arena) return;

    arena_region_t *region = arena->regions;
    while (region) {
        arena_region_t *next = region->next;
#ifdef USE_BUDDY_ALLOCATOR
        buddy_allocator_destroy(&buddy_region_cast(region)->allocator);
#else
        munmap(region->start, region->size);
#endif
        free(region);
        region = next;
    }

    memset(&arena->stats, 0, sizeof(arena_stats_t));
    free(arena);
}

void *arena_alloc(arena_t *arena, size_t size) {
    return arena_alloc_aligned(arena, size, DEFAULT_ALIGNMENT);
}

void *arena_alloc_aligned(arena_t *arena, size_t size, size_t alignment) {
    if (!arena || size == 0) return NULL;

#ifdef USE_BUDDY_ALLOCATOR
    arena_region_t *region = arena->current;
    while (region) {
        void *ptr = buddy_allocator_alloc_aligned(&buddy_region_cast(region)->allocator, size, alignment);
        if (ptr) {
            buddy_stats_t buddy = buddy_allocator_stats(&buddy_region_cast(region)->allocator);
            region->used = buddy.current_usage;
            arena->stats.total_allocated += size;
            arena->stats.current_usage += size;
            arena->stats.allocation_count++;
            if (arena->stats.current_usage > arena->stats.peak_usage) {
                arena->stats.peak_usage = arena->stats.current_usage;
            }
            return ptr;
        }
        region = region->next;
    }

    {
        size_t required = size + alignment + sizeof(void *) * 2U;
        size_t new_size = (required > arena->default_region_size)
                        ? required : arena->default_region_size;
        arena_region_t *new_region = arena_new_region(arena, new_size);
        if (!new_region) {
            return NULL;
        }

        void *ptr = buddy_allocator_alloc_aligned(&buddy_region_cast(new_region)->allocator, size, alignment);
        if (!ptr) {
            return NULL;
        }

        {
            buddy_stats_t buddy = buddy_allocator_stats(&buddy_region_cast(new_region)->allocator);
            new_region->used = buddy.current_usage;
        }
        arena->stats.total_allocated += size;
        arena->stats.current_usage += size;
        arena->stats.allocation_count++;
        if (arena->stats.current_usage > arena->stats.peak_usage) {
            arena->stats.peak_usage = arena->stats.current_usage;
        }
        return ptr;
    }
#else
    size = ALIGN_UP(size, alignment);

    if (arena->current->used + size > arena->current->size) {
        size_t new_size = (size > arena->default_region_size)
                         ? size : arena->default_region_size;
        if (!arena_new_region(arena, new_size)) {
            return NULL;
        }
    }

    void *ptr = (char *)arena->current->start + arena->current->used;
    arena->current->used += size;

    if (arena->flags & ARENA_ZERO_INIT) {
        memset(ptr, 0, size);
    }

    arena->stats.total_allocated += size;
    arena->stats.current_usage += size;
    arena->stats.allocation_count++;

    if (arena->stats.current_usage > arena->stats.peak_usage) {
        arena->stats.peak_usage = arena->stats.current_usage;
    }

    return ptr;
#endif
}

void *arena_calloc(arena_t *arena, size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = arena_alloc(arena, total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void arena_reset(arena_t *arena) {
    if (!arena) return;

#ifdef USE_BUDDY_ALLOCATOR
    arena_region_t *region = arena->regions;
    while (region) {
        buddy_region_t *buddy_region = buddy_region_cast(region);
        uint32_t flags = region->flags;
        size_t size = region->size;

        buddy_allocator_destroy(&buddy_region->allocator);
        buddy_allocator_init(&buddy_region->allocator, size, flags);
        region->start = buddy_allocator_pool(&buddy_region->allocator);
        region->size = buddy_allocator_capacity(&buddy_region->allocator);
        region->used = 0;
        region = region->next;
    }
    arena->current = arena->regions;
    arena->stats.current_usage = 0;
    arena->stats.total_freed = arena->stats.total_allocated;
    arena->stats.deallocation_count = arena->stats.allocation_count;
#else
    arena_region_t *region = arena->regions;
    while (region) {
        region->used = 0;
        region = region->next;
    }
    arena->current = arena->regions;
    arena->stats.current_usage = 0;
#endif
}

arena_region_t *arena_new_region(arena_t *arena, size_t min_size) {
    size_t size = (min_size > arena->default_region_size)
                 ? min_size : arena->default_region_size;

    arena_region_t *region = region_alloc(size, arena->flags);
    if (!region) return NULL;

    arena->current->next = region;
    arena->current = region;
    arena->stats.region_count++;

    return region;
}

int arena_merge_regions(arena_t *arena) {
    (void)arena;
    return 0;
}

int arena_trim(arena_t *arena) {
    (void)arena;
    return 0;
}

arena_stats_t arena_get_stats(const arena_t *arena) {
    return arena->stats;
}

size_t arena_total_capacity(const arena_t *arena) {
    size_t total = 0;
    arena_region_t *region = arena->regions;
    while (region) {
        total += region->size;
        region = region->next;
    }
    return total;
}

int arena_contains(const arena_t *arena, const void *ptr) {
    arena_region_t *region = arena->regions;
    while (region) {
#ifdef USE_BUDDY_ALLOCATOR
        if (buddy_allocator_contains(&buddy_region_cast_const(region)->allocator, ptr)) {
            return 1;
        }
#else
        if (ptr >= region->start &&
            ptr < (char *)region->start + region->size) {
            return 1;
        }
#endif
        region = region->next;
    }
    return 0;
}
