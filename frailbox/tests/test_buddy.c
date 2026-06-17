#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../include/buddy.h"

static int failures = 0;

#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        failures++; \
    } \
} while (0)

static int test_global_allocator(void) {
    void *a;
    void *b;
    buddy_stats_t stats;

    ASSERT_TRUE(buddy_init(4096, 0) == 0, "buddy_init should succeed");
    a = buddy_alloc(96);
    b = buddy_alloc(128);
    ASSERT_TRUE(a != NULL, "buddy_alloc should return a pointer");
    ASSERT_TRUE(b != NULL, "second buddy_alloc should return a pointer");

    stats = buddy_stats();
    ASSERT_TRUE(stats.current_usage >= 224, "current_usage should include both allocations");
    ASSERT_TRUE(stats.allocation_count == 2, "allocation_count should be 2");

    buddy_free(a);
    buddy_free(b);
    stats = buddy_stats();
    ASSERT_TRUE(stats.current_usage == 0, "current_usage should return to zero after free");
    ASSERT_TRUE(stats.deallocation_count == 2, "deallocation_count should be 2");
    buddy_shutdown();
    return failures == 0 ? 0 : 1;
}

static int test_context_allocator(void) {
    buddy_allocator_t allocator;
    void *aligned;
    void *other;
    buddy_stats_t stats;

    ASSERT_TRUE(buddy_allocator_init(&allocator, 8192, 0) == 0, "buddy_allocator_init should succeed");
    aligned = buddy_allocator_alloc_aligned(&allocator, 200, 256);
    other = buddy_allocator_alloc(&allocator, 300);

    ASSERT_TRUE(aligned != NULL, "aligned allocation should succeed");
    ASSERT_TRUE(other != NULL, "plain allocation should succeed");
    ASSERT_TRUE(((uintptr_t)aligned % 256U) == 0, "aligned allocation should honor the requested alignment");

    stats = buddy_allocator_stats(&allocator);
    ASSERT_TRUE(stats.largest_free_block > 0, "largest_free_block should be tracked");
    ASSERT_TRUE(stats.split_count > 0, "split_count should increase when allocations split blocks");

    buddy_allocator_free(&allocator, other);
    buddy_allocator_free(&allocator, aligned);
    stats = buddy_allocator_stats(&allocator);
    ASSERT_TRUE(stats.current_usage == 0, "context allocator usage should drop back to zero");
    ASSERT_TRUE(stats.merge_count > 0, "merge_count should increase after freeing buddy blocks");

    buddy_allocator_destroy(&allocator);
    return failures == 0 ? 0 : 1;
}

static int test_double_free_is_ignored(void) {
    buddy_allocator_t allocator;
    void *ptr;
    buddy_stats_t stats;

    ASSERT_TRUE(buddy_allocator_init(&allocator, 4096, 0) == 0, "buddy_allocator_init should succeed");
    ptr = buddy_allocator_alloc(&allocator, 128);
    ASSERT_TRUE(ptr != NULL, "allocation should succeed");

    buddy_allocator_free(&allocator, ptr);
    buddy_allocator_free(&allocator, ptr);

    stats = buddy_allocator_stats(&allocator);
    ASSERT_TRUE(stats.current_usage == 0, "double free should not underflow current_usage");
    ASSERT_TRUE(stats.deallocation_count == 1, "double free should not count as a second deallocation");

    buddy_allocator_destroy(&allocator);
    return failures == 0 ? 0 : 1;
}

int main(void) {
    int rc = 0;

    rc |= test_global_allocator();
    rc |= test_context_allocator();
    rc |= test_double_free_is_ignored();

    if (failures == 0) {
        printf("buddy allocator tests passed\n");
    }

    return rc;
}
