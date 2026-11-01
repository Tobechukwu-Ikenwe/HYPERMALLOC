/*
 * HyperMalloc's allocation flow
 * =============================
 *
 * A caller receives a payload pointer, never a pointer to our metadata.
 * Every payload has a 16-byte Block header immediately before it. "Block"
 * means one allocation-sized piece; "Chunk" means one mmap region containing
 * many ordinary blocks. A direct mapping is one mmap region for one request.
 * mmap obtains virtual address space from Linux; munmap returns a whole region.
 *
 * A prologue and epilogue are special allocated markers at a chunk's ends;
 * they are boundaries, not allocations returned to callers. A free block has
 * a footer (a copy of its size/flags word) so its next physical neighbor can
 * find it when joining adjacent free blocks.
 *
 * A size class groups free blocks by size. free_lists[i] points to the first
 * block in class i; bit i of free_bitmap says whether that list is nonempty.
 * A hash bucket has a different job: it indexes live direct maps by payload
 * pointer, so free/realloc need not scan every direct allocation.
 *
 * For example, hm_malloc(80) needs 80 payload bytes + a 16-byte header = a
 * 96-byte block. On an empty heap, the allocator maps one 256 KiB chunk,
 * splits 96 bytes from its large initial free block, and lists the remainder
 * for reuse. The returned pointer is 16 bytes into the 96-byte block.
 *
 * hm_malloc(size)
 *   1. Choose this thread's home arena and lock its metadata.
 *   2. Use a direct mapping for size >= 128 KiB. The same allocation helper
 *      also takes this path for hm_memalign alignment > 16 bytes. Otherwise,
 *      round up header + payload, then search free size classes.
 *   3. If no free block fits, mmap a chunk and put its initial free block on
 *      a free list. Remove a fitting block, split a usable remainder, record
 *      the requested size, and return the address after its Block header.
 *   4. Update statistics, optionally check invariants, and unlock.
 *
 * hm_free(ptr)
 *   1. Find an exact direct-map payload in a hash bucket, or find the Block
 *      header inside a known chunk. Hash buckets index live direct mappings;
 *      size-class free lists hold reusable ordinary blocks. They are distinct.
 *   2. A direct allocation is removed from its registries and munmapped.
 *      An ordinary block is marked free, joined with physically adjacent free
 *      blocks, and placed on the free list for its new size. An entirely free
 *      chunk may be unmapped; each arena retains at most one empty chunk.
 *
 * For an ordinary block, hm_realloc first tries to keep the pointer by
 * shrinking or growing into a free next block. Otherwise it allocates
 * replacement space, copies the requested data, and frees the old allocation.
 * The checker independently walks all metadata to verify consistency.
 *
 * An arena is an independent set of chunks and free lists with one mutex.
 * Threads use a home arena for allocation; freeing a pointer from another
 * thread probes arenas to find its owner. Most functions ending in _locked
 * require the selected arena's mutex; check_all_locked requires every arena
 * mutex. This file
 * uses size_t for byte counts, char * for byte-by-byte address arithmetic,
 * and uintptr_t when an address must be treated as an integer for alignment
 * or hashing. File-scope static names are private to this source file.
 * C reading guide: T *p declares a pointer to T; p->field accesses a field
 * through that pointer; NULL means no pointer; sizeof(T) measures bytes.
 * Bitwise | combines flags, & tests selected bits, and ~ inverts a mask.
 * #if/#endif select code at compile time, not while the program is running.
 */
#define _GNU_SOURCE
#include "hypermalloc.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef HM_DEBUG
#define HM_DEBUG 0
#endif
#ifndef HM_STATS
#define HM_STATS 1
#endif
#ifndef HM_DEBUG_STRIDE
#define HM_DEBUG_STRIDE 1
#endif

/* Block sizes are multiples of 16, leaving the low four bits of word free
 * for flags. For a 96-byte live block with a live predecessor, word is
 * 96 | 1 | 2 = 99; block_size masks off the flags to recover 96. Bit 1
 * avoids reading a footer when the predecessor is allocated and has none.
 * A free block needs 16 header + 16 links + 8 footer = 40 bytes, rounded to
 * 48 for alignment. These values are layout/policy choices, not OS page sizes. */
enum {
    HM_ALIGNMENT = 16,          /* malloc-compatible payload alignment */
    HM_MIN_BLOCK = 48,          /* header + two free links + footer, rounded */
    HM_CHUNK_BYTES = 256 * 1024, /* usual shared mmap region size */
    HM_LARGE_THRESHOLD = 128 * 1024, /* direct-map at half a usual chunk */
    HM_CLASS_COUNT = 64,        /* one class per bit of free_bitmap */
    HM_DIRECT_BUCKETS = 1024,   /* power of two for masked hash indices */
    HM_ARENA_CAPACITY = 8,      /* measured eight-thread default and maximum */
    HM_FLAG_ALLOC = 1,          /* bit 0: this block is live */
    HM_FLAG_PREV_ALLOC = 2,     /* bit 1: physical predecessor is live */
    HM_FLAG_MMAP = 4            /* bit 2: block belongs to a direct map */
};

#ifndef HM_ACTIVE_ARENAS
#define HM_ACTIVE_ARENAS HM_ARENA_CAPACITY
#endif
_Static_assert(HM_ACTIVE_ARENAS >= 1 && HM_ACTIVE_ARENAS <= HM_ARENA_CAPACITY,
               "HM_ACTIVE_ARENAS must be between 1 and 8");

/* A block begins with this header; its payload starts at
 * (char *)block + sizeof(Block). Casting to char * makes the addition count
 * bytes rather than whole Block objects.
 * Free blocks also have FreeLinks in the payload and a footer at their end. */
typedef struct Block {
    size_t requested; /* caller's bytes (one for size zero); zero when free */
    size_t word;      /* total block bytes (header included) OR-ed with flags */
} Block;

/* These live in a free block's former payload, not in a separate allocation.
 * They link blocks in the same size class, not physical neighbors in a chunk.
 * Two links permit unlinking a known block without searching its list. */
typedef struct FreeLinks {
    Block *prev; /* preceding free-list node, or NULL for the list head */
    Block *next; /* following free-list node, or NULL for the list tail */
} FreeLinks;

/* A chunk is one shared mmap region. The descriptor sits at its beginning;
 * ordinary blocks occupy the span from first to (but not including) epilogue. */
typedef struct Chunk {
    struct Chunk *next; /* next chunk in registry, not adjacent in memory */
    size_t mapping_size; /* total mmap bytes; also used for munmap */
    Block *first;        /* first ordinary block, after the prologue */
    Block *epilogue;     /* allocated size-zero end marker, after all blocks */
} Chunk;

/* Fail compilation if the C implementation cannot support this layout.
 * sizeof measures bytes; _Alignof gives the alignment a type requires. */
_Static_assert(sizeof(void *) == 8, "HyperMalloc requires a 64-bit ABI");
_Static_assert(sizeof(Block) == 16, "unexpected block header layout");
_Static_assert(sizeof(Chunk) == 32, "unexpected chunk layout");
_Static_assert(_Alignof(max_align_t) <= HM_ALIGNMENT,
               "malloc alignment exceeds allocator alignment");

/* One independently mapped allocation. This descriptor lives at mapping.
 * payload may be farther in, after the descriptor, padding, and Block header. */
typedef struct DirectMap {
    struct DirectMap *next; /* next entry in the full direct-map registry */
    struct DirectMap *prev; /* previous registry entry for constant-time unlink */
    struct DirectMap *hash_next; /* next entry sharing a lookup bucket */
    void *mapping;      /* original mmap address passed later to munmap */
    size_t mapping_size;/* full page-rounded mmap length */
    void *payload;      /* aligned pointer returned to the caller */
    size_t alignment;   /* alignment to preserve if realloc moves the data */
} DirectMap;

/* A checker error is collected without allocating memory for a report. */
typedef struct CheckFailure {
    int code;            /* which invariant failed; zero means no failure */
    const void *address; /* metadata address relevant to that failure */
    size_t expected;     /* expected value, printed in hexadecimal */
    size_t actual;       /* observed value, printed in hexadecimal */
} CheckFailure;

/* Each arena owns its chunks, direct maps, free lists, and accounting. Its
 * mutex protects all of them. Threads select a home arena once, while free
 * and realloc locate the arena that actually owns a pointer. */
typedef struct HeapState {
    Chunk *chunks; /* shared chunk mappings owned by this arena */
    DirectMap *direct_maps; /* its full direct-map registry */
    DirectMap *direct_buckets[HM_DIRECT_BUCKETS]; /* exact pointer lookup */
    Block *free_lists[HM_CLASS_COUNT]; /* reusable blocks by size class */
    uint64_t free_bitmap; /* nonempty free-list classes */
    hm_stats counters; /* this arena's memory accounting and operation counts */
#if HM_DEBUG
    uint64_t debug_operations; /* operations since this arena's debug checks */
#endif
    pthread_mutex_t mutex; /* serializes changes within this arena */
} HeapState;

/* Explicit static mutex initialization works before constructors and before
 * main(), which matters when this allocator is preloaded. */
static HeapState heaps[HM_ARENA_CAPACITY] = {
    {.mutex = PTHREAD_MUTEX_INITIALIZER},
    {.mutex = PTHREAD_MUTEX_INITIALIZER},
    {.mutex = PTHREAD_MUTEX_INITIALIZER},
    {.mutex = PTHREAD_MUTEX_INITIALIZER},
    {.mutex = PTHREAD_MUTEX_INITIALIZER},
    {.mutex = PTHREAD_MUTEX_INITIALIZER},
    {.mutex = PTHREAD_MUTEX_INITIALIZER},
    {.mutex = PTHREAD_MUTEX_INITIALIZER},
};
static atomic_uint next_home; /* round-robin arena assignment for new threads */
static _Thread_local HeapState *home_heap; /* this thread's allocation arena */
static _Thread_local HeapState *active_heap; /* arena used by current operation */
static atomic_size_t global_live_bytes; /* current requested bytes, all arenas */
static atomic_size_t global_heap_bytes; /* current mapped bytes, all arenas */
static atomic_size_t global_peak_live_bytes; /* process-wide live high-water */
static atomic_size_t global_peak_heap_bytes; /* process-wide mapped high-water */

/* Existing helpers operate on the arena selected by the public entry point.
 * active_heap is thread-local, so another thread cannot change this choice. */
#define chunks (active_heap->chunks)
#define direct_maps (active_heap->direct_maps)
#define direct_buckets (active_heap->direct_buckets)
#define free_lists (active_heap->free_lists)
#define free_bitmap (active_heap->free_bitmap)
#define stats (active_heap->counters)
#if HM_DEBUG
#define debug_operations (active_heap->debug_operations)
#endif
#define heap_mutex (active_heap->mutex)

static HeapState *choose_home_heap(void)
{
    if (home_heap == NULL) {
        unsigned index = atomic_fetch_add_explicit(&next_home, 1,
                                                    memory_order_relaxed);
        home_heap = &heaps[index % HM_ACTIVE_ARENAS];
    }
    return home_heap;
}

static void lock_all_heaps(void)
{
    for (size_t i = 0; i < HM_ACTIVE_ARENAS; ++i)
        (void)pthread_mutex_lock(&heaps[i].mutex);
}

static void unlock_all_heaps(void)
{
    for (size_t i = HM_ACTIVE_ARENAS; i > 0; --i)
        (void)pthread_mutex_unlock(&heaps[i - 1].mutex);
}

/* active_heap must be selected before these are called. */
static void lock_heap(void)
{
    (void)pthread_mutex_lock(&heap_mutex);
}

static void unlock_heap(void)
{
    (void)pthread_mutex_unlock(&heap_mutex);
}

/* Lock every arena before fork so the child cannot inherit an arena mutex
 * held by a thread that does not survive the fork. */
__attribute__((constructor)) static void register_fork_handlers(void)
{
    (void)pthread_atfork(lock_all_heaps, unlock_all_heaps, unlock_all_heaps);
}

static void invalid_use(void)
{
#if HM_DEBUG
    /* stdio could allocate while the allocator is broken, so use write(2). */
    static const char message[] = "HyperMalloc: invalid pointer or double free\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    abort();
#endif
}

/* The lower four bits are flags; a 16-byte multiple has zeros there. */
static size_t block_size(const Block *block)
{
    return block->word & ~(size_t)(HM_ALIGNMENT - 1);
}

static int is_allocated(const Block *block)
{
    return (block->word & HM_FLAG_ALLOC) != 0;
}

/* Advance in physical address order, unrelated to FreeLinks.next. */
static Block *next_block(const Block *block)
{
    return (Block *)((char *)block + block_size(block));
}

/* A free block's payload is safe to reuse for list pointers. */
static FreeLinks *links(Block *block)
{
    return (FreeLinks *)((char *)block + sizeof(Block));
}

/* Only free blocks need a footer. If the next block says its predecessor is
 * free, this footer supplies that predecessor's size for O(1) coalescing. */
static void write_footer(Block *block)
{
    *(size_t *)((char *)block + block_size(block) - sizeof(size_t)) = block->word;
}

/* Round up a byte count to a power-of-two boundary; reject overflow before
 * adding alignment - 1. Return 1 on success, 0 on overflow. */
static int align_size(size_t value, size_t alignment, size_t *out)
{
    if (value > SIZE_MAX - (alignment - 1)) return 0;
    *out = (value + alignment - 1) & ~(alignment - 1);
    return 1;
}

/* Classes 0..13 cover 48..256 in 16-byte steps, which keeps small-block
 * searches narrow. Larger classes cover quarters of each doubling range;
 * class 63 catches anything beyond the represented ranges. */
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

/* LIFO insertion is cheap: only the list head and its old first node change.
 * A bitmap bit means find_fit need not visit an empty class. */
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

/* The block itself contains both list links, so removal needs no list scan.
 * This is important when free must unlink neighboring blocks to coalesce. */
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

/* Look only at this size class and larger ones. ctzll finds the index of the
 * lowest set bit, i.e. the next nonempty eligible class. Scan its nodes for
 * the first actual fit because one class can contain several block sizes. */
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

/* Per-arena counters are checked against that arena's physical blocks. The
 * atomic process totals maintain true peaks across concurrent arenas. */
static void update_peak(atomic_size_t *peak, size_t value)
{
    size_t observed = atomic_load_explicit(peak, memory_order_relaxed);
    while (observed < value &&
           !atomic_compare_exchange_weak_explicit(peak, &observed, value,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {}
}

static void global_add_live(size_t bytes)
{
    size_t total = atomic_fetch_add_explicit(&global_live_bytes, bytes,
                                             memory_order_relaxed) + bytes;
    update_peak(&global_peak_live_bytes, total);
}

static void global_sub_live(size_t bytes)
{
    (void)atomic_fetch_sub_explicit(&global_live_bytes, bytes,
                                    memory_order_relaxed);
}

static void global_sub_mapping(size_t bytes)
{
    (void)atomic_fetch_sub_explicit(&global_heap_bytes, bytes,
                                    memory_order_relaxed);
}

/* Track mapped virtual bytes, not resident physical memory. Operation counts
 * can be disabled with HM_STATS; memory totals stay on for the heap checker. */
static void account_mapping_add(size_t bytes)
{
    stats.current_heap_bytes += bytes;
    if (stats.current_heap_bytes > stats.peak_heap_bytes)
        stats.peak_heap_bytes = stats.current_heap_bytes;
    size_t total = atomic_fetch_add_explicit(&global_heap_bytes, bytes,
                                             memory_order_relaxed) + bytes;
    update_peak(&global_peak_heap_bytes, total);
#if HM_STATS
    ++stats.mmaps;
#endif
}

/* Internal fragmentation is usable block capacity minus requested bytes.
 * It includes rounding and, for direct maps, trailing page-rounded space. */
static void account_alloc(Block *block)
{
    stats.current_live_bytes += block->requested;
    global_add_live(block->requested);
    if (stats.current_live_bytes > stats.peak_live_bytes)
        stats.peak_live_bytes = stats.current_live_bytes;
    stats.internal_fragmentation_bytes +=
        block_size(block) - sizeof(Block) - block->requested;
}

static void account_free(Block *block)
{
    stats.current_live_bytes -= block->requested;
    global_sub_live(block->requested);
    stats.internal_fragmentation_bytes -=
        block_size(block) - sizeof(Block) - block->requested;
}

/* A direct map is indexed by its exact returned payload pointer. Drop the
 * always-zero 16-byte alignment bits, mix address bits, then mask to 1024
 * buckets. Collisions form short hash_next chains and are checked exactly. */
static size_t direct_bucket(const void *payload)
{
    uintptr_t value = (uintptr_t)payload >> 4;
    value ^= value >> 17;
    value ^= value >> 9;
    return (size_t)(value & (HM_DIRECT_BUCKETS - 1));
}

/* Create a shared region, normally 256 KiB. The layout is:
 * [Chunk:32][allocated prologue:16][free blocks...][epilogue:16].
 * The sentinels keep neighbor checks inside the mapping. The requested
 * block may require a larger-than-usual chunk, hence the needed argument. */
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
    /* Start with one free block spanning all ordinary block space. Its
     * predecessor is the allocated prologue; its footer enables coalescing. */
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

/* Consume a free block chosen by find_fit. Split only when the remainder can
 * itself hold a header, two list links, and a footer (HM_MIN_BLOCK). */
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
        /* The block after tail (possibly the epilogue) now has a free
         * predecessor, so its PREV_ALLOC bit must be cleared. */
        next_block(tail)->word &= ~(size_t)HM_FLAG_PREV_ALLOC;
        insert_free(tail);
#if HM_STATS
        ++stats.splits;
#endif
    } else {
        /* Giving the caller the whole block avoids an unusable tiny tail. */
        block->word = original | prev_flag | HM_FLAG_ALLOC;
        next_block(block)->word |= HM_FLAG_PREV_ALLOC;
    }
    block->requested = requested;
    account_alloc(block);
    return block;
}

/* One mmap per large or specially aligned request. Unlike a shared chunk,
 * this region can be returned to Linux as soon as its one allocation dies.
 * alignment - 1 reserves enough slack to position the payload correctly. */
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
    /* Keep the Block header immediately before the aligned user pointer;
     * the DirectMap descriptor remains at the original mmap address. */
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
    item->prev = NULL;
    /* The full doubly linked registry is for checker walks and O(1) unlink.
     * The hash bucket gives quick exact-payload lookup during free/realloc. */
    item->next = direct_maps;
    if (direct_maps != NULL) direct_maps->prev = item;
    direct_maps = item;
    size_t bucket = direct_bucket(item->payload);
    item->hash_next = direct_buckets[bucket];
    direct_buckets[bucket] = item;
    account_mapping_add(bytes);
    account_alloc(block);
    return item->payload;
}

/* Common allocation decision used by hm_malloc, hm_memalign, and realloc.
 * Zero-byte requests get one requested byte so the result is freeable. */
static void *malloc_locked(size_t requested, size_t alignment)
{
    size_t effective = requested == 0 ? 1 : requested;
    if (effective >= HM_LARGE_THRESHOLD || alignment > HM_ALIGNMENT)
        return allocate_direct(effective, alignment);
    /* Ordinary blocks include their 16-byte header in needed. */
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

/* Hash lookup still requires equality: sharing a bucket does not imply two
 * pointers identify the same allocation. previous receives the registry
 * predecessor when the free path needs to unlink that item. */
static DirectMap *lookup_direct(void *ptr, DirectMap **previous)
{
    for (DirectMap *item = direct_buckets[direct_bucket(ptr)]; item != NULL;
         item = item->hash_next) {
        if (item->payload == ptr) {
            if (previous != NULL) *previous = item->prev;
            return item;
        }
    }
    return NULL;
}

/* Ordinary payloads have their Block header immediately before the pointer.
 * Scan known chunks first so a random pointer is not dereferenced blindly.
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
            block_size(candidate) >
                (size_t)((char *)chunk->epilogue - (char *)candidate))
            return NULL;
        if (candidate->requested > block_size(candidate) - sizeof(Block))
            return NULL;
        Block *next = next_block(candidate);
        if (!!(next->word & HM_FLAG_PREV_ALLOC) != is_allocated(candidate))
            return NULL;
        return candidate;
    }
    return NULL;
}

/* Same-thread frees usually find the pointer in the caller's home arena.
 * Cross-thread frees probe other arenas in a fixed cyclic order. Return with
 * the owning arena locked and active_heap set, or return 0 with no lock held. */
static int lock_owner(void *ptr, Block **block_out, DirectMap **direct_out)
{
    size_t home = (size_t)(choose_home_heap() - heaps);
    for (size_t offset = 0; offset < HM_ACTIVE_ARENAS; ++offset) {
        active_heap = &heaps[(home + offset) % HM_ACTIVE_ARENAS];
        lock_heap();
        DirectMap *direct = lookup_direct(ptr, NULL);
        Block *block = direct != NULL ?
            (Block *)((char *)ptr - sizeof(Block)) : lookup_chunk_block(ptr);
        if (block != NULL) {
            if (block_out != NULL) *block_out = block;
            if (direct_out != NULL) *direct_out = direct;
            return 1;
        }
        unlock_heap();
    }
    return 0;
}

/* Coalesce in physical address order before putting the result on a size
 * class list. Free-list prev/next links are NOT physical neighbors. */
static void merge_and_insert(Block *block)
{
    size_t total = block_size(block);
    size_t prev_flag = block->word & HM_FLAG_PREV_ALLOC;
    if (prev_flag == 0) {
        /* Only a free predecessor has a footer. Its last word is immediately
         * before this block and tells us how far backward to step. */
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
    /* The following block now has a free physical predecessor. */
    next_block(block)->word &= ~(size_t)HM_FLAG_PREV_ALLOC;

    /* Keep one fully free chunk per arena for reuse, release further empty
     * chunks in the same arena.
     * Confirm both chunk boundaries before munmap; an internal free block
     * must never cause a live allocation's mapping to be released. */
    if (total >= HM_CHUNK_BYTES - sizeof(Chunk) - 2 * sizeof(Block)) {
        Chunk **owner = &chunks;
        while (*owner != NULL && (*owner)->first != block)
            owner = &(*owner)->next;
        if (*owner != NULL && next_block(block) == (*owner)->epilogue) {
            for (Chunk *other = chunks; other != NULL; other = other->next) {
                if (other != *owner && !is_allocated(other->first) &&
                    next_block(other->first) == other->epilogue) {
                    Chunk *released = *owner;
                    size_t bytes = released->mapping_size;
                    *owner = released->next;
                    stats.current_heap_bytes -= bytes;
                    global_sub_mapping(bytes);
                    (void)munmap(released, bytes);
                    return;
                }
            }
        }
    }
    insert_free(block);
}

/* The caller already found the owning arena and validated block while its
 * mutex is held. Reuse those results: looking up ptr again would repeat the
 * chunk-range scan or direct hash traversal on every free. Registry metadata
 * must be unlinked before its mmap is destroyed. */
static void free_locked(void *ptr, Block *block, DirectMap *direct)
{
    if (direct != NULL) {
        account_free(block);
        size_t bucket = direct_bucket(ptr);
        DirectMap **link = &direct_buckets[bucket];
        /* link points at whichever pointer currently names direct: either
         * the bucket head or a preceding entry's hash_next. */
        while (*link != direct) link = &(*link)->hash_next;
        *link = direct->hash_next;
        DirectMap *previous = direct->prev;
        if (previous != NULL) previous->next = direct->next;
        else direct_maps = direct->next;
        if (direct->next != NULL) direct->next->prev = previous;
        stats.current_heap_bytes -= direct->mapping_size;
        global_sub_mapping(direct->mapping_size);
        (void)munmap(direct->mapping, direct->mapping_size);
        return;
    }
    account_free(block);
    block->word &= ~(size_t)HM_FLAG_ALLOC;
    merge_and_insert(block);
}

/* Preserve the first failed invariant's details for the verbose API.
 * Passing NULL avoids report work during ordinary debug checks. */
static int fail_check(CheckFailure *failure, int code, const void *address,
                      size_t expected, size_t actual)
{
    if (failure != NULL) {
        failure->code = code;
        failure->address = address;
        failure->expected = expected;
        failure->actual = actual;
    }
    return code;
}

/* Independently reconstruct accounting by walking memory, rather than
 * trusting the free lists or stats we are trying to validate. Return zero
 * for a healthy heap or the first nonzero invariant code. */
static int check_locked(CheckFailure *failure)
{
    size_t heap_bytes = 0;
    size_t live_bytes = 0;
    size_t fragmented = 0;
    size_t physical_free = 0;
    size_t max_blocks = 0;
    /* Phase 1: walk each chunk in address order. Every step must be a valid
     * block and the final step must land exactly on its epilogue. */
    for (Chunk *chunk = chunks; chunk != NULL; chunk = chunk->next) {
        if (chunk->mapping_size < HM_MIN_BLOCK + 2 * sizeof(Block) +
                                      sizeof(Chunk))
            return fail_check(failure, 1, chunk, HM_MIN_BLOCK +
                              2 * sizeof(Block) + sizeof(Chunk),
                              chunk->mapping_size);
        heap_bytes += chunk->mapping_size;
        size_t sum = 0;
        int previous_allocated = 1;
        for (Block *block = chunk->first; block < chunk->epilogue;) {
            size_t size = block_size(block);
            if (size < HM_MIN_BLOCK || size % HM_ALIGNMENT != 0 ||
                size > (size_t)((char *)chunk->epilogue - (char *)block))
                return fail_check(failure, 2, block, HM_MIN_BLOCK, size);
            if (!!(block->word & HM_FLAG_PREV_ALLOC) != previous_allocated)
                return fail_check(failure, 3, block,
                                  (size_t)previous_allocated,
                                  (size_t)!!(block->word & HM_FLAG_PREV_ALLOC));
            if (is_allocated(block)) {
                if (block->requested == 0 ||
                    block->requested > size - sizeof(Block))
                    return fail_check(failure, 4, block,
                                      size - sizeof(Block), block->requested);
                live_bytes += block->requested;
                fragmented += size - sizeof(Block) - block->requested;
            } else {
                /* Immediate coalescing forbids adjacent free blocks. The
                 * footer must agree with the free block's header word. */
                if (!previous_allocated ||
                    *(size_t *)((char *)block + size - sizeof(size_t)) !=
                        block->word)
                    return fail_check(failure, 5, block, block->word,
                                      *(size_t *)((char *)block + size -
                                                  sizeof(size_t)));
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
                previous_allocated)
            return fail_check(failure, 6, chunk->epilogue,
                              (size_t)((char *)chunk->epilogue -
                                       (char *)chunk->first), sum);
    }
    size_t listed_free = 0;
    uint64_t observed_bitmap = 0;
    /* Phase 2: each physical free block must appear once in a size-class
     * list, with a matching back link and bitmap bit. Bound the walk by the
     * physical block count so a list cycle becomes an error. */
    for (size_t cls = 0; cls < HM_CLASS_COUNT; ++cls) {
        Block *previous = NULL;
        for (Block *block = free_lists[cls]; block != NULL;) {
            if (++listed_free > max_blocks || !lookup_chunk_block(
                    (char *)block + sizeof(Block)))
                return fail_check(failure, 7, block, max_blocks, listed_free);
            if (is_allocated(block) || size_class(block_size(block)) != cls ||
                links(block)->prev != previous)
                return fail_check(failure, 8, block,
                                  (size_t)(uintptr_t)previous,
                                  (size_t)(uintptr_t)links(block)->prev);
            observed_bitmap |= UINT64_C(1) << cls;
            previous = block;
            block = links(block)->next;
        }
    }
    if (listed_free != physical_free || observed_bitmap != free_bitmap)
        return fail_check(failure, 9, free_lists, physical_free, listed_free);
    size_t direct_count = 0;
    /* Phase 3: registry entries must be found by the exact-payload hash
     * lookup and satisfy their recorded alignment and block metadata. */
    for (DirectMap *item = direct_maps; item != NULL; item = item->next) {
        if (++direct_count > stats.current_heap_bytes / HM_MIN_BLOCK ||
            lookup_direct(item->payload, NULL) != item ||
            (uintptr_t)item->payload % item->alignment != 0 ||
            (uintptr_t)item->payload < (uintptr_t)item->mapping +
                                            sizeof(DirectMap) + sizeof(Block) ||
            (uintptr_t)item->payload >=
                (uintptr_t)item->mapping + item->mapping_size)
            return fail_check(failure, 10, item, item->alignment,
                              (size_t)(uintptr_t)item->payload);
        Block *block = (Block *)((char *)item->payload - sizeof(Block));
        if (!is_allocated(block) || !(block->word & HM_FLAG_MMAP) ||
            block->requested > block_size(block) - sizeof(Block))
            return fail_check(failure, 11, block,
                              block_size(block) - sizeof(Block),
                              block->requested);
        heap_bytes += item->mapping_size;
        live_bytes += block->requested;
        fragmented += block_size(block) - sizeof(Block) - block->requested;
    }
    /* Phase 4: compare the fresh walk's totals with running accounting. */
    if (heap_bytes != stats.current_heap_bytes ||
        live_bytes != stats.current_live_bytes ||
        fragmented != stats.internal_fragmentation_bytes)
        return fail_check(failure, 12, &stats, stats.current_heap_bytes,
                          heap_bytes);
    return 0;
}

/* Called with every arena locked. Check each arena, then make sure the atomic
 * process totals used for peak tracking equal the sum of arena accounting. */
static int check_all_locked(CheckFailure *failure)
{
    size_t live = 0;
    size_t mapped = 0;
    for (size_t i = 0; i < HM_ACTIVE_ARENAS; ++i) {
        active_heap = &heaps[i];
        int result = check_locked(failure);
        if (result != 0) return result;
        live += stats.current_live_bytes;
        mapped += stats.current_heap_bytes;
    }
    size_t recorded_live = atomic_load_explicit(&global_live_bytes,
                                                memory_order_relaxed);
    size_t recorded_mapped = atomic_load_explicit(&global_heap_bytes,
                                                  memory_order_relaxed);
    if (live != recorded_live)
        return fail_check(failure, 13, &global_live_bytes, live, recorded_live);
    if (mapped != recorded_mapped)
        return fail_check(failure, 14, &global_heap_bytes, mapped,
                          recorded_mapped);
    return 0;
}

/* HM_DEBUG checks after operations while the lock is held. This is costly
 * because it walks the whole heap; HM_DEBUG_STRIDE can check less often. */
static void debug_check(void)
{
#if HM_DEBUG
    if (++debug_operations % HM_DEBUG_STRIDE == 0 && check_locked(NULL) != 0) {
        static const char message[] = "HyperMalloc: heap invariant failure\n";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        abort();
    }
#endif
}

/* Public entry points keep locking and errno handling outside _locked helpers.
 * Successful calls restore the caller's errno; allocation failures use ENOMEM. */
void *hm_malloc(size_t size)
{
    int saved_errno = errno;
    active_heap = choose_home_heap();
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
    /* A power of two has only one bit set. Zero and too-small alignments are
     * invalid; alignments below 16 but otherwise valid use the normal path. */
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0) {
        errno = EINVAL;
        return NULL;
    }
    if (alignment < HM_ALIGNMENT) alignment = HM_ALIGNMENT;
    int saved_errno = errno;
    active_heap = choose_home_heap();
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
    /* free must preserve errno, including when ptr is NULL. */
    int saved_errno = errno;
    if (ptr != NULL) {
        Block *block = NULL;
        DirectMap *direct = NULL;
        if (lock_owner(ptr, &block, &direct)) {
            if (is_allocated(block)) {
                free_locked(ptr, block, direct);
#if HM_STATS
                ++stats.frees;
#endif
            } else {
                invalid_use();
            }
            debug_check();
            unlock_heap();
        } else {
            invalid_use();
        }
    }
    errno = saved_errno;
}

void *hm_calloc(size_t nmemb, size_t size)
{
    /* Detect multiplication overflow before allocating or zeroing bytes. */
    if (size != 0 && nmemb > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    size_t bytes = nmemb * size;
    void *result = hm_malloc(bytes);
    if (result != NULL) memset(result, 0, bytes);
    return result;
}

/* Resize while preserving min(old requested, new requested) bytes. An
 * allocation failure leaves the old pointer live. Direct maps can stay in
 * place while their mapped capacity suffices; ordinary blocks can split or
 * absorb a free neighbor. */
void *hm_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) return hm_malloc(size);
    if (size == 0) {
        hm_free(ptr);
        return NULL;
    }
    int saved_errno = errno;
    Block *block = NULL;
    DirectMap *direct = NULL;
    if (!lock_owner(ptr, &block, &direct)) {
        invalid_use();
        errno = EINVAL;
        return NULL;
    }
#if HM_STATS
    ++stats.reallocs;
#endif
    if (!is_allocated(block)) {
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
    if (direct != NULL && size <= old_capacity) {
        /* The direct map already owns these bytes. On shrink, unmap complete
         * trailing pages while preserving the descriptor, header, payload,
         * and exact pointer recorded in its hash bucket. munmap may fail;
         * that only prevents trimming, not the valid in-place resize. */
        stats.internal_fragmentation_bytes -= old_capacity - old_requested;
        if (size < old_requested) {
            size_t used = (size_t)((uintptr_t)ptr -
                                   (uintptr_t)direct->mapping) + size;
            size_t keep_bytes;
            if (align_size(used, (size_t)getpagesize(), &keep_bytes) &&
                keep_bytes < direct->mapping_size) {
                size_t released = direct->mapping_size - keep_bytes;
                if (munmap((char *)direct->mapping + keep_bytes,
                           released) == 0) {
                    direct->mapping_size = keep_bytes;
                    block->word = ((size_t)((char *)direct->mapping +
                                           keep_bytes - (char *)block) &
                                   ~(size_t)(HM_ALIGNMENT - 1)) |
                                  HM_FLAG_ALLOC | HM_FLAG_PREV_ALLOC |
                                  HM_FLAG_MMAP;
                    stats.current_heap_bytes -= released;
                    global_sub_mapping(released);
                }
            }
        }
        if (size >= old_requested) global_add_live(size - old_requested);
        else global_sub_live(old_requested - size);
        stats.current_live_bytes = stats.current_live_bytes - old_requested +
                                   size;
        if (stats.current_live_bytes > stats.peak_live_bytes)
            stats.peak_live_bytes = stats.current_live_bytes;
        block->requested = size;
        stats.internal_fragmentation_bytes +=
            block_size(block) - sizeof(Block) - size;
        debug_check();
        unlock_heap();
        errno = saved_errno;
        return ptr;
    }
    if (direct == NULL && size < HM_LARGE_THRESHOLD) {
        /* Case 1: the current ordinary block already has enough capacity.
         * Split a viable tail so future allocations can reuse it. */
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
            if (size >= old_requested) global_add_live(size - old_requested);
            else global_sub_live(old_requested - size);
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
        /* Case 2: the next physical block is free and can be consumed.
         * This grows in place, so the caller's pointer does not change. */
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
            if (size >= old_requested) global_add_live(size - old_requested);
            else global_sub_live(old_requested - size);
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
    /* Case 3: move. Keep a direct allocation's requested alignment, copy
     * only bytes that were part of the old request, then free the old block. */
    size_t alignment = direct != NULL ? direct->alignment : HM_ALIGNMENT;
    void *replacement = malloc_locked(size, alignment);
    if (replacement == NULL) {
        unlock_heap();
        errno = ENOMEM;
        return NULL;
    }
    memcpy(replacement, ptr, old_requested < size ? old_requested : size);
    free_locked(ptr, block, direct);
    debug_check();
    unlock_heap();
    errno = saved_errno;
    return replacement;
}

/* Capacity is block size minus its header; it may exceed requested bytes. */
size_t hm_usable_size(void *ptr)
{
    if (ptr == NULL) return 0;
    Block *block = NULL;
    if (!lock_owner(ptr, &block, NULL)) return 0;
    size_t result = is_allocated(block) ? block_size(block) - sizeof(Block) : 0;
    unlock_heap();
    return result;
}

int hm_check_heap(void)
{
    lock_all_heaps();
    int result = check_all_locked(NULL);
    unlock_all_heaps();
    return result;
}

/* Tiny formatting helpers avoid stdio/malloc when reporting a broken heap.
 * The fixed stack buffer is capped at 159 content bytes plus newline. */
static size_t append_text(char *buffer, size_t position, const char *value)
{
    while (*value != '\0' && position < 159) buffer[position++] = *value++;
    return position;
}

static size_t append_hex(char *buffer, size_t position, size_t value)
{
    static const char digits[] = "0123456789abcdef";
    position = append_text(buffer, position, "0x");
    int started = 0;
    for (int shift = (int)(sizeof(size_t) * 8) - 4; shift >= 0; shift -= 4) {
        size_t nibble = (value >> shift) & 15;
        if (nibble != 0 || started || shift == 0) {
            if (position < 159) buffer[position++] = digits[nibble];
            started = 1;
        }
    }
    return position;
}

int hm_check_heap_verbose(void)
{
    CheckFailure failure = {0};
    lock_all_heaps();
    int result = check_all_locked(&failure);
    unlock_all_heaps();
    if (result != 0) {
        char buffer[160];
        size_t length = append_text(buffer, 0, "HyperMalloc checker code=");
        length = append_hex(buffer, length, (size_t)failure.code);
        length = append_text(buffer, length, " address=");
        length = append_hex(buffer, length,
                            (size_t)(uintptr_t)failure.address);
        length = append_text(buffer, length, " expected=");
        length = append_hex(buffer, length, failure.expected);
        length = append_text(buffer, length, " actual=");
        length = append_hex(buffer, length, failure.actual);
        buffer[length++] = '\n';
        ssize_t written = write(STDERR_FILENO, buffer, length);
        (void)written;
    }
    return result;
}

void hm_get_stats(hm_stats *out)
{
    if (out == NULL) return;
    /* Lock all arenas for one process-wide snapshot. */
    lock_all_heaps();
    hm_stats total = {0};
    for (size_t i = 0; i < HM_ACTIVE_ARENAS; ++i) {
        const hm_stats *part = &heaps[i].counters;
        total.current_live_bytes += part->current_live_bytes;
        total.current_heap_bytes += part->current_heap_bytes;
        total.internal_fragmentation_bytes += part->internal_fragmentation_bytes;
        total.mallocs += part->mallocs;
        total.frees += part->frees;
        total.reallocs += part->reallocs;
        total.splits += part->splits;
        total.coalesces += part->coalesces;
        total.mmaps += part->mmaps;
        total.search_steps += part->search_steps;
    }
    total.peak_live_bytes = atomic_load_explicit(&global_peak_live_bytes,
                                                 memory_order_relaxed);
    total.peak_heap_bytes = atomic_load_explicit(&global_peak_heap_bytes,
                                                memory_order_relaxed);
    *out = total;
    unlock_all_heaps();
}



#undef chunks
#undef direct_maps
#undef direct_buckets
#undef free_lists
#undef free_bitmap
#undef stats
#if HM_DEBUG
#undef debug_operations
#endif
#undef heap_mutex
