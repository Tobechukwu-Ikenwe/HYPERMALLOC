#include "hypermalloc.h"

#include <stdio.h>
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

int main(void)
{
    void *blocks[NUM_BLOCKS];
    struct timespec start, end;

    for (size_t i = 0; i < NUM_BLOCKS; i++) {
        blocks[i] = hyper_malloc(sizes[i % 6]);
    }

    for (size_t i = 0; i < NUM_BLOCKS; i++) {
        hyper_free(blocks[i]);
    }

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (size_t i = 0; i < NUM_BLOCKS; i++) {
        blocks[i] = hyper_malloc(sizes[i % 6]);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    printf("===== MIXED-SIZE REUSE BENCHMARK =====\n");
    printf("Blocks: %d\n", NUM_BLOCKS);
    printf("Sizes: 64, 128, 256, 512, 1024, 4096 bytes\n\n");
    printf("HyperMalloc reuse allocation: %.6f seconds\n",
           elapsed_seconds(start, end));

    for (size_t i = 0; i < NUM_BLOCKS; i++) {
        hyper_free(blocks[i]);
    }

    return 0;
}
