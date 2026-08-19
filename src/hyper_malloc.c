#include <unistd.h>
// sbrk() gives us memory from the operating system.

#include <stdio.h>
// Standard input/output functions such as printf() and fprintf().

#include <errno.h>
// Error codes such as EINVAL.

#include "hypermalloc.h"
// My HyperMalloc API.


/*
 * ASK FOR A REGION OF MEMORY FROM THE OPERATING SYSTEM
 */

// get the program break
//
// heap_start marks the beginning of the memory managed by HyperMalloc.
// heap_end marks the current end of the memory managed by HyperMalloc.

static void* heap_start = NULL;
static void* heap_end = NULL;


// static variable to measure number of sbrk calls for testing
static int sbrk_calls = 0;


void init_heap(void)
{
	// set both start and end to program break.
	// the heap end will grow dynamically.
	if (heap_start == NULL) {

		heap_start = sbrk(0);

		if (heap_start == (void*)-1) {
			heap_start = NULL;
			return;
		}

		heap_end = heap_start;
	}
}


/*
 * BLOCK HEADER
 *
 * Every block of memory managed by HyperMalloc has a header
 * immediately before the memory returned to the user.
 *
 * Physical heap list:
 *
 *     [header][payload]<-> [header][payload] <-> ...
 *
 * Free list:
 *
 *     [free block] <-> [free block] <-> ...
 *
 * We use two different sets of pointers because the physical
 * order of blocks and the order of free blocks are different.
 */

typedef struct BlockHeader {

	// size of the block.
	// this region is just under the BlockHeader.
	size_t size;

	
   	 // pointer to the previous block on the heap
    	struct BlockHeader* previous;

	// pointer to the neighbouring block on the physical heap.
	struct BlockHeader* next;

	// pointer to previous free block.
	struct BlockHeader* previous_free;

	// pointer to next free block.
	struct BlockHeader* next_free;

	// status: is this block free or allocated?
	int free;

} BlockHeader;


// create the head of our physical heap list.
// this is the entry point for all blocks.
static BlockHeader* list_head = NULL;


// create the head of our free list.
static BlockHeader* free_list_head = NULL;

// POINTER TO end of the heap 
static BlockHeader* heap_tail = NULL;

/*
 * FIND A BLOCK
 *
 * Search the physical heap list for the block that owns
 * the pointer returned to the user.
 *
 * The user receives:
 *
 *     [BlockHeader][user memory]
 *                  ^
 *                  ptr
 *
 * Therefore the user pointer must equal (current + 1).
 */

static BlockHeader* find_block(void* ptr)
{
	BlockHeader* current = list_head;

	while (current != NULL) {

		// check whether this block owns the pointer
		if ((void*)(current + 1) == ptr) {
			return current;
		}

		// move to the next physical block
		current = current->next;
	}

	// no block owns this pointer
	return NULL;
}


/*
 * ADD A BLOCK TO THE FREE LIST
 *
 * i insert the block at the front of the doubly linked list.
 *
 * Before:
 *
 *     free_list_head
 *          ↓
 *          A <-> B
 *
 * After:
 *
 *     free_list_head
 *          ↓
 *          H <-> A <-> B
 */

static void insert_into_free_list(BlockHeader* block)
{
	// the new head has no previous block
	block->previous_free = NULL;

	// the new head points forward to the old head
	block->next_free = free_list_head;

	// if the old head exists, connect it back to block
	if (free_list_head != NULL) {
		free_list_head->previous_free = block;
	}

	// block is now the new head
	free_list_head = block;
}


/*
 * REMOVE A BLOCK FROM THE FREE LIST
 *
 * There are three possible situations:
 *
 * 1. block is the head
 * 2. block is in the middle
 * 3. block is the tail
 *
 * The previous_free and next_free pointers allow us
 * to reconnect the neighboring blocks without walking
 * backward through the list.
 */

static void remove_from_free_list(BlockHeader* block)
{
	// if there is a previous free block,
	// connect it to block's next free block.
	if (block->previous_free != NULL) {

		block->previous_free->next_free =
			block->next_free;

	} else {

		// block is the free-list head.
		free_list_head = block->next_free;
	}

	// if there is a next free block,
	// connect it back to block's previous free block.
	if (block->next_free != NULL) {

		block->next_free->previous_free =
			block->previous_free;
	}

	// block is no longer part of the free list.
	block->previous_free = NULL;
	block->next_free = NULL;
}

/*
 * REQUEST A NEW BLOCK FROM THE OPERATING SYSTEM
 *
 * This is the only helper responsible for asking the OS
 * for more memory.
 */

static BlockHeader* request_block(size_t user_size)
{
	// ask the OS for enough space for:
	//
	//     BlockHeader + user memory
	void* allocated_start =
		sbrk(sizeof(BlockHeader) + user_size);

	// check if allocation worked
	if (allocated_start == (void*)-1) {
		perror("sbrk");
		return NULL;
	}

	// increment sbrk counter for the benchmark file
	sbrk_calls++;

	// get the new program break.
	heap_end = sbrk(0);

	// create the block header at the beginning
	// of the memory returned by sbrk().
	BlockHeader* h = allocated_start;

	h->size = user_size;
	h->free = 0;

	// allocated blocks are not in the free list
	h->previous_free = NULL;
	h->next_free = NULL;

	// this is currently the last physical block
	h->next = NULL;
	h->previous = heap_tail;

	// add h to the physical heap list
	if (heap_tail == NULL) {

		// heap list is empty
		list_head = h;

	} else {

		// connect the old last block to h
		heap_tail->next = h;
	}

	// h is now the last physical block
	heap_tail = h;

	return h;
}
/*
 * SPLIT A FREE BLOCK
 *
 * If a free block is larger than the requested allocation,
 * split it into two physical blocks:
 *
 *     [allocated block][new free block]
 *
 * The original block keeps its position in the physical
 * heap list. The new free block is inserted immediately
 * after it.
 */

static BlockHeader* split_block(BlockHeader* block, size_t user_size)
{
	// Save the original size before changing block->size.
	size_t original_size = block->size;

	// The block must have enough room for:
	//
	//     requested payload
	//     + another BlockHeader
	//     + at least 1 byte of payload
	if (original_size < user_size + sizeof(BlockHeader) + 1) {
		return NULL;
	}

	/*
	 * CREATE THE NEW FREE BLOCK
	 *
	 * block + 1 moves past the current BlockHeader.
	 * Adding user_size moves past the new allocated payload.
	 */

	BlockHeader* split =
		(BlockHeader*)((char*)(block + 1) + user_size);

	// Set up the new free block.
	split->size =
		original_size - user_size - sizeof(BlockHeader);

	split->free = 1;

	// Save the block that comes after block.
	BlockHeader* next_in_heap = block->next;

	// Connect split into the physical heap list.
	split->previous = block;
	split->next = next_in_heap;

	block->next = split;

	// Connect the next physical block back to split.
	if (next_in_heap != NULL) {
		next_in_heap->previous = split;
	} else {
		// split is now the last physical block.
		heap_tail = split;
	}

	/*
	 * CONNECT THE FREE LIST
	 *
	 * The original block is about to become allocated,
	 * so remove it from the free list.
	 *
	 * Then insert the new split block.
	 */

	remove_from_free_list(block);

	insert_into_free_list(split);


	// The original block now contains only the requested payload.
	block->size = user_size;
	block->free = 0;

	return block;
}


/*
 * FIND A SUITABLE FREE BLOCK
 *
 * Search only the free list.
 *
 * This is important:
 * we do NOT need to walk allocated blocks because they
 * cannot satisfy the allocation request.
 */

static BlockHeader* find_free_block(size_t user_size)
{
	BlockHeader* current = free_list_head;

	while (current != NULL) {

		// Find the first free block large enough.
		if (current->size >= user_size) {
			return current;
		}

		// Move through the free list.
		current = current->next_free;
	}

	// No suitable free block exists.
	return NULL;
}


/*
 * HYPER_MALLOC()
 *
 * Allocate user_size bytes of memory.
 *
 * Allocation strategy:
 *
 * 1. Initialize the heap.
 * 2. Search the free list.
 * 3. If a suitable free block exists:
 *      - split it if possible
 *      - otherwise use the entire block
 * 4. If no free block exists:
 *      - request memory from the OS.
 */


/*
 * COALESCE WITH THE NEXT PHYSICAL BLOCK
 *
 * If the block immediately after block is free,
 * merge that block into block.
 *
 * Before:
 *
 *     [block][FREE]
 *
 * After:
 *
 *     [block.............]
 */

static void coalesce_next(BlockHeader* block)
{
	BlockHeader* next = block->next;

	// Nothing after this block.
	if (next == NULL) {
		return;
	}

	// The next block must be free before we can merge it.
	if (next->free == 0) {
		return;
	}

	// Remove the neighboring free block from the free list.
	remove_from_free_list(next);

	/*
	 * Absorb the next block's header and payload.
	 *
	 * The size of the resulting block is:
	 *
	 *     current payload
	 *     + next header
	 *     + next payload
	 */

	block->size += sizeof(BlockHeader) + next->size;

	// Skip over the block that was absorbed.
	block->next = next->next;

	// Connect the following physical block back to block.
	if (block->next != NULL) {
		block->next->previous = block;
	} else {
		// next was the last physical block.
		heap_tail = block;
	}
}


void* hyper_malloc(size_t user_size)
{
	/*
	 * INITIALIZE THE HEAP
	 */

	init_heap();

	// A zero-byte allocation is not useful for this allocator.
	if (user_size == 0) {
		return NULL;
	}


	/*
	 * SEARCH THE FREE LIST
	 */

	BlockHeader* block = find_free_block(user_size);

	if (block != NULL) {

		/*
		 * HANDLE A FREE BLOCK THAT IS LARGE ENOUGH TO SPLIT
		 */

		if (block->size >=
		    user_size + sizeof(BlockHeader) + 1) {

			BlockHeader* result =
				split_block(block, user_size);

			return (void*)(result + 1);
		}


		/*
		 * USE THE ENTIRE FREE BLOCK
		 */

		remove_from_free_list(block);

		block->free = 0;

		// Return the memory immediately after the header.
		return (void*)(block + 1);
	}


	/*
	 * NO SUITABLE FREE BLOCK EXISTS
	 *
	 * Ask the operating system for more memory.
	 */

	block = request_block(user_size);

	if (block == NULL) {
		return NULL;
	}


	/*
	 * RETURN THE USER PAYLOAD
	 *
	 * The BlockHeader is metadata owned by HyperMalloc.
	 * The caller receives the memory immediately after it.
	 */

	return (void*)(block + 1);
}

/*
 * HYPER_FREE()
 *
 * Free a block previously returned by hyper_malloc().
 *
 * Freeing memory does NOT normally return the memory to the OS.
 * Instead, HyperMalloc marks the block as free and puts it
 * into the free list so it can be reused later.
 */

void hyper_free(void* ptr)
{
	/*
	 * CHECK FOR NULL
	 */

	if (ptr == NULL) {
		return;
	}


	/*
	 * FIND THE BLOCK
	 *
	 * We do not trust the pointer simply because it falls
	 * somewhere inside our heap.
	 *
	 * find_block() verifies that this exact pointer belongs
	 * to one of our allocations.
	 */

	BlockHeader* block = find_block(ptr);

	if (block == NULL) {
		errno = EINVAL;
		perror("hyper_free");
		return;
	}


	/*
	 * CHECK FOR DOUBLE FREE
	 */

	if (block->free == 1) {
		errno = EINVAL;
		perror("hyper_free");
		return;
	}


	/*
	 * MARK THE BLOCK AS FREE
	 */

	block->free = 1;


	/*
	 * COALESCE WITH THE NEXT BLOCK
	 *
	 * Example:
	 *
	 *     [A FREE][B FREE]
	 *
	 * becomes:
	 *
	 *     [A FREE................]
	 */

	while (block->next != NULL &&
	       block->next->free == 1) {

		coalesce_next(block);
	}


	/*
	 * COALESCE WITH THE PREVIOUS BLOCK
	 *
	 * Example:
	 *
	 *     [A FREE][B FREE]
	 *
	 * becomes:
	 *
	 *     [A................FREE]
	 */

	BlockHeader* previous = block->previous;
	if (previous != NULL &&
	    previous->free == 1) {

		// Merge block into the previous free block.
		coalesce_next(previous);

		// The previous block is now the resulting block.
		block = previous;
	}


	/*
	 * ADD THE RESULTING FREE BLOCK TO THE FREE LIST
	 */

	insert_into_free_list(block);
}

/*
 * HYPER_COPY()
 *
 * Copy size bytes from old_ptr to new_ptr.
 *
 * This is used by hyper_realloc() when a block must
 * be moved to a new location.
 */

static void hyper_copy(void* old_ptr, void* new_ptr, size_t size)
{
	// Copy one byte at a time.
	for (size_t i = 0; i < size; i++) {

		*((char*)new_ptr + i) =
			*((char*)old_ptr + i);
	}
}

/*
 * EXPAND A BLOCK INTO ADJACENT FREE BLOCKS
 *THIS IS USED FOR REALLOC
 * Try to grow block by consuming consecutive free blocks
 * immediately after it in the physical heap.
 *
 * Example:
 *
 *     [A allocated][B FREE][C FREE]
 *
 * can become:
 *
 *     [A.........................]
 *
 * Returns 1 if enough space was found.
 * Returns 0 if the adjacent free blocks are not enough.
 */

static int expand_block(BlockHeader* block, size_t new_size)
{
	// The block is already large enough.
	if (new_size <= block->size) {
		return 1;
	}

	// Amount of additional payload required.
	size_t required = new_size - block->size;

	// Start with the physical block immediately after block.
	BlockHeader* next = block->next;

	// Keep track of how much neighboring memory we can consume.
	size_t available = 0;

	while (next != NULL && next->free == 1) {

		/*
		 * A free block contributes:
		 *
		 *     its payload
		 *     + its BlockHeader
		 *
		 * because once absorbed, its header becomes usable space.
		 */

		available += sizeof(BlockHeader) + next->size;

		if (available >= required) {
			break;
		}

		next = next->next;
	}


	// The adjacent free blocks were not large enough.
	if (available < required) {
		return 0;
	}


	/*
	 * ABSORB THE REQUIRED FREE BLOCKS
	 */

	next = block->next;

	while (block->size < new_size &&
	       next != NULL &&
	       next->free == 1) {

		// Save the block after next before removing next.
		BlockHeader* after = next->next;

		// Remove next from the free list.
		remove_from_free_list(next);

		// Absorb next into block.
		block->size += sizeof(BlockHeader) + next->size;

		// Connect block to the next physical block.
		block->next = after;

		// Connect the next physical block back to block.
		if (after != NULL) {
			after->previous = block;
		} else {
			// The absorbed block was the last physical block.
			heap_tail = block;
		}

		// Move forward.
		next = after;


	}


	// The block may have absorbed slightly more space than requested. so just set size to be safe
	block->size = new_size;

	return 1;
}



/*
 * HYPER_REALLOC()
 *
 * Change the size of an existing allocation.
 *
 * Cases:
 *
 * 1. old_ptr == NULL
 *      behaves like malloc()
 *
 * 2. new_size == 0
 *      free the old allocation
 *
 * 3. new_size <= old size
 *      shrink in place
 *
 * 4. adjacent free blocks provide enough space
 *      expand in place
 *
 * 5. otherwise
 *      allocate a new block, copy the data, and free the old block.
 */

void* hyper_realloc(void* old_ptr, size_t new_size)
{
	/*
	 * NULL POINTER
	 *
	 * realloc(NULL, size) behaves like malloc(size).
	 */

	if (old_ptr == NULL) {
		return hyper_malloc(new_size);
	}


	/*
	 * ZERO SIZE
	 *
	 * realloc(ptr, 0) is treated here as free(ptr).
	 */

	if (new_size == 0) {
		hyper_free(old_ptr);
		return NULL;
	}


	/*
	 * FIND THE BLOCK
	 */

	BlockHeader* block = find_block(old_ptr);

	if (block == NULL) {
		errno = EINVAL;
		perror("hyper_realloc");
		return NULL;
	}


	/*
	 * CHECK WHETHER THE BLOCK HAS ALREADY BEEN FREED
	 */

	if (block->free == 1) {
		errno = EINVAL;
		perror("hyper_realloc");
		return NULL;
	}


	/*
	 * SAME OR SMALLER SIZE
	 */

	if (new_size <= block->size) {

		/*
		 * We can simply keep the same allocation.
		 *
		 * A later optimization can split the unused
		 * portion into another free block.
		 */

		return old_ptr;
	}


	/*
	 * TRY TO EXPAND IN PLACE
	 */

	if (expand_block(block, new_size)) {
		return old_ptr;
	}


	/*
	 * COULD NOT EXPAND IN PLACE
	 *
	 * Allocate a completely new block.
	 */

	void* new_ptr = hyper_malloc(new_size);

	if (new_ptr == NULL) {
		return NULL;
	}


	/*
	 * COPY THE OLD DATA
	 *
	 * Only copy the amount of data that existed in the
	 * original allocation.
	 */

	hyper_copy(old_ptr, new_ptr, block->size);


	/*
	 * FREE THE OLD BLOCK
	 */

	hyper_free(old_ptr);


	return new_ptr;
}


/*
 * CHECK HEAP
 *
 * Walk through both of our data structures and verify
 * that their basic invariants are still correct.
 *
 * This function is for debugging and testing.
 * It should NOT be used inside the timed benchmark.
 */

int check_heap(void)
{
	/*
	 * CHECK THE PHYSICAL HEAP LIST
	 */

	BlockHeader* current = list_head;

	while (current != NULL) {

		// Every block must have a valid size.
		if (current->size == 0) {
			fprintf(stderr,
				"check_heap: block has size 0\n");
			return 0;
		}

		// Free blocks must have free == 1.
		if (current->free != 0 &&
		    current->free != 1) {

			fprintf(stderr,
				"check_heap: invalid free flag\n");
			return 0;
		}

		current = current->next;
	}


	/*
	 * CHECK THE FREE LIST
	 */

	current = free_list_head;

	while (current != NULL) {

		// Every block on the free list must be marked free.
		if (current->free != 1) {
			fprintf(stderr,
				"check_heap: allocated block in free list\n");
			return 0;
		}

		/*
		 * If there is a next free block, its previous_free
		 * pointer must point back to the current block.
		 */
		if (current->next_free != NULL &&
		    current->next_free->previous_free != current) {

			fprintf(stderr,
				"check_heap: broken free-list link\n");
			return 0;
		}

		current = current->next_free;
	}


	return 1;
}

//for tests
//

int hypermalloc_get_sbrk_calls(void)
{
	return sbrk_calls;
}



