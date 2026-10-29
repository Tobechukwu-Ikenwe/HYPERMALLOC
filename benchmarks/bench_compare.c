#define _POSIX_C_SOURCE 200809L
#include "hypermalloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { SLOTS = 4096 };

typedef struct Item {
    unsigned char *ptr;
    size_t size;
} Item;

static uint64_t random_next(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

static double seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) abort();
    return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    int use_hm = strcmp(argv[1], "hm") == 0;
    if (!use_hm && strcmp(argv[1], "libc") != 0) return 2;
    int workload = strcmp(argv[2], "small") == 0 ? 0 :
                   strcmp(argv[2], "mixed") == 0 ? 1 :
                   strcmp(argv[2], "large") == 0 ? 2 : -1;
    if (workload < 0) return 2;
    char *end = NULL;
    unsigned long count = strtoul(argv[3], &end, 10);
    if (*end != '\0' || count == 0 || count > 10000000UL) return 2;
    Item items[SLOTS] = {{0}};
    uint64_t rng = UINT64_C(0x3a9d76b5f2104c89);
    uint64_t checksum = 0;
    double start = seconds();
    for (unsigned long i = 0; i < count; ++i) {
        uint64_t choice = random_next(&rng);
        size_t slot = (size_t)(choice % SLOTS);
        Item *item = &items[slot];
        if (item->ptr != NULL) {
            checksum += item->ptr[0] + item->ptr[item->size - 1];
            if ((choice & 3) == 0) {
                size_t new_size = workload == 2 ?
                    131072 + (size_t)(random_next(&rng) % 65536) :
                    1 + (size_t)(random_next(&rng) % (workload ? 4096 : 256));
                unsigned char *next = use_hm ? hm_realloc(item->ptr, new_size) :
                                               realloc(item->ptr, new_size);
                if (next == NULL) return 3;
                item->ptr = next;
                item->size = new_size;
                next[0] = (unsigned char)i;
                next[new_size - 1] = (unsigned char)(i >> 8);
                continue;
            }
            if (use_hm) hm_free(item->ptr);
            else free(item->ptr);
            item->ptr = NULL;
        } else {
            size_t size = workload == 2 ?
                131072 + (size_t)(random_next(&rng) % 65536) :
                1 + (size_t)(random_next(&rng) % (workload ? 4096 : 256));
            item->ptr = use_hm ? hm_malloc(size) : malloc(size);
            if (item->ptr == NULL) return 3;
            item->size = size;
            item->ptr[0] = (unsigned char)i;
            item->ptr[size - 1] = (unsigned char)(i >> 8);
        }
    }
    for (size_t slot = 0; slot < SLOTS; ++slot) {
        if (items[slot].ptr == NULL) continue;
        checksum += items[slot].ptr[0] + items[slot].ptr[items[slot].size - 1];
        if (use_hm) hm_free(items[slot].ptr);
        else free(items[slot].ptr);
    }
    double elapsed = seconds() - start;
    if (use_hm && hm_check_heap() != 0) return 4;
    printf("%.9f %llu\n", elapsed, (unsigned long long)checksum);
    return 0;
}
