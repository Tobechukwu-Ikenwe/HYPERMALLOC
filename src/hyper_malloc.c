#include <unistd.h>
// sbrk() gives us memory from the operating system.

#include <stdio.h>
// Standard input/output functions such as printf() and fprintf().

#include <errno.h>
// Error codes such as EINVAL.

#include "hypermalloc.h"
// My HyperMalloc API.
//
#include <limits.h>

#include <stdint.h>


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


// MAGIC VALUE TO IDENTIFY BLOCKS THAT WE ALLOCATED
#define HYPERMALLOC_MAGIC 0xDEADBEEFCAFEBABEULL


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

	// 64 bit magic value
	uint64_t magic;

} BlockHeader;


// create the head of our physical heap list.
// this is the entry point for all blocks.
static BlockHeader* list_head = NULL;



/*
 * BITMAP OF NON-EMPTY FREE LISTS
 *
 * Bit i is 1 when free_lists[i] contains at least one block.
 *
 * This lets find_free_block() determine which size class
 * contains a free block without scanning empty lists.
 */
static uint32_t free_list_bitmap = 0;


// Number of segregated free-list size classes.
#define NUM_SIZE_CLASSES 32

// Each entry points to the first free block
// belonging to that size class.
//
// Example:
//
//     free_lists[0] -> 16-byte blocks
//     free_lists[1] -> 32-byte blocks
//     free_lists[2] -> 64-byte blocks
//     free_lists[3] -> 128-byte blocks
//     ...
//
// Each individual list is doubly linked using:
//     previous_free
//     next_free
//
static BlockHeader* free_lists[NUM_SIZE_CLASSES] = { NULL };


static size_t get_size_class(size_t size)
{
	// Size 0 and 1 belong to the first class.
	if (size <= 1) {
		return 0;
	}

	/*
	 * Find the position of the highest set bit in
	 * (size - 1).
	 *
	 * The number of leading zero bits tells us where
	 * that highest bit is located.
	 */
	return sizeof(size_t) * CHAR_BIT
	       - __builtin_clzl(size - 1)
	       - 1;
}


// pointer to the end of the physical heap.
static BlockHeader* heap_tail = NULL;


// the pointer to a block header is exacly one block header behind the ptr to the user memory
//CALCULATE BLOCKHEADER FROM POINTER
//

static BlockHeader* calculate_header(void* ptr){
	return (BlockHeader*)((char*)ptr - sizeof(BlockHeader));
}

/*
 *CHECK WHETHER POINTER IS INSIDE THE HEAP
 USED TO ELIMINATE INVALID POINTERS THAT OUR ALLOCATOR DID NOT RETURN
 * */


static int validate_ptr(void* ptr){
	// is ptr inside the heap
	if (ptr < heap_start || ptr >= heap_end){
		return 0;
	}
	//is it a pointer that hypermalloc returned 
	// calculate blockheader from the pointer
	
	BlockHeader* block = calculate_header(ptr);
	if (block->magic != HYPERMALLOC_MAGIC){
		return 0;
	}

	//success
	return 1;
}

/*
 * FIND A BLOCK
 *
 * Search the physical heap list for the now allocated block that owns
 * the pointer returned to the user.
 *
 * The user receives:
 *
 *     [BlockHeader][user memory]
 *                  ^
 *                  ptr
 */

static BlockHeader* find_block(void* ptr)
{
	//validate the pointer being passed in
	if (validate_ptr(ptr)){
		// return the pointer to the blockheader
		return calculate_header(ptr);
	}
	//throw an error
	return NULL;
	

}

/*
 * ADD A BLOCK TO A SEGREGATED FREE LIST
 *
 * Insert the block at the front of the free list
 * corresponding to its size class.
 *
 * Example:
 *
 *     block->size = 256
 *            ↓
 *     get_size_class(256)
 *            ↓
 *         class 8
 *            ↓
 *     free_lists[8]
 *
 *
 * Each size class has its own doubly linked list.
 *
 * The bitmap records that this size class is non-empty.
 */

static void insert_into_free_list(BlockHeader* block)
{
	// Determine which size class this block belongs to.
	size_t class = get_size_class(block->size);

	// Get the current head of this size class.
	BlockHeader* head = free_lists[class];

	// The new block becomes the first block in the list.
	block->previous_free = NULL;

	// Connect the new block to the old head.
	block->next_free = head;

	// If the old head exists, connect it back to the new block.
	if (head != NULL) {
		head->previous_free = block;
	}

	// Make block the new head of this size class.
	free_lists[class] = block;

	/*
	 * Mark this size class as non-empty.
	 *
	 * Example:
	 *
	 *     class = 8
	 *
	 *     00000000000000000000000100000000
	 *                            ↑
	 *                         bit 8 = 1
	 */
	free_list_bitmap |= (1U << class);
}


/*
 * REMOVE A BLOCK FROM A SEGREGATED FREE LIST
 *
 * Remove block from the doubly linked list belonging
 * to its size class.
 *
 * Because every block stores previous_free and next_free,
 * removal is O(1).
 *
 * If removing the final block from a size class,
 * clear that class's bit in free_list_bitmap.
 */

static void remove_from_free_list(BlockHeader* block)
{
	// Determine which size class this block belongs to.
	size_t class = get_size_class(block->size);

	/*
	 * If there is a previous free block,
	 * connect it directly to the next free block.
	 */
	if (block->previous_free != NULL) {

		block->previous_free->next_free =
			block->next_free;

	} else {

		/*
		 * No previous block means block is the head
		 * of this size class.
		 */
		free_lists[class] = block->next_free;
	}

	/*
	 * If there is a next free block,
	 * connect it back to the previous free block.
	 */
	if (block->next_free != NULL) {

		block->next_free->previous_free =
			block->previous_free;
	}

	/*
	 * Remove block from the free list completely.
	 */
	block->previous_free = NULL;
	block->next_free = NULL;

	/*
	 * If this class is now empty, clear its bitmap bit.
	 */
	if (free_lists[class] == NULL) {

		free_list_bitmap &= ~(1U << class);
	}
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

	// set magic 
	h->magic = HYPERMALLOC_MAGIC;

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
 * Search the free list bucket to find the appropriate size
 *
 */


/*
 * FIND A SUITABLE FREE BLOCK
 *
 * Find a free block large enough for user_size.
 *
 * The allocator uses:
 *
 *     1. get_size_class()
 *          O(1) class calculation using CLZ.
 *
 *     2. free_list_bitmap
 *          Quickly identifies non-empty size classes.
 *
 *     3. free_lists[class]
 *          Direct access to the appropriate free list.
 *
 * We never scan smaller size classes because they cannot
 * satisfy the request.
 */

static BlockHeader* find_free_block(size_t user_size)
{
	/*
	 * Determine the smallest size class capable of
	 * satisfying the request.
	 */
	size_t class = get_size_class(user_size);

	/*
	 * Ignore all classes smaller than class.
	 *
	 * Shift the bitmap so that bit 0 represents our
	 * starting class.
	 */
	uint32_t available =
		free_list_bitmap >> class;

	/*
	 * No suitable free-list class contains a block.
	 */
	if (available == 0) {
		return NULL;
	}

	/*
	 * Find the first non-empty class at or above
	 * the requested class.
	 *
	 * __builtin_ctz() finds the first set bit.
	 */
	size_t offset = __builtin_ctz(available);

	size_t selected_class = class + offset;

	/*
	 * Because the bitmap guarantees that this list
	 * is non-empty, its head is immediately available.
	 */
	return free_lists[selected_class];
}
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
	 * SEARCH THE segregated  FREE LISTs
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
//
// static int expand_block(BlockHeader* block, size_t new_size)
// {
// 	// The block is already large enough. ideally realloc should not even let his happen
// 	if (new_size <= block->size) {
// 		return 1;
// 	}
//
// 	// Amount of additional payload required.
// 	size_t required = new_size - block->size;
//
// 	// Start with the physical block immediately after block.
// 	BlockHeader* next = block->next;
//
// 	// Keep track of how much neighboring memory we can consume.
// 	size_t available = 0;
//
// 	while (next != NULL && next->free == 1) {
//
// 		/*
// 		 * A free block contributes:
// 		 *
// 		 *     its payload
// 		 *     + its BlockHeader
// 		 *
// 		 * because once absorbed, its header becomes usable space.
// 		 */
//
// 		available += sizeof(BlockHeader) + next->size;
//
// 		if (available >= required) {
// 			break;
// 		}
//
// 		next = next->next;
// 	}
//
//
// 	// The adjacent free blocks were not large enough.
// 	if (available < required) {
// 		return 0;
// 	}
//
//
// 	/*
// 	 * ABSORB THE REQUIRED FREE BLOCKS
// 	 */
//
// 	next = block->next;
//
// 	while (block->size < new_size &&
// 	       next != NULL &&
// 	       next->free == 1) {
//
// 		// Save the block after next before removing next.
// 		BlockHeader* after = next->next;
//
// 		// Remove next from the free list.
// 		remove_from_free_list(next);
//
// 		// Absorb next into block.
// 		block->size += sizeof(BlockHeader) + next->size;
//
// 		// Connect block to the next physical block.
// 		block->next = after;
//
// 		// Connect the next physical block back to block.
// 		if (after != NULL) {
// 			after->previous = block;
// 		} else {
// 			// The absorbed block was the last physical block.
// 			heap_tail = block;
// 		}
//
// 		// Move forward.
// 		next = after;
//
//
// 	}
//
//
// 	// The block may have absorbed slightly more space than requested. so just set size to be safe
// 	block->size = new_size;
//
// 	return 1;
// }

static int expand_block(BlockHeader* block, size_t new_size)
{
	/*
	 * The block is already large enough.
	 */
	if (new_size <= block->size) {
		return 1;
	}

	/*
	 * Calculate how many additional payload bytes
	 * are required.
	 */
	size_t required = new_size - block->size;

	/*
	 * FIRST PASS
	 *
	 * Determine whether the consecutive free blocks
	 * immediately following `block` contain enough
	 * space.
	 *
	 * We do NOT modify anything during this pass.
	 */
	size_t available = 0;
	BlockHeader* next = block->next;

	while (next != NULL && next->free == 1) {

		/*
		 * When this block is absorbed, its header becomes
		 * usable space too.
		 */
		available += sizeof(BlockHeader) + next->size;

		/*
		 * Stop as soon as we know there is enough space.
		 */
		if (available >= required) {
			break;
		}

		next = next->next;
	}

	/*
	 * Not enough adjacent free memory.
	 *
	 * Nothing has been modified, so the caller can safely
	 * fall back to allocating a new block.
	 */
	if (available < required) {
		return 0;
	}

	/*
	 * SECOND PASS
	 *
	 * We already know enough space exists.
	 *
	 * Now absorb the consecutive free blocks.
	 */
	next = block->next;

	while (block->size < new_size) {

		/*
		 * `next` must be a free physical neighbor because
		 * the first pass proved enough space exists.
		 */
		BlockHeader* after = next->next;

		/*
		 * Remove the block from its segregated free list.
		 *
		 * This is O(1).
		 */
		remove_from_free_list(next);

		/*
		 * Absorb:
		 *
		 *     [next header][next payload]
		 *
		 * into the current block.
		 */
		block->size += sizeof(BlockHeader) + next->size;

		/*
		 * Remove `next` from the physical heap list.
		 */
		block->next = after;

		/*
		 * Repair the backward physical link.
		 */
		if (after != NULL) {
			after->previous = block;
		} else {
			/*
			 * The absorbed block was the last block
			 * in the physical heap.
			 */
			heap_tail = block;
		}

		next = after;
	}

	/*
	 * We may have absorbed slightly more memory than
	 * requested because blocks are indivisible.
	 *
	 * Expose exactly the requested payload size.
	 */
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
 * 4. new_size > old_size : adjacent free blocks provide enough space
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
	 * if not same or smaller,  TRY TO EXPAND IN PLACE
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
 * It will NOT be used inside the timed benchmark.
 */

// int check_heap(void)
// {
// 	/*
// 	 * CHECK THE PHYSICAL HEAP LIST
// 	 */
//
// 	BlockHeader* current = list_head;
//
// 	while (current != NULL) {
//
// 		// Every block must have a valid size.
// 		if (current->size == 0) {
// 			fprintf(stderr,
// 				"check_heap: block has size 0\n");
// 			return 0;
// 		}
//
// 		// Free blocks must have free == 1.
// 		if (current->free != 0 &&
// 		    current->free != 1) {
//
// 			fprintf(stderr,
// 				"check_heap: invalid free flag\n");
// 			return 0;
// 		}
//
// 		current = current->next;
// 	}
//
//
// 	/*
// 	 * CHECK THE FREE LIST
// 	 */
//
// 	current = free_list_head;
//
// 	while (current != NULL) {
//
// 		// Every block on the free list must be marked free.
// 		if (current->free != 1) {
// 			fprintf(stderr,
// 				"check_heap: allocated block in free list\n");
// 			return 0;
// 		}
//
// 		/*
// 		 * If there is a next free block, its previous_free
// 		 * pointer must point back to the current block.
// 		 */
// 		if (current->next_free != NULL &&
// 		    current->next_free->previous_free != current) {
//
// 			fprintf(stderr,
// 				"check_heap: broken free-list link\n");
// 			return 0;
// 		}
//
// 		current = current->next_free;
// 	}
//
//
// 	return 1;
// }

//for tests
//

int hypermalloc_get_sbrk_calls(void)
{
	return sbrk_calls;
}



