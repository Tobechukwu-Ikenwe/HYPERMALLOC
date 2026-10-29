#include "hypermalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NUM_BLOCKS 100000

static const size_t sizes[] = {
    64, 128, 256, 512, 1024, 4096
};

static double elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec)
         + (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

static double benchmark_hypermalloc(void)
{
    void *blocks[NUM_BLOCKS];
    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (size_t i = 0; i < NUM_BLOCKS; i++) {
        blocks[i] = hyper_malloc(sizes[i % 6]);
    }

    for (size_t i = 0; i < NUM_BLOCKS; i++) {
        hyper_free(blocks[i]);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    return elapsed_seconds(start, end);
}

static double benchmark_malloc(void)
{
    void *blocks[NUM_BLOCKS];
    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (size_t i = 0; i < NUM_BLOCKS; i++) {
        blocks[i] = malloc(sizes[i % 6]);
    }

    for (size_t i = 0; i < NUM_BLOCKS; i++) {
        free(blocks[i]);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    return elapsed_seconds(start, end);
}

int main(void)
{
    printf("===== MIXED-SIZE BENCHMARK =====\n");
    printf("Blocks: %d\n", NUM_BLOCKS);
    printf("Sizes: 64, 128, 256, 512, 1024, 4096 bytes\n\n");

    printf("glibc malloc/free: %.6f seconds\n",
           benchmark_malloc());

    printf("HyperMalloc: %.6f seconds\n",
           benchmark_hypermalloc());

    return 0;
}
