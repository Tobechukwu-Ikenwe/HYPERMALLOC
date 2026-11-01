#define _GNU_SOURCE
#include "hypermalloc.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { THREADS = 4, SLOTS = 64, ROUNDS = 2000 };
static void *handoff[THREADS][SLOTS];
static pthread_barrier_t barrier;
static int failed;

static uint32_t next_random(uint32_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static void *worker(void *argument)
{
    size_t id = (size_t)(uintptr_t)argument;
    uint32_t state = (uint32_t)(id + 1) * UINT32_C(0x9e3779b9);
    void *slots[SLOTS] = {0};
    for (size_t round = 0; round < ROUNDS; ++round) {
        size_t slot = next_random(&state) % SLOTS;
        hm_free(slots[slot]);
        size_t bytes = 1 + next_random(&state) % 2048;
        slots[slot] = hm_malloc(bytes);
        if (slots[slot] == NULL) {
            __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
            break;
        }
        memset(slots[slot], (int)id, bytes);
    }
    for (size_t slot = 0; slot < SLOTS; ++slot) hm_free(slots[slot]);
    for (size_t slot = 0; slot < SLOTS; ++slot) {
        handoff[id][slot] = hm_malloc(64 + slot);
        if (handoff[id][slot] == NULL)
            __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
        else ((unsigned char *)handoff[id][slot])[0] = (unsigned char)id;
    }
    (void)pthread_barrier_wait(&barrier);
    for (size_t slot = 0; slot < SLOTS; ++slot) {
        void *foreign = handoff[(id + 1) % THREADS][slot];
        if (foreign == NULL) continue;
        if (hm_usable_size(foreign) < 64 + slot)
            __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
        if ((slot & 1) == 0) {
            void *grown = hm_realloc(foreign, 128 + slot);
            if (grown == NULL) {
                __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
            } else {
                if (((unsigned char *)grown)[0] != (unsigned char)((id + 1) % THREADS))
                    __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
                foreign = grown;
            }
        }
        hm_free(foreign);
    }
    return NULL;
}

int main(void)
{
    pthread_t threads[THREADS];
    if (pthread_barrier_init(&barrier, NULL, THREADS) != 0) return 1;
    for (size_t id = 0; id < THREADS; ++id)
        if (pthread_create(&threads[id], NULL, worker,
                           (void *)(uintptr_t)id) != 0) return 1;
    for (size_t id = 0; id < THREADS; ++id)
        if (pthread_join(threads[id], NULL) != 0) return 1;
    (void)pthread_barrier_destroy(&barrier);
    if (__atomic_load_n(&failed, __ATOMIC_RELAXED) || hm_check_heap() != 0) {
        fputs("thread workload or heap check failed\n", stderr);
        return 1;
    }
    hm_stats result;
    hm_get_stats(&result);
    return result.current_live_bytes != 0;
}
