#ifndef FRAILBOX_BUDDY_H
#define FRAILBOX_BUDDY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct buddy_stats {
    uint64_t total_allocated;
    uint64_t total_freed;
    uint64_t peak_usage;
    uint64_t current_usage;
    uint64_t allocation_count;
    uint64_t deallocation_count;
    uint64_t failed_allocations;
    uint64_t split_count;
    uint64_t merge_count;
    size_t   pool_size;
    size_t   free_bytes;
    size_t   largest_free_block;
} buddy_stats_t;

typedef struct buddy_block buddy_block_t;

typedef struct buddy_allocator {
    void *pool;
    size_t pool_size;
    uint32_t flags;
    uint8_t min_order;
    uint8_t max_order;
    buddy_block_t *free_lists[31];
    buddy_stats_t stats;
} buddy_allocator_t;

int           buddy_init(size_t pool_size, uint32_t flags);
void          buddy_shutdown(void);
void         *buddy_alloc(size_t size);
void          buddy_free(void *ptr);
buddy_stats_t buddy_stats(void);

int           buddy_allocator_init(buddy_allocator_t *allocator, size_t pool_size, uint32_t flags);
void          buddy_allocator_destroy(buddy_allocator_t *allocator);
void         *buddy_allocator_alloc(buddy_allocator_t *allocator, size_t size);
void         *buddy_allocator_alloc_aligned(buddy_allocator_t *allocator, size_t size, size_t alignment);
void          buddy_allocator_free(buddy_allocator_t *allocator, void *ptr);
buddy_stats_t buddy_allocator_stats(const buddy_allocator_t *allocator);
void         *buddy_allocator_pool(const buddy_allocator_t *allocator);
size_t        buddy_allocator_capacity(const buddy_allocator_t *allocator);
int           buddy_allocator_contains(const buddy_allocator_t *allocator, const void *ptr);

#ifdef __cplusplus
}
#endif

#endif
