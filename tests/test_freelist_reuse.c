#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "hypermalloc.h"
#include "internal.h"

#define NUM_BLOCKS 10000
#define ALLOCATION_SIZE 256

static double elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec)
         + (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

int main(void)
{
    void *blocks[NUM_BLOCKS];
    struct timespec start;
    struct timespec end;

    /*
     * Initial allocations populate the physical heap.
     */
    for (int i = 0; i < NUM_BLOCKS; i++) {
        blocks[i] = hyper_malloc(ALLOCATION_SIZE);

        if (blocks[i] == NULL) {
            printf("Initial allocation failed at %d\n", i);
            return 1;
        }
    }

    int initial_sbrk_calls = get_sbrk_calls();

    /*
     * Free every block so the free lists contain reusable blocks.
     */
    for (int i = 0; i < NUM_BLOCKS; i++) {
        hyper_free(blocks[i]);
    }

    /*
     * Measure allocation entirely from the free lists.
     */
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < NUM_BLOCKS; i++) {
        blocks[i] = hyper_malloc(ALLOCATION_SIZE);

        if (blocks[i] == NULL) {
            printf("Reuse allocation failed at %d\n", i);
            return 1;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    int final_sbrk_calls = get_sbrk_calls();

    printf("===== FREE-LIST REUSE TEST =====\n");
    printf("Blocks: %d\n", NUM_BLOCKS);
    printf("Block size: %d bytes\n", ALLOCATION_SIZE);
    printf("Initial sbrk calls: %d\n", initial_sbrk_calls);
    printf("Final sbrk calls: %d\n", final_sbrk_calls);
    printf("New sbrk calls during reuse: %d\n",
           final_sbrk_calls - initial_sbrk_calls);
    printf("Reuse allocation time: %.6f seconds\n",
           elapsed_seconds(start, end));

    for (int i = 0; i < NUM_BLOCKS; i++) {
        hyper_free(blocks[i]);
    }

    return 0;
}
