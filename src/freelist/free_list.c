/*
 * Purpose:
 * Manages HyperMalloc's segregated free lists, including
 * size classes, insertion, removal, and free-block lookup.
 */

#include "internal.h"

#include <limits.h>

// One linked list for each size class(32).
static BlockHeader* free_lists[NUM_SIZE_CLASSES] = { NULL };

// Each bit represents whether a size class contains a free block.
static uint32_t free_list_bitmap = 0;


/*
 * Maps a block size to its corresponding size class.
 */
size_t get_size_class(size_t size)
{
    if (size <= 1) {
        return 0;
    }

    return sizeof(size_t) * CHAR_BIT // how many bits are in the representaion of a size_t e.g. 8 * 8
           - __builtin_clzl(size - 1) // how many 0s before the number starts e.g. 55 ie. 64-55  so 9 active bits being used
           - 1; // 9-1 = 8 for the class we are using since it is zero indexed
}


/*
 * Adds a free block to the front of its size-class list.
 */
void insert_into_free_list(BlockHeader* block)
{
    size_t class = get_size_class(block->size);

    block->previous_free = NULL; // add block to front of the list ie. no prev
    block->next_free = free_lists[class]; // next free should be head of freelist class

    if (free_lists[class] != NULL) {
        free_lists[class]->previous_free = block; // double link it 
    }

    free_lists[class] = block; // block is the new head of free list class 

    // Mark this size class as non-empty.
    free_list_bitmap |= (1U << class);
}


/*
 * Removes a block from its size-class free list.
 */
void remove_from_free_list(BlockHeader* block)
{
    size_t class = get_size_class(block->size);

    if (block->previous_free != NULL) {
        block->previous_free->next_free = block->next_free;
    } else {
        free_lists[class] = block->next_free;
    }

    if (block->next_free != NULL) {
        block->next_free->previous_free = block->previous_free;
    }

    block->previous_free = NULL;
    block->next_free = NULL;

    // Clear the bitmap bit if this class is now empty.
    if (free_lists[class] == NULL) {
        free_list_bitmap &= ~(1U << class);
    }
}


/*
 * Finds the first non-empty size class at or above the requested class.
 */
BlockHeader* find_free_block(size_t user_size)
{
    size_t class = get_size_class(user_size);

    // Ignore size classes smaller than the requested class.
    uint32_t available = free_list_bitmap >> class;

    if (available == 0) {
        return NULL;
    }

    // Find the first available class.
    size_t offset = __builtin_ctz(available); // count zeros from the right
    size_t selected_class = class + offset;

    return free_lists[selected_class];
}
