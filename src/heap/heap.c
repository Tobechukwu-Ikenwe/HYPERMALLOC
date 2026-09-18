#define _DEFAULT_SOURCE

/*
 * Purpose:
 * Manages HyperMalloc's heap state and requests new memory
 * from the operating system using sbrk().
 */

#include "internal.h"

#include <unistd.h>

// Tracks the beginning and end of the heap managed by HyperMalloc.
void *heap_start = NULL;
void *heap_end = NULL;

// Tracks the physical last block.
BlockHeader *heap_tail = NULL;

// Physical first block.
static BlockHeader *list_head = NULL;

// Counts how many times HyperMalloc requests memory from the OS.
static int sbrk_calls = 0;

/* Amount of memory requested from the OS at a time. */
#define HEAP_CHUNK_SIZE (64 * 1024)

/* Points to the next unused byte in the current heap chunk. */
static void *heap_cursor = NULL;

void init_heap(void)
{
    // Initialize the heap only once.
    if (heap_start != NULL) {
        return;
    }

    heap_start = sbrk(0);
    heap_end = heap_start;
    heap_cursor = heap_start;
}

// Requests a new block from the OS.
BlockHeader *request_block(size_t user_size)
{
    size_t total_size = sizeof(BlockHeader) + user_size;

    /*
     * Request a new chunk only when the current chunk cannot fit
     * the requested block.
     */
    if ((char *)heap_cursor + total_size > (char *)heap_end) {
        size_t chunk_size = HEAP_CHUNK_SIZE;

        if (total_size > chunk_size) {
            chunk_size = total_size;
        }

        void *chunk = sbrk(chunk_size);

        if (chunk == (void *)-1) {
            return NULL;
        }

        sbrk_calls++;

        heap_cursor = chunk;
        heap_end = (char *)chunk + chunk_size;
    }

    BlockHeader *block = (BlockHeader *)heap_cursor;

    heap_cursor = (char *)heap_cursor + total_size;

    block->size = user_size;
    block->free = 0;
    block->previous_free = NULL;
    block->next_free = NULL;
    block->magic = HYPERMALLOC_MAGIC;

    block->previous = heap_tail;
    block->next = NULL;

    if (heap_tail != NULL) {
        heap_tail->next = block;
    } else {
        list_head = block;
    }

    heap_tail = block;

    return block;
}

int get_sbrk_calls(void)
{
    return sbrk_calls;
}
