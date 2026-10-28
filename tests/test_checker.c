#include "hypermalloc.h"

#include <stdint.h>
#include <stdio.h>

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    void *left = hm_malloc(64);
    unsigned char *middle = hm_malloc(256);
    void *right = hm_malloc(64);
    REQUIRE(left && middle && right);
    size_t capacity = hm_usable_size(middle);
    REQUIRE(hm_check_heap() == 0);

    size_t *header = (size_t *)(middle - sizeof(size_t));
    size_t original_header = *header;
    *header ^= UINT64_C(0x10);
    REQUIRE(hm_check_heap() != 0);
    REQUIRE(hm_check_heap_verbose() != 0);
    *header = original_header;
    REQUIRE(hm_check_heap() == 0);

    hm_free(middle);
    size_t *footer = (size_t *)(middle + capacity - sizeof(size_t));
    size_t original_footer = *footer;
    *footer ^= UINT64_C(0x10);
    REQUIRE(hm_check_heap() != 0);
    *footer = original_footer;
    REQUIRE(hm_check_heap() == 0);

    void **free_prev = (void **)middle;
    void *original_prev = *free_prev;
    *free_prev = left;
    REQUIRE(hm_check_heap() != 0);
    *free_prev = original_prev;
    REQUIRE(hm_check_heap() == 0);
    hm_free(left);
    hm_free(right);
    REQUIRE(hm_check_heap() == 0);
    return 0;
}
