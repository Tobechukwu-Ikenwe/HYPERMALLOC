#ifndef HYPERMALLOC_INTERNAL_H
#define HYPERMALLOC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#define HYPERMALLOC_MAGIC 0xDEADBEEFCAFEBABEULL
#define NUM_SIZE_CLASSES 32

typedef struct BlockHeader {
    size_t size;

    /* Physical neighbors in the heap. */
    struct BlockHeader *previous;
    struct BlockHeader *next;

    /* Neighbors in the segregated free list. */
    struct BlockHeader *previous_free;
    struct BlockHeader *next_free;

    /* 1 = free, 0 = allocated. */
    int free;

    /* Used to detect invalid or corrupted blocks. */
    uint64_t magic;
} BlockHeader;


/* ---------- Heap state ---------- */

extern void *heap_start;
extern void *heap_end;
extern BlockHeader *heap_tail;


/* ---------- Heap management ---------- */

void init_heap(void);
BlockHeader *request_block(size_t user_size);


/* ---------- Block management ---------- */

BlockHeader *calculate_header(void *ptr);
int validate_ptr(void *ptr);
BlockHeader *find_block(void *ptr);

void split_block(BlockHeader *block, size_t user_size);
void coalesce_next(BlockHeader *block);


/* ---------- Free-list management ---------- */

size_t get_size_class(size_t size);

void insert_into_free_list(BlockHeader *block);
void remove_from_free_list(BlockHeader *block);
BlockHeader *find_free_block(size_t user_size);

#endif

int get_sbrk_calls(void);
