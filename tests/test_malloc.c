#include <stdio.h>

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

int main(){
	void* A = hypermalloc(100);
	void* B = hypermalloc(100);

	//print results to the console
	printf("the address of the beginning of the allocated block for A is: %p\n", A);
		printf("the address of the beginning of the allocated block for B is: %p\n", B);

	return 0;

}
