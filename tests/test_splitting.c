
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/*
 * HyperMalloc — Assignment 5
 * Test: Block Splitting
 *
 * PURPOSE
 * -------
 * Verify that a large freed block can be split to satisfy a
 * smaller allocation while leaving the remainder available
 * for another allocation.
 *
 * TEST SCENARIO
 * -------------
 *
 *     A = hypermalloc(LARGE_SIZE)
 *     hyper_free(A)
 *
 *     B = hypermalloc(SMALL_SIZE)
 *
 * The allocator should:
 *
 *     1. Find A in the free list.
 *     2. Split A into:
 *
 *        [ B allocated ][ remainder free ]
 *
 *     3. Return B using the beginning of A's old user memory.
 *     4. Keep the remainder as a valid free block.
 *
 * Then:
 *
 *     C = hypermalloc(REMAINDER_SIZE)
 *
 * should be able to reuse the remainder instead of requesting
 * another large region from the OS.
 *
 * DEBUGGING
 * ----------
 * The addresses and test results are printed so that GDB can
 * be used to inspect the allocator's metadata.
 *
 * Useful GDB commands:
 *
 *     break hypermalloc
 *     break hyper_free
 *     run
 *     p *free_list_head
 *     p free_list_head->next
 *
 * Use Valgrind afterward:
 *
 *     valgrind ./test_splitting
 */

void *hypermalloc(int user_size);
void hyper_free(void *ptr);

#define LARGE_SIZE 1000
#define SMALL_SIZE 100

int main(void)
{
    printf("=== HyperMalloc Block Splitting Test ===\n\n");

    /*
     * ---------------------------------------------------------
     * STEP 1: Allocate a large block.
     * ---------------------------------------------------------
     *
     * A should contain enough memory that it can later be
     * split into a smaller allocation and a remaining free block.
     */
    void *A = hypermalloc(LARGE_SIZE);

    if (A == NULL) {
        fprintf(stderr, "ERROR: Failed to allocate A\n");
        return 1;
    }

    printf("[1] Large allocation A\n");
    printf("    A address: %p\n", A);
    printf("    A size:    %d bytes\n\n", LARGE_SIZE);

    /*
     * Write a known pattern into A.
     *
     * This gives us something useful to verify if the allocator
     * accidentally overwrites the allocated region while splitting.
     */
    memset(A, 0xAA, LARGE_SIZE);

    /*
     * ---------------------------------------------------------
     * STEP 2: Free A.
     * ---------------------------------------------------------
     *
     * A should now appear in the free list.
     */
    hyper_free(A);

    printf("[2] A freed\n");
    printf("    A should now be available in the free list\n\n");

    /*
     * ---------------------------------------------------------
     * STEP 3: Allocate a smaller block.
     * ---------------------------------------------------------
     *
     * The allocator should find A and split it.
     *
     * Expected layout:
     *
     *     [ B allocated ][ remainder free ]
     *
     * B should begin at the same address where A began.
     */
    void *B = hypermalloc(SMALL_SIZE);

    if (B == NULL) {
        fprintf(stderr, "ERROR: Failed to allocate B\n");
        return 1;
    }

    printf("[3] Small allocation B\n");
    printf("    B address: %p\n", B);
    printf("    B size:    %d bytes\n", SMALL_SIZE);
    printf("    A address: %p\n\n", A);

    /*
     * Verify that B reused the beginning of A's old block.
     *
     * This proves that the allocator reused A instead of
     * requesting a completely unrelated block.
     */
    if (B != A) {
        fprintf(stderr,
                "ERROR: B did not reuse A's original address\n");
        fprintf(stderr,
                "       A = %p\n"
                "       B = %p\n",
                A, B);
        return 1;
    }

    printf("[PASS] B reused A's original address\n\n");

    /*
     * ---------------------------------------------------------
     * STEP 4: Verify B's memory.
     * ---------------------------------------------------------
     *
     * Write a different pattern to B.
     *
     * This helps catch overlapping blocks or metadata corruption.
     */
    unsigned char *B_bytes = B;

    memset(B_bytes, 0x55, SMALL_SIZE);

    for (int i = 0; i < SMALL_SIZE; i++) {
        if (B_bytes[i] != 0x55) {
            fprintf(stderr,
                    "ERROR: B memory corrupted at byte %d\n",
                    i);
            return 1;
        }
    }

    printf("[PASS] B memory remains valid\n\n");

    /*
     * ---------------------------------------------------------
     * STEP 5: Allocate C from the remainder.
     * ---------------------------------------------------------
     *
     * The exact remainder depends on sizeof(BlockHeader), so
     * we intentionally request another small allocation rather
     * than hard-coding the remainder size here.
     *
     * If the split was successful, C should come from the
     * remaining portion of A rather than overlapping B.
     */
    void *C = hypermalloc(SMALL_SIZE);

    if (C == NULL) {
        fprintf(stderr, "ERROR: Failed to allocate C\n");
        return 1;
    }

    printf("[4] Second allocation C\n");
    printf("    C address: %p\n", C);
    printf("    C size:    %d bytes\n\n", SMALL_SIZE);

    /*
     * C must not overlap B.
     *
     * Since B occupies SMALL_SIZE bytes starting at B,
     * C should start at a different address.
     */
    if (C == B) {
        fprintf(stderr,
                "ERROR: C reused B's address\n"
                "       B and C may be overlapping\n");
        return 1;
    }

    printf("[PASS] B and C have different addresses\n\n");

    /*
     * ---------------------------------------------------------
     * STEP 6: Verify that B and C can hold independent data.
     * ---------------------------------------------------------
     *
     * B -> 0x55
     * C -> 0xCC
     *
     * If the blocks overlap, writing C may corrupt B.
     */
    unsigned char *C_bytes = C;

    memset(C_bytes, 0xCC, SMALL_SIZE);

    for (int i = 0; i < SMALL_SIZE; i++) {
        if (C_bytes[i] != 0xCC) {
            fprintf(stderr,
                    "ERROR: C memory corrupted at byte %d\n",
                    i);
            return 1;
        }
    }

    /*
     * Re-check B after writing to C.
     *
     * This is especially important: it verifies that C did not
     * overwrite B.
     */
    for (int i = 0; i < SMALL_SIZE; i++) {
        if (B_bytes[i] != 0x55) {
            fprintf(stderr,
                    "ERROR: B was corrupted after writing to C "
                    "at byte %d\n",
                    i);
            return 1;
        }
    }

    printf("[PASS] B and C contain independent data\n\n");

    /*
     * ---------------------------------------------------------
     * STEP 7: Cleanup.
     * ---------------------------------------------------------
     */
    hyper_free(B);
    hyper_free(C);

    printf("=== Block Splitting Test Passed ===\n");

    return 0;
}

