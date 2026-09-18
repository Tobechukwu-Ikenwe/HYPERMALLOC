/*
 * Purpose:
 * Implements HyperMalloc's public free function.
 * Handles validation, coalescing, and free-list insertion.
 */

#include "hypermalloc.h"
#include "internal.h"

#include <errno.h>
#include <stdio.h>


void hyper_free(void *ptr)
{
    // free(NULL) does nothing.
    if (ptr == NULL) {
        return;
    }

    // Make sure the pointer belongs to a valid HyperMalloc block.
    BlockHeader *block = find_block(ptr);

    if (block == NULL) {
        errno = EINVAL;
        perror("hyper_free");
        return;
    }

    // Reject a double free.
    if (block->free == 1) {
        errno = EINVAL;
        perror("hyper_free");
        return;
    }

    // Mark the block as free.
    block->free = 1;

    // Merge with consecutive free blocks after this block.
    while (block->next != NULL &&
           block->next->free == 1) {
        coalesce_next(block);
    }

    // If the previous block is free, merge this block into it.
    BlockHeader *previous = block->previous;

    if (previous != NULL &&
        previous->free == 1) {
        coalesce_next(previous);
        block = previous;
    }

    // Add the final merged block to the free list.
    insert_into_free_list(block);
}
