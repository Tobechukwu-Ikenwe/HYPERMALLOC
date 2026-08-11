## Assignment 1 — Linux Memory Acquisition

Question 1

What is the program break?

This is the address marking the end of a process's heap

Question 2

What does sbrk(0) return?

It returns the current program break

Question 4

Is the address your process works with virtual or physical?

virtual

Question 5

How did your program verify that the memory was usable?

I wrote oxAA to all 4096 bytes i requested and confimrmed each byte was present using a loop and perror() to error check



## ASSIGNMENT 2
### 1. Why is requesting new memory from the OS for every allocation inefficient?

Because requesting memory from the OS has overhead, so repeatedly requesting memory is slower than reusing memory we already have.

### 2. What is a free list?

A free list is a linked list that keeps track of memory blocks that have been freed and are available for reuse.

### 3. Why does a free block need a `next` pointer?

So the allocator can link free blocks together and traverse the free list to find reusable memory.

### 4. What happens when the free list contains no suitable block?

The allocator requests more memory from the OS.

### 5. Why can a 200-byte free block satisfy a 100-byte request even though we're not splitting it yet?

Because the block is larger than the requested size, so the allocator can give the entire 200-byte block to the caller.

### 6. What is the performance advantage of reusing memory?

Reusing memory avoids unnecessary OS memory requests, making allocations faster and reducing overhead.

