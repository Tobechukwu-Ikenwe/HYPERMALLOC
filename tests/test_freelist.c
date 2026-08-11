#include <stdio.h>

void *hypermalloc(int user_size);
void hyper_free(void *ptr);

int main(void)
{
    // Create a small block and return it to the free list
    void *A = hypermalloc(100);

    if (A == NULL) {
        fprintf(stderr, "Failed to allocate A\n");
        return 1;
    }

    hyper_free(A);

    // Request something much larger than the free block.
    // The allocator should not reuse A.
    // It should request additional memory from Linux.
    void *B = hypermalloc(10000);

    if (B == NULL) {
        fprintf(stderr, "Failed to allocate B\n");
        return 1;
    }

    printf("A (freed block): %p\n", A);
    printf("B (new allocation): %p\n", B);

    if (B == A) {
        fprintf(stderr, "Error: B incorrectly reused the 100-byte block\n");
        return 1;
    }

    printf("Allocation failure fallback test passed\n");

    hyper_free(B);

    return 0;
}
