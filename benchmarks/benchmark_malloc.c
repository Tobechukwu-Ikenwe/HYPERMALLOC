/*
 * HyperMalloc Performance Benchmark
 *
 * Compares HyperMalloc against the system's libc allocator
 * using the exact same allocation workload.
 *
 * Workloads:
 *   1. Allocation only
 *   2. Allocation + free
 *   3. Allocation + free + reuse
 *   4. Realloc growth
 *   5. Realloc shrink
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "hypermalloc.h"


/* Number of allocations performed in each workload. */
#define NUM_ALLOCS 100000

/* Range of random allocation sizes. */
#define MIN_SIZE 16
#define MAX_SIZE 4096


/*
 * Returns elapsed CPU time between two timestamps.
 */
static double elapsed_seconds(clock_t start, clock_t end)
{
    return (double)(end - start) / CLOCKS_PER_SEC;
}


/*
 * Generates the allocation sizes used by every benchmark.
 *
 * Both HyperMalloc and libc receive the exact same sizes.
 */
static void generate_sizes(size_t *sizes)
{
    for (size_t i = 0; i < NUM_ALLOCS; i++) {
        sizes[i] =
            MIN_SIZE +
            (size_t)(rand() % (MAX_SIZE - MIN_SIZE + 1));
    }
}


/*
 * Benchmark 1:
 *
 * Allocate NUM_ALLOCS blocks without freeing them.
 *
 * This measures the allocator's allocation path by itself.
 */
static double benchmark_hyper_malloc(const size_t *sizes)
{
    void *ptrs[NUM_ALLOCS];

    clock_t start = clock();

    for (size_t i = 0; i < NUM_ALLOCS; i++) {
        ptrs[i] = hyper_malloc(sizes[i]);
    }

    clock_t end = clock();

    /*
     * Free the blocks after timing so the cleanup cost
     * does not affect the allocation measurement.
     */
    for (size_t i = 0; i < NUM_ALLOCS; i++) {
        hyper_free(ptrs[i]);
    }

    return elapsed_seconds(start, end);
}


/*
 * libc version of the allocation-only benchmark.
 */
static double benchmark_libc_malloc(const size_t *sizes)
{
    void *ptrs[NUM_ALLOCS];

    clock_t start = clock();

    for (size_t i = 0; i < NUM_ALLOCS; i++) {
        ptrs[i] = malloc(sizes[i]);
    }

    clock_t end = clock();

    /* Cleanup happens outside the timed section. */
    for (size_t i = 0; i < NUM_ALLOCS; i++) {
        free(ptrs[i]);
    }

    return elapsed_seconds(start, end);
}


/*
 * Benchmark 2:
 *
 * Allocate and immediately free every block.
 *
 * This measures both sides of the allocator lifecycle.
 */
static double benchmark_hyper_malloc_free(const size_t *sizes)
{
    clock_t start = clock();

    for (size_t i = 0; i < NUM_ALLOCS; i++) {
        void *ptr = hyper_malloc(sizes[i]);

        if (ptr != NULL) {
            hyper_free(ptr);
        }
    }

    clock_t end = clock();

    return elapsed_seconds(start, end);
}


/*
 * libc version of allocation + free.
 */
static double benchmark_libc_malloc_free(const size_t *sizes)
{
    clock_t start = clock();

    for (size_t i = 0; i < NUM_ALLOCS; i++) {
        void *ptr = malloc(sizes[i]);

        if (ptr != NULL) {
            free(ptr);
        }
    }

    clock_t end = clock();

    return elapsed_seconds(start, end);
}


/*
 * Benchmark 3:
 *
 * Allocate many blocks, free them all, then allocate again.
 *
 * The second allocation phase tests whether the allocator
 * can efficiently reuse memory that was previously freed.
 */
static double benchmark_hyper_reuse(const size_t *sizes)
{
    void *ptrs[NUM_ALLOCS];

    clock_t start = clock();

    /* First allocation pass. */
    for (size_t i = 0; i < NUM_ALLOCS; i++) {
        ptrs[i] = hyper_malloc(sizes[i]);
    }

    /* Return every block to HyperMalloc's free lists. */
    for (size_t i = 0; i < NUM_ALLOCS; i++) {
        hyper_free(ptrs[i]);
    }

    /* Allocate the same workload again. */
    for (size_t i = 0; i < NUM_ALLOCS; i++) {
        ptrs[i] = hyper_malloc(sizes[i]);
    }

    clock_t end = clock();

    /* Cleanup outside the timed section. */
    for (size_t i = 0; i < NUM_ALLOCS; i++) {
        hyper_free(ptrs[i]);
    }

    return elapsed_seconds(start, end);
}


/*
 * libc version of the reuse benchmark.
 */
static double benchmark_libc_reuse(const size_t *sizes)
{
    void *ptrs[NUM_ALLOCS];

    clock_t start = clock();

    /* First allocation pass. */
    for (size_t i = 0; i < NUM_ALLOCS; i++) {
        ptrs[i] = malloc(sizes[i]);
    }

    /* Return every block to libc. */
    for (size_t i = 0; i < NUM_ALLOCS; i++) {
        free(ptrs[i]);
    }

    /* Allocate the same workload again. */
    for (size_t i = 0; i < NUM_ALLOCS; i++) {
        ptrs[i] = malloc(sizes[i]);
    }

    clock_t end = clock();

    /* Cleanup outside the timed section. */
    for (size_t i = 0; i < NUM_ALLOCS; i++) {
        free(ptrs[i]);
    }

    return elapsed_seconds(start, end);
}


/*
 * Benchmark 4:
 *
 * Allocate a small block and repeatedly grow it.
 *
 * This tests HyperMalloc's ability to expand allocations.
 */
static double benchmark_hyper_realloc_grow(void)
{
    clock_t start = clock();

    for (size_t i = 0; i < NUM_ALLOCS; i++) {

        size_t initial_size = 64;
        size_t new_size = 1024;

        void *ptr = hyper_malloc(initial_size);

        if (ptr == NULL) {
            continue;
        }

        void *new_ptr = hyper_realloc(ptr, new_size);

        if (new_ptr != NULL) {
            hyper_free(new_ptr);
        } else {
            hyper_free(ptr);
        }
    }

    clock_t end = clock();

    return elapsed_seconds(start, end);
}


/*
 * libc version of realloc growth.
 */
static double benchmark_libc_realloc_grow(void)
{
    clock_t start = clock();

    for (size_t i = 0; i < NUM_ALLOCS; i++) {

        size_t initial_size = 64;
        size_t new_size = 1024;

        void *ptr = malloc(initial_size);

        if (ptr == NULL) {
            continue;
        }

        void *new_ptr = realloc(ptr, new_size);

        if (new_ptr != NULL) {
            free(new_ptr);
        } else {
            free(ptr);
        }
    }

    clock_t end = clock();

    return elapsed_seconds(start, end);
}


/*
 * Benchmark 5:
 *
 * Allocate a large block and shrink it.
 *
 * HyperMalloc currently keeps the same allocation when
 * the requested size becomes smaller.
 */
static double benchmark_hyper_realloc_shrink(void)
{
    clock_t start = clock();

    for (size_t i = 0; i < NUM_ALLOCS; i++) {

        void *ptr = hyper_malloc(4096);

        if (ptr == NULL) {
            continue;
        }

        void *new_ptr = hyper_realloc(ptr, 64);

        if (new_ptr != NULL) {
            hyper_free(new_ptr);
        } else {
            hyper_free(ptr);
        }
    }

    clock_t end = clock();

    return elapsed_seconds(start, end);
}


/*
 * libc version of realloc shrink.
 */
static double benchmark_libc_realloc_shrink(void)
{
    clock_t start = clock();

    for (size_t i = 0; i < NUM_ALLOCS; i++) {

        void *ptr = malloc(4096);

        if (ptr == NULL) {
            continue;
        }

        void *new_ptr = realloc(ptr, 64);

        if (new_ptr != NULL) {
            free(new_ptr);
        } else {
            free(ptr);
        }
    }

    clock_t end = clock();

    return elapsed_seconds(start, end);
}


int main(void)
{
    /*
     * Generate the workload once.
     *
     * Every benchmark receives this exact same sequence.
     */
    size_t *sizes =
        malloc(NUM_ALLOCS * sizeof(size_t));

    if (sizes == NULL) {
        fprintf(stderr, "Failed to allocate benchmark sizes\n");
        return 1;
    }

    /*
     * Fixed seed makes the benchmark reproducible.
     */
    srand(42);

    generate_sizes(sizes);


    printf("HyperMalloc vs libc malloc benchmark\n");
    printf("=====================================\n");
    printf("Allocations per test: %d\n", NUM_ALLOCS);
    printf("Random allocation sizes: %d-%d bytes\n\n",
           MIN_SIZE,
           MAX_SIZE);


    /*
     * ---------------- Allocation Only ----------------
     */

    double hyper_malloc_time =
        benchmark_hyper_malloc(sizes);

    double libc_malloc_time =
        benchmark_libc_malloc(sizes);

    printf("Allocation only:\n");
    printf("  HyperMalloc: %.6f seconds\n", hyper_malloc_time);
    printf("  libc malloc: %.6f seconds\n\n", libc_malloc_time);


    /*
     * ---------------- Allocation + Free ----------------
     */

    double hyper_malloc_free_time =
        benchmark_hyper_malloc_free(sizes);

    double libc_malloc_free_time =
        benchmark_libc_malloc_free(sizes);

    printf("Allocation + free:\n");
    printf("  HyperMalloc: %.6f seconds\n", hyper_malloc_free_time);
    printf("  libc malloc: %.6f seconds\n\n", libc_malloc_free_time);


    /*
     * ---------------- Memory Reuse ----------------
     */

    double hyper_reuse_time =
        benchmark_hyper_reuse(sizes);

    double libc_reuse_time =
        benchmark_libc_reuse(sizes);

    printf("Allocation + free + reuse:\n");
    printf("  HyperMalloc: %.6f seconds\n", hyper_reuse_time);
    printf("  libc malloc: %.6f seconds\n\n", libc_reuse_time);


    /*
     * ---------------- Realloc Growth ----------------
     */

    double hyper_realloc_grow_time =
        benchmark_hyper_realloc_grow();

    double libc_realloc_grow_time =
        benchmark_libc_realloc_grow();

    printf("Realloc growth (64 -> 1024 bytes):\n");
    printf("  HyperMalloc: %.6f seconds\n", hyper_realloc_grow_time);
    printf("  libc realloc: %.6f seconds\n\n", libc_realloc_grow_time);


    /*
     * ---------------- Realloc Shrink ----------------
     */

    double hyper_realloc_shrink_time =
        benchmark_hyper_realloc_shrink();

    double libc_realloc_shrink_time =
        benchmark_libc_realloc_shrink();

    printf("Realloc shrink (4096 -> 64 bytes):\n");
    printf("  HyperMalloc: %.6f seconds\n", hyper_realloc_shrink_time);
    printf("  libc realloc: %.6f seconds\n", libc_realloc_shrink_time);


    /*
     * Free the memory used to store benchmark sizes.
     */
    free(sizes);

    return 0;
}
