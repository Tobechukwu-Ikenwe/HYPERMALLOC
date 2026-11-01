#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    printf("Running memory allocation demo...\n");

    char *message = malloc(100);

    if (message == NULL) {
        printf("Allocation failed!\n");
        return 1;
    }

    strcpy(message, "Hello from HyperMalloc!");

    printf("Allocated memory: %s\n", message);

    free(message);

    printf("Memory freed successfully!\n");

    return 0;
}
