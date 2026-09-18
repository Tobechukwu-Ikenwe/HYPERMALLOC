#include <stdio.h>
#include <stdlib.h>

#include "hypermalloc.h"
#include "internal.h"

#define NUM_ALLOCATIONS 10000
#define ALLOCATION_SIZE 256

int main(void)
{
    void *blocks[NUM_ALLOCATIONS];

    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        blocks[i] = hyper_malloc(ALLOCATION_SIZE);

        if (blocks[i] == NULL) {
            printf("Allocation failed at %d\n", i);
            return 1;
        }
    }

    printf("Allocations: %d\n", NUM_ALLOCATIONS);
    printf("Allocation size: %d bytes\n", ALLOCATION_SIZE);
    printf("sbrk calls: %d\n", get_sbrk_calls());

    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        hyper_free(blocks[i]);
    }

    return 0;
}
