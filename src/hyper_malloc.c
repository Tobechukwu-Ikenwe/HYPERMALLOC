#include <unistd.h>
#include <stdio.h>

// make a free list to track each allocated memory block

//it stores metadata ie. the header for the allocated block of memory

typedef struct BlockHeader{
	// size of the block
	int size;
	// pointer to the next block of memory
	struct BlockHeader* next;
	// status is this block freeor not 
	int free;
}BlockHeader;

// create the head of our linked list
BlockHeader* list_head;

// the hypermalloc() function takes in a size and returns a pointer to the beginning of the allocated memory

void *hypermalloc(int user_size){

	// does the list have any blocks?
	if (list_head == NULL){
	void* allocated_start = sbrk(sizeof(BlockHeader) + user_size);

	//check if error
	
	if (allocated_start == (void*)-1){
		perror("sbrk");
		return NULL;
		}

	//track in list 
	// move allocated_start the size of 1 block header behind to store the block h
	//set free to 0
	BlockHeader *h = allocated_start;
	h->free = 0;
	h->size = user_size;
	h->next = NULL;
	list_head = h;

	//move up one block header to return the part of the block we can use.
	return (void*)(h+1);
	//

		
	}

	//if not null search list
	
	// search the free list for a block with a size >= user_size
	BlockHeader* current = list_head;
	while (current->next != NULL){
		if (current->size >= user_size || current->free == 1){
			// free suitable block return address  of the block
			// that is exactly one block header pass current which is start of allocated block
		printf("Allocated %d bytes successfully \n", user_size);
		// set free to 0 and remove from free 
		current->free = 0;
		return (void*)(current + 1);

		}else{
			current = current->next;
		}

	}

	//if we search the list and find nothing:ask the OS to allocate more data an dadd it to free list
	void* allocated_extra = sbrk(sizeof(BlockHeader) + user_size);

	// check if allocation worked
	if (allocated_extra == (void*)-1){
		perror("sbrk");
		return NULL;
	}
	//return the allocated start pointer and add the header information to the linked list
	BlockHeader* h = allocated_extra;
	current->next = h; // add to current the end of our list
	h->size = user_size;
	h->next = NULL;
	return (void*)(h+1);


	}
