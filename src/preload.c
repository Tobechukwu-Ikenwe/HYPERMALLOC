#define _GNU_SOURCE
#include "hypermalloc.h"

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

void *malloc(size_t size) { return hm_malloc(size); }
void free(void *ptr) { hm_free(ptr); }
void *calloc(size_t count, size_t size) { return hm_calloc(count, size); }
void *realloc(void *ptr, size_t size) { return hm_realloc(ptr, size); }

void *reallocarray(void *ptr, size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    return hm_realloc(ptr, count * size);
}

void *aligned_alloc(size_t alignment, size_t size)
{
    if (alignment == 0 || size % alignment != 0) {
        errno = EINVAL;
        return NULL;
    }
    return hm_memalign(alignment, size);
}

int posix_memalign(void **result, size_t alignment, size_t size)
{
    if (alignment < sizeof(void *) || alignment % sizeof(void *) != 0 ||
        (alignment & (alignment - 1)) != 0)
        return EINVAL;
    int saved_errno = errno;
    void *ptr = hm_memalign(alignment, size);
    int error = ptr == NULL ? ENOMEM : 0;
    if (ptr != NULL) *result = ptr;
    errno = saved_errno;
    return error;
}

void *memalign(size_t alignment, size_t size)
{
    return hm_memalign(alignment, size);
}

void *valloc(size_t size)
{
    return hm_memalign((size_t)getpagesize(), size);
}

size_t malloc_usable_size(void *ptr)
{
    return hm_usable_size(ptr);
}
