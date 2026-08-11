#include <stdio.h>

void *hypermalloc(int user_size);
void hyper_free(void *ptr);

int hypermalloc_get_sbrk_calls(void);

#define NUM_ALLOCS 10000
#define ALLOC_SIZE 1000

int main(void)
{
    void *blocks[NUM_ALLOCS];

    printf("=== HyperMalloc Reuse Benchmark ===\n\n");

    /*
     * Scenario A:
     * Allocate many blocks without freeing them.
     */
    int start_calls = hypermalloc_get_sbrk_calls();

    for (int i = 0; i < NUM_ALLOCS; i++) {
        blocks[i] = hypermalloc(ALLOC_SIZE);

        if (blocks[i] == NULL) {
            fprintf(stderr, "Allocation failed at %d\n", i);
            return 1;
        }
    }

    int no_reuse_calls =
        hypermalloc_get_sbrk_calls() - start_calls;

    /*
     * Free all blocks from Scenario A.
     */
    for (int i = 0; i < NUM_ALLOCS; i++) {
        hyper_free(blocks[i]);
    }

    /*
     * Scenario B:
     * Allocate and immediately free the same-sized block.
     * The allocator should reuse the freed block.
     */
    start_calls = hypermalloc_get_sbrk_calls();

    for (int i = 0; i < NUM_ALLOCS; i++) {
        void *block = hypermalloc(ALLOC_SIZE);

        if (block == NULL) {
            fprintf(stderr, "Allocation failed at %d\n", i);
            return 1;
        }

        hyper_free(block);
    }

    int reuse_calls =
        hypermalloc_get_sbrk_calls() - start_calls;

    printf("Allocations per scenario: %d\n", NUM_ALLOCS);
    printf("Allocation size: %d bytes\n\n", ALLOC_SIZE);

    printf("Without reuse: %d sbrk calls\n", no_reuse_calls);
    printf("With reuse:    %d sbrk calls\n", reuse_calls);

    return 0;
}
