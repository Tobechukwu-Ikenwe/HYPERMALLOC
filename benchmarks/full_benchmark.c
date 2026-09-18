#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "hypermalloc.h"


/*
 * Purpose:
 * Measures allocation, reallocation, and deallocation performance
 * for libc malloc and HyperMalloc using the same workload.
 */

#define NUM_ALLOCS 100000
#define BLOCK_SIZE 256


// Calculate elapsed time between two timestamps in seconds.
static double elapsed_seconds(struct timespec start, struct timespec end)
{
	double seconds = (double)(end.tv_sec - start.tv_sec);

	seconds += (double)(end.tv_nsec - start.tv_nsec) / 1e9;

	return seconds;
}


/*
 * Benchmark libc malloc, realloc, and free.
 */
static void benchmark_malloc(void)
{
	struct timespec start;
	struct timespec end;

	void *blocks[NUM_ALLOCS];


	/*
	 * ALLOCATION
	 */

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (size_t i = 0; i < NUM_ALLOCS; i++) {
		blocks[i] = malloc(BLOCK_SIZE);

		if (blocks[i] == NULL) {
			fprintf(stderr,
				"malloc failed at allocation %zu\n", i);
			exit(EXIT_FAILURE);
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &end);

	printf("malloc allocation: %.6f seconds\n",
	       elapsed_seconds(start, end));


	/*
	 * REALLOCATION
	 */

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (size_t i = 0; i < NUM_ALLOCS; i++) {
		void *new_ptr = realloc(blocks[i], BLOCK_SIZE * 2);

		if (new_ptr == NULL) {
			fprintf(stderr,
				"realloc failed at allocation %zu\n", i);
			exit(EXIT_FAILURE);
		}

		blocks[i] = new_ptr;
	}

	clock_gettime(CLOCK_MONOTONIC, &end);

	printf("malloc realloc: %.6f seconds\n",
	       elapsed_seconds(start, end));


	/*
	 * FREE
	 */

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (size_t i = 0; i < NUM_ALLOCS; i++) {
		free(blocks[i]);
	}

	clock_gettime(CLOCK_MONOTONIC, &end);

	printf("malloc free: %.6f seconds\n",
	       elapsed_seconds(start, end));
}


/*
 * Benchmark HyperMalloc allocation, reallocation, and free.
 */
static void benchmark_hypermalloc(void)
{
	struct timespec start;
	struct timespec end;

	void *blocks[NUM_ALLOCS];


	/*
	 * ALLOCATION
	 */

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (size_t i = 0; i < NUM_ALLOCS; i++) {
		blocks[i] = hyper_malloc(BLOCK_SIZE);

		if (blocks[i] == NULL) {
			fprintf(stderr,
				"hyper_malloc failed at allocation %zu\n", i);
			exit(EXIT_FAILURE);
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &end);

	printf("hyper_malloc allocation: %.6f seconds\n",
	       elapsed_seconds(start, end));


	/*
	 * REALLOCATION
	 */

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (size_t i = 0; i < NUM_ALLOCS; i++) {
		void *new_ptr =
			hyper_realloc(blocks[i], BLOCK_SIZE * 2);

		if (new_ptr == NULL) {
			fprintf(stderr,
				"hyper_realloc failed at allocation %zu\n", i);
			exit(EXIT_FAILURE);
		}

		blocks[i] = new_ptr;
	}

	clock_gettime(CLOCK_MONOTONIC, &end);

	printf("hyper_realloc: %.6f seconds\n",
	       elapsed_seconds(start, end));


	/*
	 * FREE
	 */

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (size_t i = 0; i < NUM_ALLOCS; i++) {
		hyper_free(blocks[i]);
	}

	clock_gettime(CLOCK_MONOTONIC, &end);

	printf("hyper_free: %.6f seconds\n",
	       elapsed_seconds(start, end));
}


int main(void)
{
	printf("\n===== C MEMORY API =====\n");

	benchmark_malloc();

	printf("\n===== HYPERMALLOC =====\n");

	benchmark_hypermalloc();

	return 0;
}
