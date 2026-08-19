#ifndef HYPERMALLOC_H
#define HYPERMALLOC_H

#include <stddef.h>

void *hyper_malloc(size_t size);
void hyper_free(void *ptr);
void *hyper_realloc(void *ptr, size_t size);
int check_heap(void);
#endif
