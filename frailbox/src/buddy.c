#include "buddy.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define BUDDY_MIN_ORDER 5U
#define BUDDY_MAX_ORDER 30U
#define BUDDY_MAGIC 0xBADDCAFEu
#define BUDDY_PREFIX_MAGIC 0xC0DEFACEu
#define BUDDY_DEFAULT_ALIGNMENT 16U

typedef struct buddy_block {
    uint32_t magic;
    uint8_t order;
    uint8_t is_free;
    uint16_t reserved;
    struct buddy_block *next;
} buddy_block_t;

typedef struct buddy_prefix {
    buddy_block_t *block;
    size_t requested_size;
    uint32_t magic;
} buddy_prefix_t;

static buddy_allocator_t g_default_allocator;
static int g_default_initialized = 0;

static size_t align_up_size(size_t value, size_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static int is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1U)) == 0;
}

static uint8_t order_for_size(size_t size) {
    uint8_t order = BUDDY_MIN_ORDER;
    size_t block_size = (size_t)1U << order;

    while (block_size < size && order < BUDDY_MAX_ORDER) {
        order++;
        block_size <<= 1U;
    }

    return order;
}

static size_t next_power_of_two(size_t value) {
    size_t size = (size_t)1U << BUDDY_MIN_ORDER;

    while (size < value && size < ((size_t)1U << BUDDY_MAX_ORDER)) {
        size <<= 1U;
    }

    return size;
}

static buddy_block_t *block_from_offset(const buddy_allocator_t *allocator, size_t offset) {
    return (buddy_block_t *)((char *)allocator->pool + offset);
}

static size_t offset_from_block(const buddy_allocator_t *allocator, const buddy_block_t *block) {
    return (size_t)((const char *)block - (const char *)allocator->pool);
}

static void free_list_push(buddy_allocator_t *allocator, buddy_block_t *block) {
    block->is_free = 1;
    block->next = allocator->free_lists[block->order];
    allocator->free_lists[block->order] = block;
}

static void free_list_remove(buddy_allocator_t *allocator, buddy_block_t *block) {
    buddy_block_t **cursor = &allocator->free_lists[block->order];

    while (*cursor) {
        if (*cursor == block) {
            *cursor = block->next;
            block->next = NULL;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

static buddy_block_t *free_list_pop(buddy_allocator_t *allocator, uint8_t order) {
    buddy_block_t *block = allocator->free_lists[order];

    if (!block) {
        return NULL;
    }

    allocator->free_lists[order] = block->next;
    block->next = NULL;
    block->is_free = 0;
    return block;
}

static buddy_block_t *buddy_block(buddy_allocator_t *allocator, buddy_block_t *block) {
    size_t offset = offset_from_block(allocator, block);
    size_t buddy_offset = offset ^ ((size_t)1U << block->order);

    if (buddy_offset >= allocator->pool_size) {
        return NULL;
    }

    return block_from_offset(allocator, buddy_offset);
}

static void init_block(buddy_block_t *block, uint8_t order, int is_free) {
    block->magic = BUDDY_MAGIC;
    block->order = order;
    block->is_free = (uint8_t)is_free;
    block->reserved = 0;
    block->next = NULL;
}

static void stats_refresh(buddy_allocator_t *allocator) {
    size_t free_bytes = 0;
    size_t largest_free = 0;

    for (uint8_t order = allocator->min_order; order <= allocator->max_order; ++order) {
        buddy_block_t *block = allocator->free_lists[order];
        size_t block_size = (size_t)1U << order;

        while (block) {
            free_bytes += block_size;
            if (block_size > largest_free) {
                largest_free = block_size;
            }
            block = block->next;
        }
    }

    allocator->stats.pool_size = allocator->pool_size;
    allocator->stats.free_bytes = free_bytes;
    allocator->stats.largest_free_block = largest_free;
}

int buddy_allocator_init(buddy_allocator_t *allocator, size_t pool_size, uint32_t flags) {
    size_t actual_size;
    buddy_block_t *root;

    if (!allocator) {
        return -1;
    }

    memset(allocator, 0, sizeof(*allocator));

    actual_size = next_power_of_two(pool_size);
    if (actual_size < ((size_t)1U << BUDDY_MIN_ORDER)) {
        actual_size = (size_t)1U << BUDDY_MIN_ORDER;
    }
    if (!is_power_of_two(actual_size)) {
        return -1;
    }

    allocator->pool = mmap(NULL, actual_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (allocator->pool == MAP_FAILED) {
        allocator->pool = NULL;
        return -1;
    }

    allocator->pool_size = actual_size;
    allocator->flags = flags;
    allocator->min_order = BUDDY_MIN_ORDER;
    allocator->max_order = order_for_size(actual_size);

    root = (buddy_block_t *)allocator->pool;
    init_block(root, allocator->max_order, 1);
    allocator->free_lists[allocator->max_order] = root;
    stats_refresh(allocator);
    return 0;
}

void buddy_allocator_destroy(buddy_allocator_t *allocator) {
    if (!allocator) {
        return;
    }

    if (allocator->pool) {
        munmap(allocator->pool, allocator->pool_size);
    }

    memset(allocator, 0, sizeof(*allocator));
}

void *buddy_allocator_alloc_aligned(buddy_allocator_t *allocator, size_t size, size_t alignment) {
    size_t total_size;
    uint8_t requested_order;
    uint8_t current_order;
    buddy_block_t *block;
    char *payload_base;
    char *payload;
    buddy_prefix_t *prefix;

    if (!allocator || !allocator->pool || size == 0) {
        return NULL;
    }

    if (alignment == 0) {
        alignment = BUDDY_DEFAULT_ALIGNMENT;
    }
    if (!is_power_of_two(alignment)) {
        errno = EINVAL;
        return NULL;
    }
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }

    total_size = sizeof(buddy_block_t) + sizeof(buddy_prefix_t) + size + alignment - 1U;
    requested_order = order_for_size(total_size);
    if (requested_order > allocator->max_order) {
        allocator->stats.failed_allocations++;
        return NULL;
    }

    current_order = requested_order;
    while (current_order <= allocator->max_order && !allocator->free_lists[current_order]) {
        current_order++;
    }
    if (current_order > allocator->max_order) {
        allocator->stats.failed_allocations++;
        return NULL;
    }

    block = free_list_pop(allocator, current_order);
    while (current_order > requested_order) {
        buddy_block_t *sibling;
        size_t half_size;

        current_order--;
        half_size = (size_t)1U << current_order;
        sibling = (buddy_block_t *)((char *)block + half_size);

        init_block(block, current_order, 0);
        init_block(sibling, current_order, 1);
        free_list_push(allocator, sibling);
        allocator->stats.split_count++;
    }

    payload_base = (char *)block + sizeof(buddy_block_t) + sizeof(buddy_prefix_t);
    payload = (char *)align_up_size((size_t)payload_base, alignment);
    prefix = (buddy_prefix_t *)(payload - sizeof(buddy_prefix_t));
    prefix->block = block;
    prefix->requested_size = size;
    prefix->magic = BUDDY_PREFIX_MAGIC;

    allocator->stats.total_allocated += size;
    allocator->stats.current_usage += size;
    allocator->stats.allocation_count++;
    if (allocator->stats.current_usage > allocator->stats.peak_usage) {
        allocator->stats.peak_usage = allocator->stats.current_usage;
    }
    stats_refresh(allocator);

    if (allocator->flags & 1U) {
        memset(payload, 0, size);
    }

    return payload;
}

void *buddy_allocator_alloc(buddy_allocator_t *allocator, size_t size) {
    return buddy_allocator_alloc_aligned(allocator, size, BUDDY_DEFAULT_ALIGNMENT);
}

void buddy_allocator_free(buddy_allocator_t *allocator, void *ptr) {
    buddy_prefix_t *prefix;
    buddy_block_t *block;
    size_t requested_size;

    if (!allocator || !ptr) {
        return;
    }

    prefix = (buddy_prefix_t *)((char *)ptr - sizeof(buddy_prefix_t));
    if (prefix->magic != BUDDY_PREFIX_MAGIC || !prefix->block) {
        return;
    }

    block = prefix->block;
    if (block->magic != BUDDY_MAGIC || block->is_free) {
        return;
    }

    requested_size = prefix->requested_size;
    prefix->magic = 0;
    prefix->block = NULL;

    allocator->stats.total_freed += requested_size;
    if (allocator->stats.current_usage >= requested_size) {
        allocator->stats.current_usage -= requested_size;
    } else {
        allocator->stats.current_usage = 0;
    }
    allocator->stats.deallocation_count++;

    init_block(block, block->order, 1);

    while (block->order < allocator->max_order) {
        buddy_block_t *peer = buddy_block(allocator, block);

        if (!peer || peer->magic != BUDDY_MAGIC || !peer->is_free || peer->order != block->order) {
            break;
        }

        free_list_remove(allocator, peer);
        if (peer < block) {
            block = peer;
        }
        init_block(block, block->order + 1U, 1);
        allocator->stats.merge_count++;
    }

    free_list_push(allocator, block);
    stats_refresh(allocator);
}

buddy_stats_t buddy_allocator_stats(const buddy_allocator_t *allocator) {
    if (!allocator) {
        buddy_stats_t empty = {0};
        return empty;
    }

    return allocator->stats;
}

void *buddy_allocator_pool(const buddy_allocator_t *allocator) {
    return allocator ? allocator->pool : NULL;
}

size_t buddy_allocator_capacity(const buddy_allocator_t *allocator) {
    return allocator ? allocator->pool_size : 0;
}

int buddy_allocator_contains(const buddy_allocator_t *allocator, const void *ptr) {
    if (!allocator || !allocator->pool || !ptr) {
        return 0;
    }

    return ptr >= allocator->pool &&
           ptr < (const void *)((const char *)allocator->pool + allocator->pool_size);
}

int buddy_init(size_t pool_size, uint32_t flags) {
    if (g_default_initialized) {
        buddy_shutdown();
    }

    if (buddy_allocator_init(&g_default_allocator, pool_size, flags) != 0) {
        return -1;
    }

    g_default_initialized = 1;
    return 0;
}

void buddy_shutdown(void) {
    if (!g_default_initialized) {
        return;
    }

    buddy_allocator_destroy(&g_default_allocator);
    g_default_initialized = 0;
}

void *buddy_alloc(size_t size) {
    if (!g_default_initialized) {
        return NULL;
    }

    return buddy_allocator_alloc(&g_default_allocator, size);
}

void buddy_free(void *ptr) {
    if (!g_default_initialized) {
        return;
    }

    buddy_allocator_free(&g_default_allocator, ptr);
}

buddy_stats_t buddy_stats(void) {
    return buddy_allocator_stats(&g_default_allocator);
}
