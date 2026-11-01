#define _GNU_SOURCE
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    unsigned char *ptr = malloc(37);
    if (ptr == NULL || (uintptr_t)ptr % 16 != 0 ||
        malloc_usable_size(ptr) < 37) return 1;
    memset(ptr, 0x5a, 37);
    unsigned char *grown = realloc(ptr, 5000);
    if (grown == NULL) return 2;
    for (size_t i = 0; i < 37; ++i)
        if (grown[i] != 0x5a) return 3;
    free(grown);

    void *aligned = NULL;
    if (posix_memalign(&aligned, 4096, 7) != 0 ||
        (uintptr_t)aligned % 4096 != 0) return 4;
    free(aligned);
    aligned = aligned_alloc(64, 128);
    if (aligned == NULL || (uintptr_t)aligned % 64 != 0) return 5;
    free(aligned);

    void *zero = calloc(23, 17);
    if (zero == NULL) return 6;
    for (size_t i = 0; i < 23 * 17; ++i)
        if (((unsigned char *)zero)[i] != 0) return 7;
    free(zero);

    void *from_memalign = memalign(256, 33);
    if (from_memalign == NULL || (uintptr_t)from_memalign % 256 != 0) return 8;
    free(from_memalign);
    void *from_valloc = valloc(19);
    if (from_valloc == NULL ||
        (uintptr_t)from_valloc % (size_t)getpagesize() != 0) return 9;
    free(from_valloc);
    void *invalid = (void *)(uintptr_t)0x1234;
    if (posix_memalign(&invalid, 3, 64) != EINVAL ||
        invalid != (void *)(uintptr_t)0x1234) return 10;
    unsigned char *array = reallocarray(NULL, 32, 4);
    if (array == NULL) return 11;
    memset(array, 0x31, 128);
    volatile size_t impossible_count = SIZE_MAX;
    errno = 0;
    if (reallocarray(array, impossible_count, 2) != NULL || errno != ENOMEM ||
        array[0] != 0x31) return 12;
    free(array);
    return 0;
}
