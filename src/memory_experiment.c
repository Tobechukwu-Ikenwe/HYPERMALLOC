#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(){
	void *current_break = sbrk(4096); // save old brk and request new mem
	sleep(30);
	void *new_break = sbrk(0);// save current program break as new break 
	if (current_break == (void* )-1){
		perror("sbrk"); // prints out the os message for the error
		return 1;
	}
	printf("address of old program break is %p\n", current_break);
	printf("address of new program break is %p\n", new_break);

	memset(current_break, 0xAA, 4096); // 0xAA for 4096 bytes
	unsigned char *memory = current_break; 
	
	for (int i = 0; i < 4096; i++){
		if(memory[i] != 0xAA){
		fprintf(stderr, "Memory verification failed at byte %d\n", i);
       		return 1;
		}
				
	}
	printf("Memory Verification Passed!!!\n");

	//restore program break
	if (brk(current_break) == -1){
		write(STDOUT_FILENO, "Restoration FAILED\n", 19);
		return 1;
	}
	else{
		write(STDOUT_FILENO, "Restoration PASSED\n", 19);
	}

	// for program to stall while I use proc
}
