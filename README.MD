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




