#include <stdio.h>
// Standard input/output functions such as printf() and fprintf().

#include <stdlib.h>
// Standard library functions such as malloc(), realloc(), free(), and exit().

#include <time.h>
// Timing functions such as clock_gettime().

#include "hypermalloc.h"
// My HyperMalloc API.


// Calculate the elapsed time between two timestamps in seconds.
static double elapsed_seconds(struct timespec start, struct timespec end)
{
	// Difference in whole seconds.
	double seconds = (double)(end.tv_sec - start.tv_sec);

	// Add the difference in nanoseconds converted to seconds.
	seconds += (double)(end.tv_nsec - start.tv_nsec) / 1e9;

	return seconds;
}


// Number of memory blocks each allocator will allocate during the benchmark.
#define NUM_ALLOCS 100000

// Size of each individual memory block in bytes.
#define BLOCK_SIZE 256


/*
 * BENCHMARK SYSTEM MALLOC
 *
 * Performs the same allocation, reallocation, and free
 * workload that HyperMalloc will perform.
 */

static void benchmark_malloc(void)
{
	struct timespec start;
	struct timespec end;

	// Store every pointer returned by malloc().
	// We need these later so we can realloc() and free() them.
	void *blocks[NUM_ALLOCS];


	/*
	 * ALLOCATION
	 */

	// Record the time immediately before allocations begin.
	clock_gettime(CLOCK_MONOTONIC, &start);

	// Allocate NUM_ALLOCS blocks.
	for (size_t i = 0; i < NUM_ALLOCS; i++) {

		blocks[i] = malloc(BLOCK_SIZE);

		// Stop if malloc() fails.
		if (blocks[i] == NULL) {
			fprintf(stderr,
				"malloc failed at allocation %zu\n", i);
			exit(EXIT_FAILURE);
		}
	}

	// Record the time immediately after allocations finish.
	clock_gettime(CLOCK_MONOTONIC, &end);

	printf("malloc allocation: %.6f seconds\n",
	       elapsed_seconds(start, end));


	/*
	 * REALLOC
	 */

	// Record the time immediately before reallocations begin.
	clock_gettime(CLOCK_MONOTONIC, &start);

	// Resize every allocation to twice its original size.
	for (size_t i = 0; i < NUM_ALLOCS; i++) {

		void *new_ptr =
			realloc(blocks[i], BLOCK_SIZE * 2);

		// Stop if realloc() fails.
		if (new_ptr == NULL) {
			fprintf(stderr,
				"realloc failed at allocation %zu\n", i);
			exit(EXIT_FAILURE);
		}

		// realloc() may have moved the allocation.
		blocks[i] = new_ptr;
	}

	// Record the time immediately after reallocations finish.
	clock_gettime(CLOCK_MONOTONIC, &end);

	printf("malloc realloc: %.6f seconds\n",
	       elapsed_seconds(start, end));


	/*
	 * FREE
	 */

	// Record the time immediately before frees begin.
	clock_gettime(CLOCK_MONOTONIC, &start);

	// Free every allocation.
	for (size_t i = 0; i < NUM_ALLOCS; i++) {
		free(blocks[i]);
	}

	// Record the time immediately after frees finish.
	clock_gettime(CLOCK_MONOTONIC, &end);

	printf("malloc free: %.6f seconds\n",
	       elapsed_seconds(start, end));
}


/*
 * BENCHMARK HYPERMALLOC
 *
 * Performs the exact same workload as benchmark_malloc().
 */

static void benchmark_hypermalloc(void)
{
	struct timespec start;
	struct timespec end;

	// Store every pointer returned by hyper_malloc().
	// We need these later so we can hyper_realloc() and hyper_free() them.
	void *blocks[NUM_ALLOCS];


	/*
	 * ALLOCATION
	 */

	// Record the time immediately before allocations begin.
	clock_gettime(CLOCK_MONOTONIC, &start);

	// Allocate NUM_ALLOCS blocks.
	for (size_t i = 0; i < NUM_ALLOCS; i++) {

		blocks[i] = hyper_malloc(BLOCK_SIZE);

		// Stop if HyperMalloc fails.
		if (blocks[i] == NULL) {
			fprintf(stderr,
				"hyper_malloc failed at allocation %zu\n", i);
			exit(EXIT_FAILURE);
		}
	}

	// Record the time immediately after allocations finish.
	clock_gettime(CLOCK_MONOTONIC, &end);

	printf("hyper_malloc allocation: %.6f seconds\n",
	       elapsed_seconds(start, end));


	/*
	 * REALLOC
	 */

	// Record the time immediately before reallocations begin.
	clock_gettime(CLOCK_MONOTONIC, &start);

	// Resize every allocation to twice its original size.
	for (size_t i = 0; i < NUM_ALLOCS; i++) {

		void *new_ptr =
			hyper_realloc(blocks[i], BLOCK_SIZE * 2);

		// Stop if HyperMalloc fails.
		if (new_ptr == NULL) {
			fprintf(stderr,
				"hyper_realloc failed at allocation %zu\n", i);
			exit(EXIT_FAILURE);
		}

		// hyper_realloc() may have moved the allocation.
		blocks[i] = new_ptr;
	}

	// Record the time immediately after reallocations finish.
	clock_gettime(CLOCK_MONOTONIC, &end);

	printf("hyper_realloc: %.6f seconds\n",
	       elapsed_seconds(start, end));


	/*
	 * FREE
	 */

	// Record the time immediately before frees begin.
	clock_gettime(CLOCK_MONOTONIC, &start);

	// Free every allocation.
	for (size_t i = 0; i < NUM_ALLOCS; i++) {
		hyper_free(blocks[i]);
	}

	// Record the time immediately after frees finish.
	clock_gettime(CLOCK_MONOTONIC, &end);

	printf("hyper_free: %.6f seconds\n",
	       elapsed_seconds(start, end));
}


/*
 * MAIN
 *
 * Compare the standard C memory API against HyperMalloc
 * using the exact same workload.
 */

int main(void)
{
	printf("\n===== C MEMORY API =====\n");

	benchmark_malloc();


	printf("\n===== HYPERMALLOC =====\n");

	benchmark_hypermalloc();


	return 0;
}
