#ifndef HYPERMALLOC_H
#define HYPERMALLOC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hm_stats {
    size_t current_live_bytes;
    size_t peak_live_bytes;
    size_t current_heap_bytes;
    size_t peak_heap_bytes;
    size_t internal_fragmentation_bytes;
    uint64_t mallocs;
    uint64_t frees;
    uint64_t reallocs;
    uint64_t splits;
    uint64_t coalesces;
    uint64_t mmaps;
    uint64_t search_steps;
} hm_stats;

/* Allocate at least size bytes, aligned to 16 bytes. A zero-size request
 * allocates one usable byte. On failure, return NULL and set errno=ENOMEM. */
void *hm_malloc(size_t size);
/* Free a pointer returned by this allocator. NULL is a no-op. Invalid frees
 * abort in HM_DEBUG builds; release builds ignore them. */
void hm_free(void *ptr);
/* Allocate nmemb*size zeroed bytes. Multiplication overflow returns NULL and
 * sets errno=ENOMEM. Fresh anonymous mmap pages are already zero. */
void *hm_calloc(size_t nmemb, size_t size);
/* Resize an allocation, preserving min(old,new) bytes. NULL behaves as
 * hm_malloc. A zero size frees ptr and returns NULL. Failure leaves ptr live. */
void *hm_realloc(void *ptr, size_t size);
/* Allocate size bytes at a power-of-two alignment of at least sizeof(void*).
 * A zero-size request returns a unique freeable allocation. */
void *hm_memalign(size_t alignment, size_t size);
/* Return the usable capacity of a live allocation, or zero for NULL/invalid. */
size_t hm_usable_size(void *ptr);
/* Return zero if all heap invariants hold, nonzero otherwise. */
int hm_check_heap(void);
/* As hm_check_heap, but write the first failure's code, address, expected and
 * actual values to standard error using write(2). */
int hm_check_heap_verbose(void);
/* Copy allocator counters to out. NULL is ignored. With HM_STATS=0, counter
 * fields are zero; current memory accounting still remains available. */
void hm_get_stats(hm_stats *out);

void *hyper_malloc(size_t size);
void *hyper_calloc(size_t count, size_t size);
void hyper_free(void *ptr);
void *hyper_realloc(void *ptr, size_t size);
void *hyper_reallocarray(void *ptr, size_t count, size_t size);
void *hyper_aligned_alloc(size_t alignment, size_t size);
int hyper_posix_memalign(void **memptr, size_t alignment, size_t size);
size_t hyper_malloc_usable_size(void *ptr);
int check_heap(void);
#ifdef __cplusplus
}
#endif
#endif
