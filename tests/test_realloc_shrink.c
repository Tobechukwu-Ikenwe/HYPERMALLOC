#include "hypermalloc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    unsigned char *block = hm_malloc(1024);
    void *guard = hm_malloc(64);
    if (block == NULL || guard == NULL) return 1;
    memset(block, 0x5a, 1024);
    unsigned char *shrunk = hm_realloc(block, 64);
    if (shrunk != block || hm_usable_size(shrunk) != 64) {
        fputs("realloc shrink did not release the tail\n", stderr);
        return 1;
    }
    for (size_t i = 0; i < 64; ++i) {
        if (shrunk[i] != 0x5a) return 1;
    }
    if (hm_check_heap() != 0) return 1;
    void *reused = hm_malloc(512);
    if (reused == NULL || hm_check_heap() != 0) return 1;
    hm_free(shrunk);
    hm_free(guard);
    hm_free(reused);
    if (hm_check_heap() != 0) return 1;

    void *aligned = hm_memalign(64, 65);
    if (aligned == NULL ||
        (uintptr_t)aligned % 64 != 0) {
        fputs("hm_memalign rejected a non-multiple size\n", stderr);
        return 1;
    }
    hm_free(aligned);
    return hm_check_heap() != 0;
}
