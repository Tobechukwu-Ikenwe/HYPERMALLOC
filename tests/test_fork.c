#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HM_FORK_STATIC
#include "hypermalloc.h"
#define allocate hm_malloc
#define deallocate hm_free
#else
#define allocate malloc
#define deallocate free
#endif

static atomic_int stop_worker;

static void *worker(void *unused)
{
    (void)unused;
    size_t size = 1;
    while (!atomic_load_explicit(&stop_worker, memory_order_relaxed)) {
        void *ptr = allocate(size);
        if (ptr == NULL) return (void *)(uintptr_t)1;
        ((unsigned char *)ptr)[0] = 0xa5;
        deallocate(ptr);
        size = size % 4096 + 1;
    }
    return NULL;
}

int main(void)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL) != 0) return 1;
    for (int iteration = 0; iteration < 40; ++iteration) {
        pid_t child = fork();
        if (child < 0) return 2;
        if (child == 0) {
            void *ptr = allocate(8192);
            if (ptr == NULL) _exit(3);
            ((unsigned char *)ptr)[0] = 0x5a;
            deallocate(ptr);
            _exit(0);
        }
        int status;
        if (waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 4;
    }
    atomic_store_explicit(&stop_worker, 1, memory_order_relaxed);
    void *result = NULL;
    if (pthread_join(thread, &result) != 0 || result != NULL) return 5;
    return 0;
}
