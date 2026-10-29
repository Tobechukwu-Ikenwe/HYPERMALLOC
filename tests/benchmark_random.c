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

static void shuffle(size_t *order)
{
    for (size_t i = NUM_BLOCKS - 1; i > 0; i--) {
        size_t j = (size_t)rand() % (i + 1);

        size_t temp = order[i];
        order[i] = order[j];
        order[j] = temp;
    }
}

static double benchmark_hypermalloc(size_t *order)
{
    void *blocks[NUM_BLOCKS];
    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (size_t i = 0; i < NUM_BLOCKS; i++) {
        blocks[i] = hyper_malloc(sizes[i % 6]);
    }

    for (size_t i = 0; i < NUM_BLOCKS; i++) {
        hyper_free(blocks[order[i]]);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    return elapsed_seconds(start, end);
}

static double benchmark_malloc(size_t *order)
{
    void *blocks[NUM_BLOCKS];
    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (size_t i = 0; i < NUM_BLOCKS; i++) {
        blocks[i] = malloc(sizes[i % 6]);
    }

    for (size_t i = 0; i < NUM_BLOCKS; i++) {
        free(blocks[order[i]]);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    return elapsed_seconds(start, end);
}

int main(void)
{
    size_t order[NUM_BLOCKS];

    for (size_t i = 0; i < NUM_BLOCKS; i++) {
        order[i] = i;
    }

    srand(42);
    shuffle(order);

    printf("===== RANDOMIZED ALLOCATION/FREE BENCHMARK =====\n");
    printf("Blocks: %d\n", NUM_BLOCKS);
    printf("Sizes: 64, 128, 256, 512, 1024, 4096 bytes\n");
    printf("Free order: randomized with fixed seed\n\n");

    printf("glibc malloc/free: %.6f seconds\n",
           benchmark_malloc(order));

    printf("HyperMalloc: %.6f seconds\n",
           benchmark_hypermalloc(order));

    return 0;
}
