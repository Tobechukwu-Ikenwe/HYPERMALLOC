#include "hypermalloc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    unsigned char *block = hyper_malloc(1024);
    void *guard = hyper_malloc(64);
    if (block == NULL || guard == NULL) return 1;
    memset(block, 0x5a, 1024);
    unsigned char *shrunk = hyper_realloc(block, 64);
    if (shrunk != block || hyper_malloc_usable_size(shrunk) != 64) {
        fputs("realloc shrink did not release the tail\n", stderr);
        return 1;
    }
    for (size_t i = 0; i < 64; ++i) {
        if (shrunk[i] != 0x5a) return 1;
    }
    if (!check_heap()) return 1;
    void *reused = hyper_malloc(512);
    if (reused == NULL || !check_heap()) return 1;
    hyper_free(shrunk);
    hyper_free(guard);
    hyper_free(reused);
    if (!check_heap()) return 1;

    void *aligned = NULL;
    if (hyper_posix_memalign(&aligned, 64, 65) != 0 || aligned == NULL ||
        (uintptr_t)aligned % 64 != 0) {
        fputs("posix_memalign rejected a non-multiple size\n", stderr);
        return 1;
    }
    hyper_free(aligned);
    return !check_heap();
}
