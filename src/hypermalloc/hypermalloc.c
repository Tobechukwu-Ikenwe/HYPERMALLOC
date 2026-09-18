/*
 * Purpose:
 * Implements HyperMalloc's public allocation function.
 * Reuses free blocks when possible, otherwise requests new memory.
 */

#include "hypermalloc.h"
#include "internal.h"

void *hyper_malloc(size_t user_size)
{
    // Initialize the heap on the first allocation.
    init_heap();

    // A zero-size allocation returns NULL.
    if (user_size == 0) {
        return NULL;
    }

    // Look for a reusable free block.
    BlockHeader *block = find_free_block(user_size);

    if (block != NULL) {

        // Split the block if enough space remains.
        if (block->size >= user_size + sizeof(BlockHeader) + 1) {
            split_block(block, user_size);
        } else {
            // Use the entire free block.
            remove_from_free_list(block);
            block->free = 0;
        }

        // Return the payload, not the header.
        return (void *)(block + 1);
    }

    // No suitable free block exists, so request more memory.
    block = request_block(user_size);

    if (block == NULL) {
        return NULL;
    }

    // Return the memory immediately after the header.
    return (void *)(block + 1);
}
