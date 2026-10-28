#include "hypermalloc.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static uint64_t rng_state = UINT64_C(0x4ddcf479a42b93e1);

static uint32_t random_u32(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)rng_state;
}

static int basic(void)
{
    void *zero_a = hm_malloc(0);
    void *zero_b = hm_malloc(0);
    REQUIRE(zero_a != NULL && zero_b != NULL && zero_a != zero_b);
    REQUIRE((uintptr_t)zero_a % 16 == 0);
    hm_free(zero_a);
    hm_free(zero_b);
    hm_free(NULL);
    for (size_t size = 1; size <= 4096; ++size) {
        unsigned char *ptr = hm_malloc(size);
        REQUIRE(ptr != NULL && (uintptr_t)ptr % 16 == 0);
        memset(ptr, 0x5a, size);
        REQUIRE(hm_usable_size(ptr) >= size);
        hm_free(ptr);
    }
    errno = 0;
    REQUIRE(hm_malloc(SIZE_MAX) == NULL && errno == ENOMEM);
    errno = 0;
    REQUIRE(hm_calloc(SIZE_MAX, 2) == NULL && errno == ENOMEM);
    unsigned char *zeroed = hm_calloc(64, 64);
    REQUIRE(zeroed != NULL);
    for (size_t i = 0; i < 4096; ++i) REQUIRE(zeroed[i] == 0);
    hm_free(zeroed);
    hm_stats before_large;
    hm_get_stats(&before_large);
    unsigned char *large = hm_malloc(128 * 1024);
    REQUIRE(large != NULL);
    memset(large, 0x6b, 128 * 1024);
    hm_stats during_large;
    hm_get_stats(&during_large);
    REQUIRE(during_large.current_heap_bytes > before_large.current_heap_bytes);
    unsigned char *larger = hm_realloc(large, 256 * 1024);
    REQUIRE(larger != NULL);
    for (size_t i = 0; i < 128 * 1024; ++i) REQUIRE(larger[i] == 0x6b);
    hm_free(larger);
    hm_stats after_large;
    hm_get_stats(&after_large);
    REQUIRE(after_large.current_heap_bytes == before_large.current_heap_bytes);
    REQUIRE(hm_check_heap() == 0);
    return 0;
}

static int alignment(void)
{
    for (size_t align = 16; align <= 65536; align *= 2) {
        unsigned char *ptr = hm_memalign(align, 65);
        REQUIRE(ptr != NULL && (uintptr_t)ptr % align == 0);
        memset(ptr, 0xa5, 65);
        REQUIRE(hm_usable_size(ptr) >= 65);
        hm_free(ptr);
    }
    REQUIRE(hm_check_heap() == 0);
    return 0;
}

static int coalesce_realloc(void)
{
    unsigned char *left = hm_malloc(1024);
    unsigned char *middle = hm_malloc(1024);
    unsigned char *right = hm_malloc(1024);
    void *guard = hm_malloc(64);
    REQUIRE(left && middle && right && guard);
    memset(middle, 0x73, 1024);
    hm_free(right);
    middle = hm_realloc(middle, 1800);
    REQUIRE(middle != NULL);
    for (size_t i = 0; i < 1024; ++i) REQUIRE(middle[i] == 0x73);
    hm_free(left);
    hm_free(middle);
    REQUIRE(hm_check_heap() == 0);
    void *combined = hm_malloc(2500);
    REQUIRE(combined != NULL);
    hm_free(combined);
    hm_free(guard);
    REQUIRE(hm_check_heap() == 0);
    return 0;
}

static int random_stress(void)
{
    enum { SLOTS = 128, OPERATIONS = 10000 };
    unsigned char *ptrs[SLOTS] = {0};
    size_t sizes[SLOTS] = {0};
    unsigned char patterns[SLOTS] = {0};
    for (size_t op = 0; op < OPERATIONS; ++op) {
        size_t slot = random_u32() % SLOTS;
        if (ptrs[slot] != NULL) {
            for (size_t i = 0; i < sizes[slot]; ++i)
                REQUIRE(ptrs[slot][i] == patterns[slot]);
            if (random_u32() % 3 == 0) {
                size_t new_size = 1 + random_u32() % 4096;
                unsigned char *changed = hm_realloc(ptrs[slot], new_size);
                REQUIRE(changed != NULL);
                size_t kept = sizes[slot] < new_size ? sizes[slot] : new_size;
                for (size_t i = 0; i < kept; ++i)
                    REQUIRE(changed[i] == patterns[slot]);
                ptrs[slot] = changed;
                sizes[slot] = new_size;
                memset(ptrs[slot], patterns[slot], new_size);
            } else {
                hm_free(ptrs[slot]);
                ptrs[slot] = NULL;
            }
        } else {
            sizes[slot] = 1 + random_u32() % 4096;
            ptrs[slot] = hm_malloc(sizes[slot]);
            REQUIRE(ptrs[slot] != NULL);
            patterns[slot] = (unsigned char)(slot + 1);
            memset(ptrs[slot], patterns[slot], sizes[slot]);
        }
        if (op % 100 == 0) REQUIRE(hm_check_heap() == 0);
    }
    for (size_t slot = 0; slot < SLOTS; ++slot) hm_free(ptrs[slot]);
    REQUIRE(hm_check_heap() == 0);
    hm_stats result;
    hm_get_stats(&result);
    REQUIRE(result.current_live_bytes == 0);
    return 0;
}

int main(void)
{
    if (basic() || alignment() || coalesce_realloc() || random_stress()) {
        fprintf(stderr, "seed=0x%llx\n", (unsigned long long)rng_state);
        return 1;
    }
    return 0;
}
