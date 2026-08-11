#include <unistd.h>
#include <stdio.h>

// make a free list to track each allocated memory block

//it stores metadata ie. the header for the allocated block of memory

typedef struct BlockHeader{
	// size of the block
	int size;

	// pointer to the next block of memory
	struct BlockHeader* next;

	// status is this block free or not 
	int free;

}BlockHeader;

//create the head of our free list (a singly linked list we use to manage free blocks)
BlockHeader* free_list_head;

// create a pointer previous to track the previous value in our free list
BlockHeader* previous = NULL;


// static variable to meausre number of sbrk calls for testing
static int sbrk_calls = 0;

// the hypermalloc() function takes in a size and returns a pointer to the beginning of the allocated memory

void *hypermalloc(int user_size){

	// Are there any blocks on our free list?
	if (free_list_head == NULL){
	void* allocated_start = sbrk(sizeof(BlockHeader) + user_size);

	//check if error
	
	if (allocated_start == (void*)- 1){
		perror("sbrk");
		return NULL;
	}

	//increment sbrk counter for the benchmark file
	sbrk_calls++;

	//track in list 
	// move allocated_start the size of 1 block header behind to store the block h
	//set free to 0
	BlockHeader *h = allocated_start;
	h->free = 0;
	h->size = user_size;
	h->next = NULL;

	//move up one block header to return the part of the block we can use.
	printf("Allocated %d bytes successfully \n", user_size);

	return (void*)(h+1);


		
	}

	/*
	 *IF HEAD IS NOT NULL THEN SEARCH THE CURRENT FREE LIST
	 * */
	
	// search the free list for a block with a size >= user_size + BLOCK header size
	BlockHeader* current = free_list_head;

	while (current != NULL){
		// set up the minimum size to split
		// user size + sizeofblockheader + 1byte payload 
		if (current->size >= (user_size + sizeof(BlockHeader) + 1) && current->free == 1){
			// free suitable block hence return address  of the block
			// we removeonly the user size amount add a new blockheader and set the rest as free
			
			current->free = 0;

			//split the remaining free data 
			//new free header containing the size of the remaining data that was not allocated
			//we do this to have more free memory for future allocations
			BlockHeader* h = (BlockHeader*)((char*)(current + 1) + user_size);
			h->free = 1;
			h->size = current->size - user_size - sizeof(BlockHeader);
			

			// add h to free list 
			h->next = free_list_head;
			free_list_head = h;

			printf("Allocated %d bytes successfully \n", user_size);


				// check if this is the list head ie. previous is null
			if (previous == NULL){
				// set the head as the next value and remove from list
				free_list_head = current->next;
				
			}
			else{
				//set the next pointer of previous to point to next of current removing the current current from linked list
				previous->next = current->next;
			}


			//return the pointer
			return (void*)(current + 1);

		}

		else if (current->size == user_size && current->free == 1){
			// allocate normally
			current-> free = 0;

				// check if this is the list head ie. previous is null
			if (previous == NULL){
				// set the head as the next value and remove from list
				free_list_head = current->next;
				
			}
			else{
				//set the next pointer of previous to point to next of current removing the current current from linked list
				previous->next = current->next;
			}


			//return the pointer
			return (void*)(current + 1);



		}

		
	
			
		// if block not free move to next block
		current = current->next;
	
	}

		//if we search the list and find nothing:ask the OS to allocate more data an dadd 		it to free list
	void* allocated_extra = sbrk(sizeof(BlockHeader) + user_size);


	// check if allocation worked
	if (allocated_extra == (void*)-1){
		perror("sbrk");
		return NULL;
	}

	sbrk_calls++;

	//return the allocated start pointer and add the header information to the linked list
	BlockHeader* h = allocated_extra;
	h->size = user_size;
	h->next = NULL;
	printf("Allocated %d bytes successfully \n", user_size);
	//return one blockheader past h
	return (void*)(h+1);



	}

              





// takes in the pointer returned by hypermalloc and frees the allocated block
void hyper_free(void* ptr){



	if (ptr == NULL){
		fprintf(stderr, "Pass in a valid pointer to a memory region\n");
		return;
	}

	


	//go a block header behind pointer to see the metadata for the block
	BlockHeader* h = (BlockHeader*)(ptr - 1 * (sizeof(BlockHeader)));

	//set free to 1
	
	h->free = 1;
	//set it as the new list head for ease of search for new memory
	//move original list_head to be after h
	h->next = free_list_head;
	free_list_head = h;
	printf("Memory at address %p has been freed\n.", ptr);

	



	
}

int hypermalloc_get_sbrk_calls(void){
	return sbrk_calls;
}

