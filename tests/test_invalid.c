#include "hypermalloc.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef HM_DEBUG
#define HM_DEBUG 0
#endif

static int run_invalid(int mode)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return 1;
    pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        (void)close(pipefd[0]);
        (void)dup2(pipefd[1], STDERR_FILENO);
        (void)close(pipefd[1]);
        if (mode == 0) {
            void *ptr = hm_malloc(64);
            hm_free(ptr);
            hm_free(ptr);
        } else {
            int local = 0;
            hm_free(&local);
        }
        _exit(0);
    }
    (void)close(pipefd[1]);
    char message[128] = {0};
    ssize_t bytes = read(pipefd[0], message, sizeof(message) - 1);
    (void)close(pipefd[0]);
    int status = 0;
    if (waitpid(child, &status, 0) != child) return 1;
#if HM_DEBUG
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT ||
        bytes <= 0 || strstr(message, "HyperMalloc: invalid pointer") == NULL)
        return 1;
#else
    (void)bytes;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 1;
#endif
    return 0;
}

int main(void)
{
    if (run_invalid(0) || run_invalid(1)) {
        fputs("invalid-use contract failed\n", stderr);
        return 1;
    }
    return 0;
}
