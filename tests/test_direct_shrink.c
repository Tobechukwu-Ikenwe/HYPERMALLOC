#include "hypermalloc.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(test) do { if (!(test)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); \
    return 1; \
} } while (0)

int main(void)
{
    hm_stats before, after_alloc, after_shrink, after_free;
    hm_get_stats(&before);
    unsigned char *ptr = hm_memalign(4096, 512 * 1024);
    REQUIRE(ptr != NULL && (uintptr_t)ptr % 4096 == 0);
    memset(ptr, 0xa7, 256 * 1024);
    hm_get_stats(&after_alloc);
    REQUIRE(after_alloc.current_heap_bytes > before.current_heap_bytes);
    errno = 7;
    unsigned char *shrunk = hm_realloc(ptr, 256 * 1024);
    REQUIRE(shrunk == ptr && errno == 7);
    for (size_t i = 0; i < 256 * 1024; ++i) REQUIRE(shrunk[i] == 0xa7);
    hm_get_stats(&after_shrink);
    REQUIRE(after_shrink.current_heap_bytes < after_alloc.current_heap_bytes);
    REQUIRE(after_shrink.current_live_bytes == before.current_live_bytes +
                                              256 * 1024);
    REQUIRE(hm_usable_size(shrunk) >= 256 * 1024);
    REQUIRE(hm_check_heap() == 0);
    errno = 0;
    REQUIRE(hm_realloc(shrunk, SIZE_MAX) == NULL && errno == ENOMEM);
    REQUIRE(shrunk[0] == 0xa7 && shrunk[256 * 1024 - 1] == 0xa7);
    unsigned char *smaller = hm_realloc(shrunk, 128 * 1024);
    REQUIRE(smaller == shrunk && hm_check_heap() == 0);
    unsigned char *regrown = hm_realloc(smaller, 512 * 1024);
    REQUIRE(regrown != NULL && (uintptr_t)regrown % 4096 == 0);
    for (size_t i = 0; i < 128 * 1024; ++i) REQUIRE(regrown[i] == 0xa7);
    hm_free(regrown);
    hm_get_stats(&after_free);
    REQUIRE(after_free.current_heap_bytes == before.current_heap_bytes);
    REQUIRE(hm_check_heap() == 0);

    /* A direct map can also grow in place into its page-rounded slack. */
    unsigned char *slack = hm_malloc(128 * 1024);
    REQUIRE(slack != NULL);
    memset(slack, 0x39, 128 * 1024);
    unsigned char *grown = hm_realloc(slack, 128 * 1024 + 1);
    REQUIRE(grown == slack);
    for (size_t i = 0; i < 128 * 1024; ++i) REQUIRE(grown[i] == 0x39);
    hm_free(grown);
    REQUIRE(hm_check_heap() == 0);
    return 0;
}
