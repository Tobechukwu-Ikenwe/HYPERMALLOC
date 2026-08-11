#include <stdio.h>
#include <string.h>

// test allocations of 
/*
 *
 * allocation of 1 byte
 * allocation of 10 bytes
 * allocation of 100 bytes
 * allocation of 4096 bytes
 * multiple allocations
 * writing to allocated memory
 * reading it back
 *
 * */

void* hypermalloc(int user_size);
void hyper_free(void* ptr);
int main(){
	void* A = hypermalloc(100);
	void* B = hypermalloc(100);
	void* C;


	printf("the address of the beginning of the allocated block for A is: %p\n", A);
	printf("the address of the beginning of the allocated block for B is: %p\n", B);



	hyper_free(A);

	printf("ALLOCATE C");

	C = hypermalloc(100);

		// print c
	 	printf("the address of the beginning of the allocated block for C is: %p\n", C);
		


	// test thatB remainsvalid
	printf("Check if B is still valid after freeing A\n");

	printf("the address of the beginning of the allocated block for B is: %p\n", B);

	//write to B and verify its contents
	//
	//convert b to bytes
	unsigned char* B_bytes  = B;
	memset(B_bytes, 0xAA, 100);
	for (int i = 0; i < 100; i++){

		if (B_bytes[i] != 0xAA){
			fprintf(stderr, "Memory verification failed at byte %d\n.", i);
			return 1;
		}
		


	
	}

	printf("Memory write to B is successful\n");






	//make sure it does not crash

	hyper_free(NULL);

	
	



	return 0;

}
