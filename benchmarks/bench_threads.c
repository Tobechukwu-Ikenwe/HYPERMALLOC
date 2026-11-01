#define _GNU_SOURCE
#include "hypermalloc.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { SLOTS = 64, MAX_THREADS = 16 };
static unsigned char *handoff[MAX_THREADS][SLOTS];

typedef struct Worker {
    size_t id;
    size_t operations;
    int workload;
    int use_hm;
    int failed;
    uint64_t checksum;
    pthread_barrier_t *start;
    pthread_barrier_t *phase;
    size_t thread_count;
    unsigned char *ptr[SLOTS];
    size_t size[SLOTS];
} Worker;

static uint64_t next_random(uint64_t *state)
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

/* Batch cross-thread frees: each worker allocates, then the preceding worker
 * frees those pointers after a barrier. Two barriers prevent reuse races. */
static void run_handoff(Worker *job)
{
    for (size_t round = 0; round < job->operations / SLOTS; ++round) {
        for (size_t slot = 0; slot < SLOTS; ++slot) {
            unsigned char *ptr = job->use_hm ? hm_malloc(64 + slot) :
                                                malloc(64 + slot);
            if (ptr == NULL) job->failed = 1;
            else ptr[0] = (unsigned char)(round + slot + job->id);
            handoff[job->id][slot] = ptr;
        }
        (void)pthread_barrier_wait(job->phase);
        size_t source = (job->id + 1) % job->thread_count;
        for (size_t slot = 0; slot < SLOTS; ++slot) {
            unsigned char *ptr = handoff[source][slot];
            if (ptr == NULL) {
                job->failed = 1;
                continue;
            }
            job->checksum += ptr[0];
            if (job->use_hm) hm_free(ptr);
            else free(ptr);
        }
        (void)pthread_barrier_wait(job->phase);
    }
}

static void *worker(void *argument)
{
    Worker *job = argument;
    uint64_t state = UINT64_C(0x3a9d76b5f2104c89) ^
                     ((uint64_t)job->id * UINT64_C(0x9e3779b97f4a7c15));
    (void)pthread_barrier_wait(job->start);
    if (job->workload == 3) {
        run_handoff(job);
        return NULL;
    }
    for (size_t i = 0; i < job->operations; ++i) {
        size_t slot = (size_t)(next_random(&state) % SLOTS);
        if (job->ptr[slot] != NULL) {
            job->checksum += job->ptr[slot][0] +
                             job->ptr[slot][job->size[slot] - 1];
            if (job->use_hm) hm_free(job->ptr[slot]);
            else free(job->ptr[slot]);
        }
        size_t size = job->workload == 0 ?
            16 + (size_t)(next_random(&state) % 241) :
            job->workload == 1 ?
            16 + (size_t)(next_random(&state) % 4081) :
            131072 + (size_t)(next_random(&state) % 65536);
        job->ptr[slot] = job->use_hm ? hm_malloc(size) : malloc(size);
        if (job->ptr[slot] == NULL) {
            job->failed = 1;
            break;
        }
        job->size[slot] = size;
        job->ptr[slot][0] = (unsigned char)(i + job->id);
        job->ptr[slot][size - 1] = (unsigned char)(i >> 8);
    }
    for (size_t slot = 0; slot < SLOTS; ++slot) {
        if (job->ptr[slot] == NULL) continue;
        job->checksum += job->ptr[slot][0] +
                         job->ptr[slot][job->size[slot] - 1];
        if (job->use_hm) hm_free(job->ptr[slot]);
        else free(job->ptr[slot]);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    int use_hm = strcmp(argv[1], "hm") == 0;
    if (!use_hm && strcmp(argv[1], "libc") != 0) return 2;
    int workload = strcmp(argv[2], "small") == 0 ? 0 :
                   strcmp(argv[2], "mixed") == 0 ? 1 :
                   strcmp(argv[2], "direct") == 0 ? 2 :
                   strcmp(argv[2], "handoff") == 0 ? 3 : -1;
    if (workload < 0) return 2;
    char *end = NULL;
    unsigned long thread_count = strtoul(argv[3], &end, 10);
    if (*end != '\0' || thread_count == 0 || thread_count > MAX_THREADS)
        return 2;
    unsigned long operations = strtoul(argv[4], &end, 10);
    if (*end != '\0' || operations == 0 || operations > 1000000UL) return 2;
    if (workload == 3 && operations % SLOTS != 0) return 2;

    pthread_barrier_t start;
    pthread_barrier_t phase;
    if (pthread_barrier_init(&start, NULL, (unsigned)thread_count + 1) != 0)
        return 3;
    if (pthread_barrier_init(&phase, NULL, (unsigned)thread_count) != 0)
        return 3;
    pthread_t threads[MAX_THREADS];
    Worker jobs[MAX_THREADS] = {{0}};
    for (size_t id = 0; id < (size_t)thread_count; ++id) {
        jobs[id].id = id;
        jobs[id].operations = (size_t)operations;
        jobs[id].workload = workload;
        jobs[id].use_hm = use_hm;
        jobs[id].start = &start;
        jobs[id].phase = &phase;
        jobs[id].thread_count = (size_t)thread_count;
        if (pthread_create(&threads[id], NULL, worker, &jobs[id]) != 0)
            return 3;
    }
    double begin = seconds();
    (void)pthread_barrier_wait(&start);
    uint64_t checksum = 0;
    int failed = 0;
    for (size_t id = 0; id < (size_t)thread_count; ++id) {
        if (pthread_join(threads[id], NULL) != 0) return 3;
        checksum += jobs[id].checksum;
        failed |= jobs[id].failed;
    }
    double elapsed = seconds() - begin;
    (void)pthread_barrier_destroy(&start);
    (void)pthread_barrier_destroy(&phase);
    if (failed || (use_hm && hm_check_heap() != 0)) return 4;
    hm_stats snapshot = {0};
    if (use_hm) hm_get_stats(&snapshot);
    printf("%.9f %llu %zu %zu\n", elapsed, (unsigned long long)checksum,
           snapshot.current_heap_bytes, snapshot.peak_heap_bytes);
    return 0;
}
