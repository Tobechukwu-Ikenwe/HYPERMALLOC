/*
 * Standard allocation names for LD_PRELOAD
 * ========================================
 *
 * core.c implements the allocator under hm_* names. This file is the adapter
 * for an existing Linux program that calls malloc/free and cannot be changed
 * to call hm_malloc/hm_free. The shared target combines core.c + preload.c;
 * the static target contains only core.c, so linking it does not replace the
 * program's ordinary malloc symbol.
 *
 * With LD_PRELOAD=.../libhypermalloc_preload.so, the dynamic linker loads this
 * shared library before the program's usual libraries. Its exported malloc,
 * free, and related names are then chosen for matching dynamically linked
 * calls. For example:
 *
 *   program malloc(80) -> this file's malloc(80) -> core.c's hm_malloc(80)
 *   program free(p)    -> this file's free(p)    -> core.c's hm_free(p)
 *
 * No block splitting, mapping, or free-list logic lives here; core.c owns it.
 * These functions translate standard API names and edge-case rules to hm_*.
 * A pointer allocated through this adapter must be freed through the same
 * allocator. C syntax: "void *" is a generic pointer, size_t is a byte count,
 * NULL is no pointer, and errno is the calling thread's error number.
 */
#define _GNU_SOURCE
#include "hypermalloc.h"

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

/* The four common entry points are direct forwards. hm_* performs locking,
 * pointer bookkeeping, and allocation; these wrappers add no metadata. */
void *malloc(size_t size) { return hm_malloc(size); }
void free(void *ptr) { hm_free(ptr); }
void *calloc(size_t count, size_t size) { return hm_calloc(count, size); }
void *realloc(void *ptr, size_t size) { return hm_realloc(ptr, size); }

/* reallocarray asks for count * size bytes. Check multiplication before it
 * can wrap around to a smaller number; hm_realloc then preserves old data or
 * leaves the old allocation live if the new allocation fails. */
void *reallocarray(void *ptr, size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    return hm_realloc(ptr, count * size);
}

/* C's aligned_alloc requires size to be a multiple of alignment. hm_memalign
 * handles the power-of-two alignment check and chooses a direct mapping when
 * the requested alignment exceeds the ordinary 16-byte alignment. */
void *aligned_alloc(size_t alignment, size_t size)
{
    if (alignment == 0 || size % alignment != 0) {
        errno = EINVAL;
        return NULL;
    }
    return hm_memalign(alignment, size);
}

/* POSIX reports errors as the return value, not through errno. Only write
 * *result on success; keep the caller's prior errno even though hm_memalign
 * uses errno internally. The bit test rejects non-power-of-two alignments. */
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

/* GNU/BSD-style aligned allocation: unlike aligned_alloc, size need not be
 * a multiple of alignment. hm_memalign validates the alignment. */
void *memalign(size_t alignment, size_t size)
{
    return hm_memalign(alignment, size);
}

/* valloc requests an address aligned to this machine's memory page size.
 * getpagesize() supplies that boundary; core.c provides the storage. */
void *valloc(size_t size)
{
    return hm_memalign((size_t)getpagesize(), size);
}

/* Report the block's usable capacity, which can exceed the requested size
 * because blocks and mappings are rounded upward. */
size_t malloc_usable_size(void *ptr)
{
    return hm_usable_size(ptr);
}
