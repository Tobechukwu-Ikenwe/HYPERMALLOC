#include "hypermalloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FUZZ_SLOTS = 64, FUZZ_RECORD_BYTES = 5 };

typedef struct Slot {
    unsigned char *ptr;
    unsigned char *reference;
    size_t size;
    unsigned char pattern;
} Slot;

static int verify_slots(const Slot *slots)
{
    for (size_t i = 0; i < FUZZ_SLOTS; ++i) {
        if (slots[i].ptr == NULL) continue;
        for (size_t byte = 0; byte < slots[i].size; ++byte)
            if (slots[i].ptr[byte] != slots[i].reference[byte] ||
                slots[i].ptr[byte] != slots[i].pattern) return 1;
        uintptr_t begin = (uintptr_t)slots[i].ptr;
        uintptr_t end = begin + slots[i].size;
        for (size_t j = i + 1; j < FUZZ_SLOTS; ++j) {
            if (slots[j].ptr == NULL) continue;
            uintptr_t other_begin = (uintptr_t)slots[j].ptr;
            uintptr_t other_end = other_begin + slots[j].size;
            if (begin < other_end && other_begin < end) return 2;
        }
    }
    return 0;
}

static int overlaps_existing(const Slot *slots, size_t except,
                             const unsigned char *ptr, size_t size)
{
    uintptr_t begin = (uintptr_t)ptr;
    uintptr_t end = begin + size;
    for (size_t index = 0; index < FUZZ_SLOTS; ++index) {
        if (index == except || slots[index].ptr == NULL) continue;
        uintptr_t other_begin = (uintptr_t)slots[index].ptr;
        uintptr_t other_end = other_begin + slots[index].size;
        if (begin < other_end && other_begin < end) return 1;
    }
    return 0;
}

static int run_ops(const uint8_t *data, size_t length)
{
    Slot slots[FUZZ_SLOTS] = {{0}};
    int failure = 0;
    for (size_t offset = 0; offset + FUZZ_RECORD_BYTES <= length;
         offset += FUZZ_RECORD_BYTES) {
        uint8_t kind = data[offset] % 5;
        size_t index = data[offset + 1] % FUZZ_SLOTS;
        size_t size = ((size_t)data[offset + 2] << 8) | data[offset + 3];
        size = size % 8192;
        if (data[offset + 4] == UINT8_MAX)
            size += 128 * 1024; /* Occasionally cross the direct-map boundary. */
        Slot *slot = &slots[index];
        if (slot->ptr != NULL) {
            for (size_t byte = 0; byte < slot->size; ++byte)
                if (slot->ptr[byte] != slot->reference[byte] ||
                    slot->ptr[byte] != slot->pattern) { failure = 1; break; }
            if (failure) break;
            if (kind == 2) {
                unsigned char *changed = hm_realloc(slot->ptr, size);
                if (size == 0) {
                    if (changed != NULL) { failure = 2; break; }
                    free(slot->reference);
                    slot->ptr = NULL;
                    slot->reference = NULL;
                    slot->size = 0;
                } else if (changed != NULL) {
                    unsigned char *reference = realloc(slot->reference, size);
                    slot->ptr = changed;
                    if (reference != NULL) slot->reference = reference;
                    if (reference == NULL) { failure = 11; break; }
                    if (overlaps_existing(slots, index, changed, size)) {
                        failure = 9;
                        break;
                    }
                    size_t kept = slot->size < size ? slot->size : size;
                    for (size_t byte = 0; byte < kept; ++byte)
                        if (changed[byte] != reference[byte]) {
                            failure = 3;
                            break;
                        }
                    if (failure) break;
                    slot->size = size;
                    memset(changed, slot->pattern, size);
                    memset(reference, slot->pattern, size);
                }
            } else {
                hm_free(slot->ptr);
                free(slot->reference);
                slot->ptr = NULL;
                slot->reference = NULL;
                slot->size = 0;
            }
        } else {
            unsigned char *ptr;
            unsigned char *reference;
            if (kind == 3) {
                size_t count = 1 + data[offset + 4] % 16;
                size_t item_size = size / count;
                ptr = hm_calloc(count, item_size);
                reference = calloc(count, item_size == 0 ? 1 : item_size);
                size = count * item_size;
                if (ptr != NULL)
                    for (size_t byte = 0; byte < size; ++byte)
                        if (ptr[byte] != 0) { failure = 4; break; }
            } else if (kind == 4) {
                size_t alignment = (size_t)16 << (data[offset + 4] % 7);
                ptr = hm_memalign(alignment, size);
                reference = aligned_alloc(alignment,
                    ((size == 0 ? 1 : size) + alignment - 1) & ~(alignment - 1));
                if (ptr != NULL && (uintptr_t)ptr % alignment != 0) failure = 5;
            } else {
                ptr = hm_malloc(size);
                reference = malloc(size == 0 ? 1 : size);
            }
            if (failure) break;
            if (ptr == NULL || reference == NULL) { failure = 6; break; }
            if (overlaps_existing(slots, index, ptr, size == 0 ? 1 : size)) {
                failure = 10;
                break;
            }
            slot->ptr = ptr;
            slot->reference = reference;
            slot->size = size == 0 ? 1 : size;
            slot->pattern = (unsigned char)(index + 1);
            memset(ptr, slot->pattern, slot->size);
            memset(reference, slot->pattern, slot->size);
        }
        if ((offset / FUZZ_RECORD_BYTES) % 128 == 0 &&
            (hm_check_heap() != 0 || verify_slots(slots) != 0)) {
            failure = 7;
            break;
        }
    }
    for (size_t index = 0; index < FUZZ_SLOTS; ++index) {
        hm_free(slots[index].ptr);
        free(slots[index].reference);
    }
    if (!failure && hm_check_heap() != 0) failure = 8;
    return failure;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    int result = run_ops(data, size);
    if (result != 0) abort();
    return 0;
}

#ifdef HM_FUZZ_STANDALONE
static uint64_t next_random(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--file") == 0) {
        FILE *input = fopen(argv[2], "rb");
        if (input == NULL) return 2;
        uint8_t buffer[65536];
        size_t bytes = fread(buffer, 1, sizeof(buffer), input);
        (void)fclose(input);
        int result = run_ops(buffer, bytes);
        if (result != 0) fprintf(stderr, "corpus failure code=%d\n", result);
        return result != 0;
    }
    if (argc != 5 || strcmp(argv[1], "--seed") != 0) {
        fputs("usage: fuzz_ops --seed first last operations | --file path\n",
              stderr);
        return 2;
    }
    uint64_t first = strtoull(argv[2], NULL, 0);
    uint64_t last = strtoull(argv[3], NULL, 0);
    size_t records = (size_t)strtoull(argv[4], NULL, 0);
    if (first == 0 || last < first || last > 1000000 ||
        records == 0 || records > 100000) return 2;
    static uint8_t buffer[100000 * FUZZ_RECORD_BYTES];
    for (uint64_t seed = first; seed <= last; ++seed) {
        uint64_t state = seed;
        for (size_t index = 0; index < records * FUZZ_RECORD_BYTES; ++index)
            buffer[index] = (uint8_t)next_random(&state);
        int result = run_ops(buffer, records * FUZZ_RECORD_BYTES);
        if (result != 0) {
            fprintf(stderr, "seed=%llu failure=%d\n",
                    (unsigned long long)seed, result);
            return 1;
        }
    }
    printf("seeds=%llu..%llu operations=%llu\n",
           (unsigned long long)first, (unsigned long long)last,
           (unsigned long long)((last - first + 1) * records));
    return 0;
}
#endif
