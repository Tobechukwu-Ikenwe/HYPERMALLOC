#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <string.h>

#include "hyper_malloc.h"

/*
 * ============================================================
 * HyperMalloc Benchmark
 * ============================================================
 *
 * Purpose:
 *
 * Compare the performance of:
 *
 *      HyperMalloc
 *          vs.
 *      glibc malloc()
 *
 * under the exact same workloads.
 *
 * We are measuring ALLOCATOR performance, so the workload
 * must be identical regardless of which allocator we use.
 *
 * The benchmark currently focuses on:
 *
 *      1. Allocation + deallocation throughput
 *      2. Different allocation sizes
 *      3. Repeated allocations
 *
 * Later we will add:
 *
 *      - randomized allocation lifetimes
 *      - fragmentation workloads
 *      - perf analysis
 *      - cache/branch measurements
 * ============================================================
 */


/*
 * Number of blocks that remain allocated during a workload.
 *
 * Keeping many allocations alive simultaneously is more
 * realistic than simply doing:
 *
 *      malloc()
 *      free()
 *      malloc()
 *      free()
 *
 * because the allocator has to manage multiple blocks.
 */
#define NUM_BLOCKS 10000


/*
 * Number of times we repeat the entire allocation/free cycle.
 *
 * 10,000 blocks × 100 rounds = 1,000,000 allocations.
 */
#define NUM_ROUNDS 100


/*
 * Different allocation sizes that we want to test.
 *
 * Allocators can behave very differently for:
 *
 *      tiny allocations
 *      small allocations
 *      page-sized allocations
 *      large allocations
 */
#define NUM_SIZES 5

static const size_t ALLOCATION_SIZES[NUM_SIZES] = {
    16,
    64,
    256,
    4096,
    65536
};


/*
 * ------------------------------------------------------------
 * Timing helper
 * ------------------------------------------------------------
 *
 * clock_gettime() gives us a high-resolution monotonic clock.
 *
 * CLOCK_MONOTONIC is important because we are measuring
 * elapsed time.
 *
 * We don't want changes to the system clock to affect
 * our measurements.
 */
static double get_time_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (double)ts.tv_sec +
           (double)ts.tv_nsec / 1000000000.0;
}


/*
 * ------------------------------------------------------------
 * Memory initialization
 * ------------------------------------------------------------
 *
 * We don't want to benchmark allocations that are never used.
 *
 * After allocating memory, we write to it.
 *
 * This makes the benchmark closer to what an actual program
 * would do with allocated memory.
 */
static void touch_memory(void **blocks,
                         size_t num_blocks,
                         size_t size)
{
    for (size_t i = 0; i < num_blocks; i++) {

        /*
         * Treat the allocation as bytes so we can write
         * directly into the allocated memory.
         */
        unsigned char *memory = blocks[i];

        /*
         * Write a predictable value into every byte.
         */
        for (size_t j = 0; j < size; j++) {
            memory[j] = (unsigned char)(j & 0xFF);
        }
    }
}


/*
 * ------------------------------------------------------------
 * HyperMalloc benchmark
 * ------------------------------------------------------------
 *
 * This function performs the SAME workload that the libc
 * benchmark performs below.
 *
 * The only difference is:
 *
 *      hyper_malloc()
 *      hyper_free()
 *
 * instead of:
 *
 *      malloc()
 *      free()
 */
static double benchmark_hypermalloc(size_t size)
{
    /*
     * Store pointers to every active allocation.
     *
     * This allows us to allocate many blocks first and
     * free them afterward.
     */
    void *blocks[NUM_BLOCKS];

    double start = get_time_seconds();


    /*
     * --------------------------------------------------------
     * Allocation phase
     * --------------------------------------------------------
     *
     * Allocate NUM_BLOCKS blocks of the requested size.
     */
    for (int round = 0; round < NUM_ROUNDS; round++) {

        for (size_t i = 0; i < NUM_BLOCKS; i++) {

            blocks[i] = hyper_malloc(size);

            if (blocks[i] == NULL) {
                fprintf(stderr,
                        "HyperMalloc allocation failed\n");

                exit(EXIT_FAILURE);
            }
        }


        /*
         * Actually touch the allocated memory.
         */
        touch_memory(blocks, NUM_BLOCKS, size);


        /*
         * ----------------------------------------------------
         * Free phase
         * ----------------------------------------------------
         *
         * Every allocation created above is returned to
         * HyperMalloc.
         */
        for (size_t i = 0; i < NUM_BLOCKS; i++) {
            hyper_free(blocks[i]);
        }
    }


    double end = get_time_seconds();

    return end - start;
}


/*
 * ------------------------------------------------------------
 * glibc malloc benchmark
 * ------------------------------------------------------------
 *
 * This performs EXACTLY the same workload as the HyperMalloc
 * benchmark.
 *
 * The only difference is the allocator being tested.
 */
static double benchmark_libc_malloc(size_t size)
{
    void *blocks[NUM_BLOCKS];

    double start = get_time_seconds();


    for (int round = 0; round < NUM_ROUNDS; round++) {

        /*
         * Allocate the same number of blocks.
         */
        for (size_t i = 0; i < NUM_BLOCKS; i++) {

            blocks[i] = malloc(size);

            if (blocks[i] == NULL) {
                fprintf(stderr,
                        "malloc allocation failed\n");

                exit(EXIT_FAILURE);
            }
        }


        /*
         * Touch the exact same amount of memory.
         */
        touch_memory(blocks, NUM_BLOCKS, size);


        /*
         * Free every allocation.
         */
        for (size_t i = 0; i < NUM_BLOCKS; i++) {
            free(blocks[i]);
        }
    }


    double end = get_time_seconds();

    return end - start;
}


/*
 * ------------------------------------------------------------
 * Main
 * ------------------------------------------------------------
 */
int main(void)
{
    printf("============================================\n");
    printf("        HyperMalloc Benchmark v1\n");
    printf("============================================\n\n");

    printf("Blocks per round : %d\n", NUM_BLOCKS);
    printf("Rounds           : %d\n", NUM_ROUNDS);
    printf("Total allocations: %d\n\n",
           NUM_BLOCKS * NUM_ROUNDS);


    /*
     * --------------------------------------------------------
     * Test every allocation size.
     * --------------------------------------------------------
     */
    for (int i = 0; i < NUM_SIZES; i++) {

        size_t size = ALLOCATION_SIZES[i];

        printf("--------------------------------------------\n");
        printf("Allocation size: %zu bytes\n", size);
        printf("--------------------------------------------\n");


        /*
         * Run HyperMalloc.
         */
        double hyper_time =
            benchmark_hypermalloc(size);


        /*
         * Run libc malloc().
         */
        double libc_time =
            benchmark_libc_malloc(size);


        /*
         * Calculate the total number of allocator
         * operations.
         *
         * Each allocation is paired with a free.
         *
         * Therefore:
         *
         * allocations = NUM_BLOCKS × NUM_ROUNDS
         *
         * allocator operations =
         * allocations × 2
         */
        double operations =
            (double)NUM_BLOCKS *
            NUM_ROUNDS *
            2.0;


        /*
         * Calculate operations per second.
         */
        double hyper_ops_per_sec =
            operations / hyper_time;

        double libc_ops_per_sec =
            operations / libc_time;


        /*
         * Print raw timing results.
         */
        printf("HyperMalloc time : %.6f seconds\n",
               hyper_time);

        printf("glibc malloc time: %.6f seconds\n",
               libc_time);


        /*
         * Print throughput.
         */
        printf("HyperMalloc      : %.2f M ops/sec\n",
               hyper_ops_per_sec / 1000000.0);

        printf("glibc malloc     : %.2f M ops/sec\n",
               libc_ops_per_sec / 1000000.0);


        /*
         * Compare the two allocators.
         *
         * Example:
         *
         *      0.50
         *
         * means HyperMalloc is operating at 50% of the
         * throughput of glibc for this workload.
         */
        printf("Relative speed   : %.2f%%\n",
               (hyper_ops_per_sec /
                libc_ops_per_sec) * 100.0);

        printf("\n");
    }


    return 0;
}
