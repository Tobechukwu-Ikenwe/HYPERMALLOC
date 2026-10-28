#define _GNU_SOURCE
#include "hypermalloc.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef HM_THREADSAFE
#define HM_THREADSAFE 1
#endif
#ifndef HM_DEBUG
#define HM_DEBUG 0
#endif
#ifndef HM_STATS
#define HM_STATS 1
#endif
#ifndef HM_DEBUG_STRIDE
#define HM_DEBUG_STRIDE 1
#endif

enum {
    HM_ALIGNMENT = 16,
    HM_MIN_BLOCK = 48,
    HM_CHUNK_BYTES = 256 * 1024,
    HM_LARGE_THRESHOLD = 128 * 1024,
    HM_CLASS_COUNT = 64,
    HM_FLAG_ALLOC = 1,
    HM_FLAG_PREV_ALLOC = 2,
    HM_FLAG_MMAP = 4
};

typedef struct Block {
    size_t requested;
    size_t word;
} Block;

typedef struct FreeLinks {
    Block *prev;
    Block *next;
} FreeLinks;

typedef struct Chunk {
    struct Chunk *next;
    size_t mapping_size;
    Block *first;
    Block *epilogue;
} Chunk;

typedef struct DirectMap {
    struct DirectMap *next;
    void *mapping;
    size_t mapping_size;
    void *payload;
    size_t alignment;
} DirectMap;

static Chunk *chunks;
static DirectMap *direct_maps;
static Block *free_lists[HM_CLASS_COUNT];
static uint64_t free_bitmap;
static hm_stats stats;
#if HM_DEBUG
static uint64_t debug_operations;
#endif
#if HM_THREADSAFE
static pthread_mutex_t heap_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static void lock_heap(void)
{
#if HM_THREADSAFE
    (void)pthread_mutex_lock(&heap_mutex);
#endif
}

static void unlock_heap(void)
{
#if HM_THREADSAFE
    (void)pthread_mutex_unlock(&heap_mutex);
#endif
}

static void invalid_use(void)
{
#if HM_DEBUG
    static const char message[] = "HyperMalloc: invalid pointer or double free\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    abort();
#endif
}

static size_t block_size(const Block *block)
{
    return block->word & ~(size_t)(HM_ALIGNMENT - 1);
}

static int is_allocated(const Block *block)
{
    return (block->word & HM_FLAG_ALLOC) != 0;
}

static Block *next_block(const Block *block)
{
    return (Block *)((char *)block + block_size(block));
}

static FreeLinks *links(Block *block)
{
    return (FreeLinks *)((char *)block + sizeof(Block));
}

static void write_footer(Block *block)
{
    *(size_t *)((char *)block + block_size(block) - sizeof(size_t)) = block->word;
}

static int align_size(size_t value, size_t alignment, size_t *out)
{
    if (value > SIZE_MAX - (alignment - 1)) return 0;
    *out = (value + alignment - 1) & ~(alignment - 1);
    return 1;
}

/* Classes 0..13 cover 48..256 in 16-byte steps. Higher classes cover
 * quarters of each doubling range. Class 63 catches the rare oversized tail. */
static size_t size_class(size_t size)
{
    if (size <= 256) {
        if (size <= HM_MIN_BLOCK) return 0;
        return ((size - 1) / HM_ALIGNMENT) - 2;
    }
    size_t index = 14;
    size_t base = 256;
    while (base <= SIZE_MAX / 2) {
        size_t step = base / 4;
        for (size_t quarter = 1; quarter <= 4; ++quarter) {
            if (size <= base + quarter * step) return index;
            if (index == HM_CLASS_COUNT - 1) return index;
            ++index;
        }
        base *= 2;
    }
    return HM_CLASS_COUNT - 1;
}

static void insert_free(Block *block)
{
    size_t cls = size_class(block_size(block));
    FreeLinks *item = links(block);
    item->prev = NULL;
    item->next = free_lists[cls];
    if (item->next != NULL) links(item->next)->prev = block;
    free_lists[cls] = block;
    free_bitmap |= UINT64_C(1) << cls;
}

static void remove_free(Block *block)
{
    size_t cls = size_class(block_size(block));
    FreeLinks *item = links(block);
    if (item->prev != NULL) links(item->prev)->next = item->next;
    else free_lists[cls] = item->next;
    if (item->next != NULL) links(item->next)->prev = item->prev;
    if (free_lists[cls] == NULL) free_bitmap &= ~(UINT64_C(1) << cls);
    item->prev = NULL;
    item->next = NULL;
}

static Block *find_fit(size_t needed)
{
    size_t cls = size_class(needed);
    uint64_t candidates = free_bitmap & (UINT64_MAX << cls);
    while (candidates != 0) {
        size_t selected = (size_t)__builtin_ctzll(candidates);
        for (Block *block = free_lists[selected]; block != NULL;
             block = links(block)->next) {
#if HM_STATS
            ++stats.search_steps;
#endif
            if (block_size(block) >= needed) return block;
        }
        candidates &= candidates - 1;
    }
    return NULL;
}

static void account_mapping_add(size_t bytes)
{
    stats.current_heap_bytes += bytes;
    if (stats.current_heap_bytes > stats.peak_heap_bytes)
        stats.peak_heap_bytes = stats.current_heap_bytes;
#if HM_STATS
    ++stats.mmaps;
#endif
}

static void account_alloc(Block *block)
{
    stats.current_live_bytes += block->requested;
    if (stats.current_live_bytes > stats.peak_live_bytes)
        stats.peak_live_bytes = stats.current_live_bytes;
    stats.internal_fragmentation_bytes +=
        block_size(block) - sizeof(Block) - block->requested;
}

static void account_free(Block *block)
{
    stats.current_live_bytes -= block->requested;
    stats.internal_fragmentation_bytes -=
        block_size(block) - sizeof(Block) - block->requested;
}

static Block *add_chunk(size_t needed)
{
    size_t bytes;
    if (needed > SIZE_MAX - sizeof(Chunk) - 2 * sizeof(Block)) return NULL;
    bytes = needed + sizeof(Chunk) + 2 * sizeof(Block);
    if (bytes < HM_CHUNK_BYTES) bytes = HM_CHUNK_BYTES;
    if (!align_size(bytes, HM_ALIGNMENT, &bytes)) return NULL;
    void *mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return NULL;

    Chunk *chunk = mapping;
    chunk->mapping_size = bytes;
    chunk->next = chunks;
    chunks = chunk;
    Block *prologue = (Block *)((char *)mapping + sizeof(Chunk));
    prologue->requested = 0;
    prologue->word = sizeof(Block) | HM_FLAG_ALLOC | HM_FLAG_PREV_ALLOC;
    chunk->first = (Block *)((char *)prologue + sizeof(Block));
    chunk->epilogue = (Block *)((char *)mapping + bytes - sizeof(Block));
    Block *first = chunk->first;
    first->requested = 0;
    first->word = (size_t)((char *)chunk->epilogue - (char *)first) |
                  HM_FLAG_PREV_ALLOC;
    write_footer(first);
    chunk->epilogue->requested = 0;
    chunk->epilogue->word = HM_FLAG_ALLOC;
    insert_free(first);
    account_mapping_add(bytes);
    return first;
}

static Block *allocate_from_free(Block *block, size_t needed, size_t requested)
{
    size_t original = block_size(block);
    size_t prev_flag = block->word & HM_FLAG_PREV_ALLOC;
    remove_free(block);
    if (original - needed >= HM_MIN_BLOCK) {
        block->word = needed | prev_flag | HM_FLAG_ALLOC;
        Block *tail = next_block(block);
        tail->requested = 0;
        tail->word = (original - needed) | HM_FLAG_PREV_ALLOC;
        write_footer(tail);
        next_block(tail)->word &= ~(size_t)HM_FLAG_PREV_ALLOC;
        insert_free(tail);
#if HM_STATS
        ++stats.splits;
#endif
    } else {
        block->word = original | prev_flag | HM_FLAG_ALLOC;
        next_block(block)->word |= HM_FLAG_PREV_ALLOC;
    }
    block->requested = requested;
    account_alloc(block);
    return block;
}

static void *allocate_direct(size_t requested, size_t alignment)
{
    size_t effective = requested == 0 ? 1 : requested;
    size_t prefix = sizeof(DirectMap) + sizeof(Block);
    if (effective > SIZE_MAX - prefix ||
        effective + prefix > SIZE_MAX - (alignment - 1)) return NULL;
    size_t bytes;
    if (!align_size(effective + prefix + alignment - 1,
                    (size_t)getpagesize(), &bytes))
        return NULL;
    void *mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return NULL;
    DirectMap *item = mapping;
    uintptr_t start = (uintptr_t)mapping + prefix;
    uintptr_t aligned = (start + alignment - 1) & ~(uintptr_t)(alignment - 1);
    Block *block = (Block *)(aligned - sizeof(Block));
    size_t block_bytes = ((uintptr_t)mapping + bytes - (uintptr_t)block) &
                         ~(size_t)(HM_ALIGNMENT - 1);
    block->requested = effective;
    block->word = block_bytes | HM_FLAG_ALLOC | HM_FLAG_PREV_ALLOC | HM_FLAG_MMAP;
    item->mapping = mapping;
    item->mapping_size = bytes;
    item->payload = (void *)aligned;
    item->alignment = alignment;
    item->next = direct_maps;
    direct_maps = item;
    account_mapping_add(bytes);
    account_alloc(block);
    return item->payload;
}

static void *malloc_locked(size_t requested, size_t alignment)
{
    size_t effective = requested == 0 ? 1 : requested;
    if (effective >= HM_LARGE_THRESHOLD || alignment > HM_ALIGNMENT)
        return allocate_direct(effective, alignment);
    if (effective > SIZE_MAX - sizeof(Block)) return NULL;
    size_t needed;
    if (!align_size(effective + sizeof(Block), HM_ALIGNMENT, &needed))
        return NULL;
    if (needed < HM_MIN_BLOCK) needed = HM_MIN_BLOCK;
    Block *block = find_fit(needed);
    if (block == NULL) {
        if (add_chunk(needed) == NULL) return NULL;
        block = find_fit(needed);
    }
    if (block == NULL) return NULL;
    return (char *)allocate_from_free(block, needed, effective) + sizeof(Block);
}

static DirectMap *lookup_direct(void *ptr, DirectMap **previous)
{
    DirectMap *prev = NULL;
    for (DirectMap *item = direct_maps; item != NULL; item = item->next) {
        if (item->payload == ptr) {
            if (previous != NULL) *previous = prev;
            return item;
        }
        prev = item;
    }
    return NULL;
}

/* Region membership is checked before dereferencing the candidate header.
 * A forged aligned interior pointer can still mimic metadata: invalid frees
 * remain best-effort detection, as required by the public contract. */
static Block *lookup_chunk_block(void *ptr)
{
    uintptr_t address = (uintptr_t)ptr;
    if ((address & (HM_ALIGNMENT - 1)) != 0) return NULL;
    for (Chunk *chunk = chunks; chunk != NULL; chunk = chunk->next) {
        if (address < (uintptr_t)chunk->first + sizeof(Block) ||
            address >= (uintptr_t)chunk->epilogue) continue;
        Block *candidate = (Block *)(address - sizeof(Block));
        if ((uintptr_t)candidate < (uintptr_t)chunk->first ||
            block_size(candidate) < HM_MIN_BLOCK ||
            (uintptr_t)candidate + block_size(candidate) >
                (uintptr_t)chunk->epilogue) return NULL;
        if (candidate->requested > block_size(candidate) - sizeof(Block))
            return NULL;
        Block *next = next_block(candidate);
        if (!!(next->word & HM_FLAG_PREV_ALLOC) != is_allocated(candidate))
            return NULL;
        return candidate;
    }
    return NULL;
}

static void merge_and_insert(Block *block)
{
    size_t total = block_size(block);
    size_t prev_flag = block->word & HM_FLAG_PREV_ALLOC;
    if (prev_flag == 0) {
        size_t previous_word = *(size_t *)((char *)block - sizeof(size_t));
        size_t previous_size = previous_word & ~(size_t)(HM_ALIGNMENT - 1);
        Block *previous = (Block *)((char *)block - previous_size);
        remove_free(previous);
        block = previous;
        total += previous_size;
        prev_flag = block->word & HM_FLAG_PREV_ALLOC;
#if HM_STATS
        ++stats.coalesces;
#endif
    }
    Block *next = (Block *)((char *)block + total);
    if (!is_allocated(next)) {
        remove_free(next);
        total += block_size(next);
#if HM_STATS
        ++stats.coalesces;
#endif
    }
    block->requested = 0;
    block->word = total | prev_flag;
    write_footer(block);
    next_block(block)->word &= ~(size_t)HM_FLAG_PREV_ALLOC;
    insert_free(block);
}

static int free_locked(void *ptr)
{
    if (ptr == NULL) return 1;
    DirectMap *previous = NULL;
    DirectMap *direct = lookup_direct(ptr, &previous);
    if (direct != NULL) {
        Block *block = (Block *)((char *)ptr - sizeof(Block));
        account_free(block);
        if (previous != NULL) previous->next = direct->next;
        else direct_maps = direct->next;
        stats.current_heap_bytes -= direct->mapping_size;
        (void)munmap(direct->mapping, direct->mapping_size);
        return 1;
    }
    Block *block = lookup_chunk_block(ptr);
    if (block == NULL || !is_allocated(block)) {
        invalid_use();
        return 0;
    }
    account_free(block);
    block->word &= ~(size_t)HM_FLAG_ALLOC;
    merge_and_insert(block);
    return 1;
}

static int check_locked(void)
{
    size_t heap_bytes = 0;
    size_t live_bytes = 0;
    size_t fragmented = 0;
    size_t physical_free = 0;
    size_t max_blocks = 0;
    for (Chunk *chunk = chunks; chunk != NULL; chunk = chunk->next) {
        if (chunk->mapping_size < HM_MIN_BLOCK + 2 * sizeof(Block) +
                                      sizeof(Chunk)) return 1;
        heap_bytes += chunk->mapping_size;
        size_t sum = 0;
        int previous_allocated = 1;
        for (Block *block = chunk->first; block < chunk->epilogue;) {
            size_t size = block_size(block);
            if (size < HM_MIN_BLOCK || size % HM_ALIGNMENT != 0 ||
                (uintptr_t)block + size > (uintptr_t)chunk->epilogue)
                return 2;
            if (!!(block->word & HM_FLAG_PREV_ALLOC) != previous_allocated)
                return 3;
            if (is_allocated(block)) {
                if (block->requested == 0 ||
                    block->requested > size - sizeof(Block)) return 4;
                live_bytes += block->requested;
                fragmented += size - sizeof(Block) - block->requested;
            } else {
                if (!previous_allocated ||
                    *(size_t *)((char *)block + size - sizeof(size_t)) !=
                        block->word) return 5;
                ++physical_free;
            }
            previous_allocated = is_allocated(block);
            sum += size;
            ++max_blocks;
            block = (Block *)((char *)block + size);
        }
        if (sum != (size_t)((char *)chunk->epilogue - (char *)chunk->first) ||
            block_size(chunk->epilogue) != 0 ||
            !is_allocated(chunk->epilogue) ||
            !!(chunk->epilogue->word & HM_FLAG_PREV_ALLOC) !=
                previous_allocated) return 6;
    }
    size_t listed_free = 0;
    uint64_t observed_bitmap = 0;
    for (size_t cls = 0; cls < HM_CLASS_COUNT; ++cls) {
        Block *previous = NULL;
        for (Block *block = free_lists[cls]; block != NULL;) {
            if (++listed_free > max_blocks || !lookup_chunk_block(
                    (char *)block + sizeof(Block))) return 7;
            if (is_allocated(block) || size_class(block_size(block)) != cls ||
                links(block)->prev != previous) return 8;
            observed_bitmap |= UINT64_C(1) << cls;
            previous = block;
            block = links(block)->next;
        }
    }
    if (listed_free != physical_free || observed_bitmap != free_bitmap)
        return 9;
    size_t direct_count = 0;
    for (DirectMap *item = direct_maps; item != NULL; item = item->next) {
        if (++direct_count > stats.current_heap_bytes / HM_MIN_BLOCK ||
            (uintptr_t)item->payload % item->alignment != 0 ||
            (uintptr_t)item->payload < (uintptr_t)item->mapping +
                                            sizeof(DirectMap) + sizeof(Block) ||
            (uintptr_t)item->payload >=
                (uintptr_t)item->mapping + item->mapping_size) return 10;
        Block *block = (Block *)((char *)item->payload - sizeof(Block));
        if (!is_allocated(block) || !(block->word & HM_FLAG_MMAP) ||
            block->requested > block_size(block) - sizeof(Block)) return 11;
        heap_bytes += item->mapping_size;
        live_bytes += block->requested;
        fragmented += block_size(block) - sizeof(Block) - block->requested;
    }
    if (heap_bytes != stats.current_heap_bytes ||
        live_bytes != stats.current_live_bytes ||
        fragmented != stats.internal_fragmentation_bytes) return 12;
    return 0;
}

static void debug_check(void)
{
#if HM_DEBUG
    if (++debug_operations % HM_DEBUG_STRIDE == 0 && check_locked() != 0) {
        static const char message[] = "HyperMalloc: heap invariant failure\n";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        abort();
    }
#endif
}

void *hm_malloc(size_t size)
{
    int saved_errno = errno;
    lock_heap();
    void *result = malloc_locked(size, HM_ALIGNMENT);
#if HM_STATS
    ++stats.mallocs;
#endif
    debug_check();
    unlock_heap();
    if (result == NULL) errno = ENOMEM;
    else errno = saved_errno;
    return result;
}

void *hm_memalign(size_t alignment, size_t size)
{
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0) {
        errno = EINVAL;
        return NULL;
    }
    if (alignment < HM_ALIGNMENT) alignment = HM_ALIGNMENT;
    int saved_errno = errno;
    lock_heap();
    void *result = malloc_locked(size, alignment);
#if HM_STATS
    ++stats.mallocs;
#endif
    debug_check();
    unlock_heap();
    if (result == NULL) errno = ENOMEM;
    else errno = saved_errno;
    return result;
}

void hm_free(void *ptr)
{
    int saved_errno = errno;
    lock_heap();
    if (ptr != NULL) {
        if (free_locked(ptr)) {
#if HM_STATS
            ++stats.frees;
#endif
        }
    }
    debug_check();
    unlock_heap();
    errno = saved_errno;
}

void *hm_calloc(size_t nmemb, size_t size)
{
    if (size != 0 && nmemb > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    size_t bytes = nmemb * size;
    void *result = hm_malloc(bytes);
    if (result != NULL) memset(result, 0, bytes);
    return result;
}

void *hm_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) return hm_malloc(size);
    if (size == 0) {
        hm_free(ptr);
        return NULL;
    }
    int saved_errno = errno;
    lock_heap();
#if HM_STATS
    ++stats.reallocs;
#endif
    DirectMap *direct = lookup_direct(ptr, NULL);
    Block *block = direct != NULL ?
        (Block *)((char *)ptr - sizeof(Block)) : lookup_chunk_block(ptr);
    if (block == NULL || !is_allocated(block)) {
        invalid_use();
        unlock_heap();
        errno = EINVAL;
        return NULL;
    }
    size_t old_requested = block->requested;
    size_t old_capacity = block_size(block) - sizeof(Block);
    size_t needed;
    if (size > SIZE_MAX - sizeof(Block) ||
        !align_size(size + sizeof(Block), HM_ALIGNMENT, &needed)) {
        unlock_heap();
        errno = ENOMEM;
        return NULL;
    }
    if (needed < HM_MIN_BLOCK) needed = HM_MIN_BLOCK;
    if (direct == NULL && size < HM_LARGE_THRESHOLD) {
        if (needed <= block_size(block)) {
            size_t original = block_size(block);
            if (original - needed >= HM_MIN_BLOCK) {
                block->word = needed | (block->word & HM_FLAG_PREV_ALLOC) |
                              HM_FLAG_ALLOC;
                Block *tail = next_block(block);
                tail->requested = 0;
                tail->word = (original - needed) | HM_FLAG_PREV_ALLOC;
                write_footer(tail);
                next_block(tail)->word &= ~(size_t)HM_FLAG_PREV_ALLOC;
                merge_and_insert(tail);
#if HM_STATS
                ++stats.splits;
#endif
            }
            stats.current_live_bytes -= old_requested;
            stats.current_live_bytes += size;
            stats.internal_fragmentation_bytes -= old_capacity - old_requested;
            block->requested = size;
            stats.internal_fragmentation_bytes +=
                block_size(block) - sizeof(Block) - size;
            if (stats.current_live_bytes > stats.peak_live_bytes)
                stats.peak_live_bytes = stats.current_live_bytes;
            debug_check();
            unlock_heap();
            errno = saved_errno;
            return ptr;
        }
        Block *next = next_block(block);
        if (!is_allocated(next) && block_size(block) + block_size(next) >=
                                       needed) {
            remove_free(next);
            size_t joined = block_size(block) + block_size(next);
            block->word = joined | (block->word & HM_FLAG_PREV_ALLOC) |
                          HM_FLAG_ALLOC;
            next_block(block)->word |= HM_FLAG_PREV_ALLOC;
#if HM_STATS
            ++stats.coalesces;
#endif
            if (joined - needed >= HM_MIN_BLOCK) {
                block->word = needed | (block->word & HM_FLAG_PREV_ALLOC) |
                              HM_FLAG_ALLOC;
                Block *tail = next_block(block);
                tail->requested = 0;
                tail->word = (joined - needed) | HM_FLAG_PREV_ALLOC;
                write_footer(tail);
                next_block(tail)->word &= ~(size_t)HM_FLAG_PREV_ALLOC;
                insert_free(tail);
#if HM_STATS
                ++stats.splits;
#endif
            }
            stats.current_live_bytes -= old_requested;
            stats.current_live_bytes += size;
            stats.internal_fragmentation_bytes -= old_capacity - old_requested;
            block->requested = size;
            stats.internal_fragmentation_bytes +=
                block_size(block) - sizeof(Block) - size;
            if (stats.current_live_bytes > stats.peak_live_bytes)
                stats.peak_live_bytes = stats.current_live_bytes;
            debug_check();
            unlock_heap();
            errno = saved_errno;
            return ptr;
        }
    }
    size_t alignment = direct != NULL ? direct->alignment : HM_ALIGNMENT;
    void *replacement = malloc_locked(size, alignment);
    if (replacement == NULL) {
        unlock_heap();
        errno = ENOMEM;
        return NULL;
    }
    memcpy(replacement, ptr, old_requested < size ? old_requested : size);
    (void)free_locked(ptr);
    debug_check();
    unlock_heap();
    errno = saved_errno;
    return replacement;
}

size_t hm_usable_size(void *ptr)
{
    lock_heap();
    DirectMap *direct = lookup_direct(ptr, NULL);
    Block *block = direct != NULL ?
        (Block *)((char *)ptr - sizeof(Block)) : lookup_chunk_block(ptr);
    size_t result = block != NULL && is_allocated(block) ?
        block_size(block) - sizeof(Block) : 0;
    unlock_heap();
    return result;
}

int hm_check_heap(void)
{
    lock_heap();
    int result = check_locked();
    unlock_heap();
    return result;
}

void hm_get_stats(hm_stats *out)
{
    if (out == NULL) return;
    lock_heap();
    *out = stats;
    unlock_heap();
}

/* Compatibility API retained for the repository's existing clients. */
void *hyper_malloc(size_t size) { return size == 0 ? NULL : hm_malloc(size); }
void hyper_free(void *ptr) { hm_free(ptr); }
void *hyper_calloc(size_t count, size_t size) { return hm_calloc(count, size); }
void *hyper_realloc(void *ptr, size_t size) { return hm_realloc(ptr, size); }
void *hyper_reallocarray(void *ptr, size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    return hm_realloc(ptr, count * size);
}
void *hyper_aligned_alloc(size_t alignment, size_t size)
{
    if (alignment == 0 || size % alignment != 0) {
        errno = EINVAL;
        return NULL;
    }
    return hm_memalign(alignment, size);
}
int hyper_posix_memalign(void **memptr, size_t alignment, size_t size)
{
    if (memptr == NULL || alignment < sizeof(void *) ||
        (alignment & (alignment - 1)) != 0) return EINVAL;
    void *result = hm_memalign(alignment, size);
    if (result == NULL) return errno == EINVAL ? EINVAL : ENOMEM;
    *memptr = result;
    return 0;
}
size_t hyper_malloc_usable_size(void *ptr) { return hm_usable_size(ptr); }
int check_heap(void) { return hm_check_heap() == 0; }
