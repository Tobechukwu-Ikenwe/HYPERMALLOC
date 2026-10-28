/*
 * Purpose:
 * Implements HyperMalloc's public realloc function.
 * Tries to resize the existing block before allocating new memory.
 */

#include "hypermalloc.h"
#include "internal.h"

#include <string.h>

#include <errno.h>


/*
 * Copies bytes from one memory region to another.
 *
 * This is HyperMalloc's internal replacement for memcpy().
 */
static void hyper_copy(void* old_ptr, void* new_ptr, size_t size)
{
    memcpy(new_ptr, old_ptr, size);
}


/*
 * Tries to expand a block into consecutive free blocks
 * immediately following it.
 *
 * Returns 1 if the block was successfully expanded.
 * Returns 0 if there is not enough adjacent free space.
 */
static int expand_block(BlockHeader *block, size_t new_size)
{
    // The block is already large enough.
    if (new_size <= block->size) {
        return 1;
    }

    // Calculate how many additional bytes we need.
    size_t required = new_size - block->size;

    // Track how much space neighboring free blocks provide.
    size_t available = 0;

    // Start checking the physical block immediately after this one.
    BlockHeader *next = block->next;

    // Add consecutive free blocks until we have enough space.
    while (next != NULL && next->free == 1) {

        if (next->previous == NULL ||
            (char *)(next->previous + 1) + next->previous->size != (char *)next)
            break;

        available += sizeof(BlockHeader) + next->size;

        if (available >= required) {
            break;
        }

        next = next->next;
    }

    // There is not enough adjacent free space.
    if (available < required) {
        return 0;
    }

    // Start merging the neighboring free blocks into this block.
    next = block->next;

    while (block->size < new_size) {

        if (next == NULL || (char *)(block + 1) + block->size != (char *)next)
            return 0;

        // Save the block after the one we're about to remove.
        BlockHeader *after = next->next;

        // Remove the neighboring block from the free list.
        remove_from_free_list(next);

        // Absorb its header and payload into our block.
        block->size += sizeof(BlockHeader) + next->size;

        // Remove the neighboring block from the physical list.
        block->next = after;

        // Fix the previous pointer of the block after it.
        if (after != NULL) {
            after->previous = block;
        } else {
            // If there is no block after it, this is now the heap tail.
            heap_tail = block;
        }

        // Move to the next physical block.
        next = after;
    }

    return 1;
}


static void *hyper_realloc_impl(void *old_ptr, size_t new_size)
{
    // realloc(NULL, size) behaves like malloc(size).
    if (old_ptr == NULL) {
        return hyper_malloc(new_size);
    }

    // realloc(ptr, 0) frees the block and returns NULL.
    if (new_size == 0) {
        hyper_free(old_ptr);
        return NULL;
    }

    const size_t alignment = _Alignof(max_align_t);
    if (new_size > SIZE_MAX - (alignment - 1)) {
        errno = ENOMEM;
        return NULL;
    }
    new_size = (new_size + alignment - 1) & ~(alignment - 1);
    if (new_size > SIZE_MAX - sizeof(BlockHeader)) {
        errno = ENOMEM;
        return NULL;
    }

    // Find the block header belonging to the user's pointer.
    BlockHeader *block = find_block(old_ptr);

    // Reject pointers that do not belong to HyperMalloc.
    if (block == NULL) {
        errno = EINVAL;
        return NULL;
    }

    // Reject a block that has already been freed.
    if (block->free == 1) {
        errno = EINVAL;
        return NULL;
    }

    // If the requested size is smaller or equal,
    // the existing allocation is already large enough.
    if (new_size < block->size) {
        if (block->size - new_size >= sizeof(BlockHeader) + alignment) {
            split_block(block, new_size);
            /* split_block has inserted the new free tail. Merge that tail
             * with an already-free following block, never with the live
             * block being resized. */
            BlockHeader *tail = block->next;
            remove_from_free_list(tail);
            while (coalesce_next(tail)) { }
            insert_into_free_list(tail);
        }
        return old_ptr;
    }

    if (new_size == block->size) {
        return old_ptr;
    }

    // Try to expand the existing block using adjacent
    // free blocks instead of allocating somewhere else.
    if (expand_block(block, new_size)) {
        return old_ptr;
    }

    // We could not expand in place, so request a new block.
    void *new_ptr = hyper_malloc(new_size);

    // If allocation failed, keep the original block untouched.
    if (new_ptr == NULL) {
        return NULL;
    }

    // Copy the old contents into the new allocation.
    hyper_copy(old_ptr, new_ptr, block->size);

    // Release the old allocation.
    hyper_free(old_ptr);

    // Give the caller the new allocation.
    return new_ptr;
}

void *hyper_realloc(void *ptr, size_t size)
{
    allocator_lock();
    void *result = hyper_realloc_impl(ptr, size);
    allocator_unlock();
    return result;
}
