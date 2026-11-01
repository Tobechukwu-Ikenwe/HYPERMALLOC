#define _POSIX_C_SOURCE 200809L
#include "hypermalloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Repeated large-map shrink, with data checks and no work outside the timed
 * region except setup. The libc path allows comparison with LD_PRELOAD peers. */
static double now_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) abort();
    return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}

static uint64_t now_nanoseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) abort();
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) return 2;
    int sample_latency = argc == 4 && strcmp(argv[3], "latency") == 0;
    if (argc == 4 && !sample_latency) return 2;
    int use_hm = strcmp(argv[1], "hm") == 0;
    if (!use_hm && strcmp(argv[1], "libc") != 0) return 2;
    char *end = NULL;
    unsigned long repeats = strtoul(argv[2], &end, 10);
    if (*end != '\0' || repeats == 0 || repeats > 1000000UL) return 2;
    uint64_t *latencies = sample_latency ?
        calloc(repeats, sizeof(*latencies)) : NULL;
    if (sample_latency && latencies == NULL) return 3;
    uint64_t checksum = 0;
    double start = now_seconds();
    for (unsigned long i = 0; i < repeats; ++i) {
        unsigned char *ptr = use_hm ? hm_malloc(512 * 1024) : malloc(512 * 1024);
        if (ptr == NULL) return 3;
        ptr[0] = (unsigned char)i;
        ptr[256 * 1024 - 1] = (unsigned char)(i >> 8);
        uint64_t start_ns = sample_latency ? now_nanoseconds() : 0;
        unsigned char *shrunk = use_hm ? hm_realloc(ptr, 256 * 1024) :
                                         realloc(ptr, 256 * 1024);
        if (sample_latency) latencies[i] = now_nanoseconds() - start_ns;
        if (shrunk == NULL || shrunk[0] != (unsigned char)i ||
            shrunk[256 * 1024 - 1] != (unsigned char)(i >> 8)) return 4;
        checksum += shrunk[0] + shrunk[256 * 1024 - 1];
        if (use_hm) hm_free(shrunk);
        else free(shrunk);
    }
    double elapsed = now_seconds() - start;
    if (use_hm && hm_check_heap() != 0) return 5;
    printf("%.9f %llu\n", elapsed, (unsigned long long)checksum);
    if (sample_latency) {
        qsort(latencies, repeats, sizeof(*latencies), compare_u64);
        printf("%llu %llu %llu\n",
               (unsigned long long)latencies[(repeats - 1) * 50 / 100],
               (unsigned long long)latencies[(repeats - 1) * 95 / 100],
               (unsigned long long)latencies[(repeats - 1) * 99 / 100]);
        free(latencies);
    }
    return 0;
}
