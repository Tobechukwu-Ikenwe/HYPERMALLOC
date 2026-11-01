#include <stdio.h>
#include <string.h>
#include "hypermalloc.h"

int main(void) {
    printf("=== HyperMalloc Direct API Demo ===\n");

    char *message = hm_malloc(100);

    if (!message) {
        printf("HyperMalloc allocation failed!\n");
        return 1;
    }

    strcpy(message, "I am using HyperMalloc directly!");

    printf("Allocated 100 bytes using hm_malloc().\n");
    printf("Stored message: %s\n", message);

    hm_free(message);

    printf("Released memory using hm_free().\n");
    printf("HyperMalloc direct API demo completed!\n");

    return 0;
}
