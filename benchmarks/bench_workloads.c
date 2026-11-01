#define _POSIX_C_SOURCE 200809L
#include "hypermalloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { SLOTS = 1024 };
typedef struct Record {
    unsigned char *title, *body, *tags;
    size_t title_size, body_size, tags_size;
} Record;
static uint64_t state = UINT64_C(0x7f4a7c159e3779b9);
static uint64_t checksum;
static int use_hm;

static uint64_t next_random(void)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static double seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) abort();
    return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}

static unsigned char *get_block(size_t size)
{
    unsigned char *ptr = use_hm ? hm_malloc(size) : malloc(size);
    if (ptr == NULL) abort();
    if (size != 0) {
        ptr[0] = (unsigned char)(size & 255);
        ptr[size - 1] = (unsigned char)(size >> 8);
    }
    return ptr;
}

static void put_block(unsigned char *ptr, size_t size)
{
    if (size != 0) checksum += ptr[0] + ptr[size - 1];
    if (use_hm) hm_free(ptr);
    else free(ptr);
}

static void immediate(size_t operations, size_t base, size_t spread)
{
    for (size_t i = 0; i < operations; ++i) {
        size_t size = base + (size_t)(next_random() % spread);
        unsigned char *ptr = get_block(size);
        put_block(ptr, size);
    }
}

static void batches(size_t operations, int holes)
{
    unsigned char *ptr[SLOTS];
    size_t sizes[SLOTS];
    for (size_t done = 0; done < operations; done += SLOTS) {
        size_t count = operations - done < SLOTS ? operations - done : SLOTS;
        for (size_t i = 0; i < count; ++i) {
            sizes[i] = holes ? (i & 1 ? 64 : 2048) :
                                16 + (size_t)(next_random() % 4096);
            ptr[i] = get_block(sizes[i]);
        }
        if (holes) {
            for (size_t i = 0; i < count; i += 2) {
                put_block(ptr[i], sizes[i]);
                sizes[i] = 512;
                ptr[i] = get_block(sizes[i]);
            }
        }
        for (size_t i = 0; i < count; ++i) put_block(ptr[i], sizes[i]);
    }
}

static void aligned(size_t operations)
{
    for (size_t i = 0; i < operations; ++i) {
        size_t alignment = (size_t)1 << (5 + (i % 8));
        size_t size = 17 + (size_t)(next_random() % 1024);
        unsigned char *ptr;
        if (use_hm) ptr = hm_memalign(alignment, size);
        else {
            void *result = NULL;
            if (posix_memalign(&result, alignment, size) != 0) abort();
            ptr = result;
        }
        if (ptr == NULL || (uintptr_t)ptr % alignment != 0) abort();
        ptr[0] = (unsigned char)size;
        ptr[size - 1] = (unsigned char)(size >> 8);
        put_block(ptr, size);
    }
}

static void release_record(Record *record)
{
    if (record->title == NULL) return;
    put_block(record->title, record->title_size);
    put_block(record->body, record->body_size);
    put_block(record->tags, record->tags_size);
    record->title = record->body = record->tags = NULL;
}

/* An application-style cache: records have three independently sized fields,
 * survive across operations, and are replaced or evicted at random. */
static void records(size_t operations)
{
    Record items[SLOTS] = {{0}};
    for (size_t i = 0; i < operations; ++i) {
        Record *record = &items[next_random() % SLOTS];
        release_record(record);
        if ((next_random() & 3) == 0) continue;
        record->title_size = 24 + (size_t)(next_random() % 80);
        record->body_size = 256 + (size_t)(next_random() % 4096);
        record->tags_size = 16 + (size_t)(next_random() % 240);
        record->title = get_block(record->title_size);
        record->body = get_block(record->body_size);
        record->tags = get_block(record->tags_size);
    }
    for (size_t i = 0; i < SLOTS; ++i) release_record(&items[i]);
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    use_hm = strcmp(argv[1], "hm") == 0;
    if (!use_hm && strcmp(argv[1], "libc") != 0) return 2;
    char *end = NULL;
    unsigned long operations = strtoul(argv[3], &end, 10);
    if (*end != '\0' || operations == 0 || operations > 1000000UL) return 2;
    double start = seconds();
    if (strcmp(argv[2], "small") == 0) immediate(operations, 1, 256);
    else if (strcmp(argv[2], "medium") == 0)
        immediate(operations, 512, 16384);
    else if (strcmp(argv[2], "large") == 0)
        immediate(operations, 256 * 1024, 128 * 1024);
    else if (strcmp(argv[2], "zero") == 0)
        immediate(operations, 0, 1);
    else if (strcmp(argv[2], "threshold") == 0)
        immediate(operations, 128 * 1024 - 512, 1024);
    else if (strcmp(argv[2], "batch") == 0) batches(operations, 0);
    else if (strcmp(argv[2], "holes") == 0) batches(operations, 1);
    else if (strcmp(argv[2], "aligned") == 0) aligned(operations);
    else if (strcmp(argv[2], "records") == 0) records(operations);
    else return 2;
    double elapsed = seconds() - start;
    hm_stats snapshot = {0};
    if (use_hm) {
        if (hm_check_heap() != 0) return 4;
        hm_get_stats(&snapshot);
    }
    printf("%.9f %llu %zu %zu %zu %zu\n", elapsed,
           (unsigned long long)checksum, snapshot.current_heap_bytes,
           snapshot.peak_heap_bytes, snapshot.peak_live_bytes,
           snapshot.internal_fragmentation_bytes);
    return 0;
}
