/*
 * Purpose:
 * Implements HyperMalloc's public allocation function.
 * Reuses free blocks when possible, otherwise requests new memory.
 */

#include "hypermalloc.h"
#include "internal.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

static int align_request(size_t size, size_t *aligned)
{
    const size_t alignment = _Alignof(max_align_t);
    if (size > SIZE_MAX - (alignment - 1)) return 0;
    *aligned = (size + alignment - 1) & ~(alignment - 1);
    return 1;
}

static void *hyper_malloc_impl(size_t user_size)
{
    // Initialize the heap on the first allocation.
    init_heap();

    // A zero-size allocation returns NULL.
    if (user_size == 0) {
        return NULL;
    }

    if (!align_request(user_size, &user_size) ||
        user_size > SIZE_MAX - sizeof(BlockHeader)) {
        errno = ENOMEM;
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

void *hyper_malloc(size_t size)
{
    allocator_lock();
    void *ptr = hyper_malloc_impl(size);
    allocator_unlock();
    return ptr;
}

void *hyper_calloc(size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = count * size;
    void *ptr = hyper_malloc(total);
    if (ptr != NULL) memset(ptr, 0, total);
    return ptr;
}

void *hyper_reallocarray(void *ptr, size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    return hyper_realloc(ptr, count * size);
}

size_t hyper_malloc_usable_size(void *ptr)
{
    allocator_lock();
    BlockHeader *block = find_block(ptr);
    size_t result = (block == NULL || block->free) ? 0 : block->size;
    allocator_unlock();
    return result;
}

static void *hyper_aligned_alloc_impl(size_t alignment, size_t size)
{
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0 ||
        size % alignment != 0) {
        errno = EINVAL;
        return NULL;
    }
    if (size == 0) return NULL;
    if (alignment <= _Alignof(max_align_t)) return hyper_malloc(size);

    if (size > SIZE_MAX - alignment ||
        size + alignment > SIZE_MAX - 2 * sizeof(BlockHeader) - _Alignof(max_align_t)) {
        errno = ENOMEM;
        return NULL;
    }

    size_t request = size + alignment + 2 * sizeof(BlockHeader) + _Alignof(max_align_t);
    void *raw = hyper_malloc(request);
    if (raw == NULL) return NULL;

    BlockHeader *original = calculate_header(raw);
    uintptr_t payload = (uintptr_t)raw;
    uintptr_t minimum = payload + 2 * sizeof(BlockHeader) + _Alignof(max_align_t);
    uintptr_t aligned = (minimum + alignment - 1) & ~(uintptr_t)(alignment - 1);
    BlockHeader *new_block = (BlockHeader *)(aligned - sizeof(BlockHeader));
    size_t gap = (size_t)((char *)new_block - (char *)raw);

    size_t old_size = original->size;
    BlockHeader *previous = original->previous;
    BlockHeader *next = original->next;
    BlockHeader *prefix = original;

    prefix->size = gap - sizeof(BlockHeader);
    prefix->free = 1;
    prefix->next = new_block;
    prefix->previous = previous;

    new_block->size = old_size - gap - sizeof(BlockHeader);
    new_block->free = 0;
    new_block->magic = HYPERMALLOC_MAGIC;
    new_block->previous = prefix;
    new_block->next = next;
    new_block->previous_free = NULL;
    new_block->next_free = NULL;
    if (previous != NULL) previous->next = prefix;
    if (next != NULL) next->previous = new_block;
    else heap_tail = new_block;

    insert_into_free_list(prefix);
    if (previous != NULL && previous->free) {
        remove_from_free_list(previous);
        (void)coalesce_next(previous);
        insert_into_free_list(previous);
    }
    return (void *)(new_block + 1);
}

void *hyper_aligned_alloc(size_t alignment, size_t size)
{
    allocator_lock();
    void *ptr = hyper_aligned_alloc_impl(alignment, size);
    allocator_unlock();
    return ptr;
}

int hyper_posix_memalign(void **memptr, size_t alignment, size_t size)
{
    if (memptr == NULL || alignment < sizeof(void *) ||
        (alignment & (alignment - 1)) != 0) return EINVAL;
    if (size == 0) {
        *memptr = NULL;
        return 0;
    }
    /* C11 aligned_alloc requires a multiple of alignment; POSIX does not. */
    if (size > SIZE_MAX - (alignment - 1)) return ENOMEM;
    size_t rounded = (size + alignment - 1) & ~(alignment - 1);
    void *result = hyper_aligned_alloc(alignment, rounded);
    if (result == NULL) return errno == EINVAL ? EINVAL : ENOMEM;
    *memptr = result;
    return 0;
}
