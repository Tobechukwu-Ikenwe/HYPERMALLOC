#include "hypermalloc.h"

#include <stddef.h>

int main(void)
{
    enum { SLOTS = 512, SIZE = 4096, CHUNK_BYTES = 256 * 1024 };
    void *slots[SLOTS];
    for (size_t i = 0; i < SLOTS; ++i) {
        slots[i] = hm_malloc(SIZE);
        if (slots[i] == NULL) return 1;
    }
    hm_stats occupied;
    hm_get_stats(&occupied);
    if (occupied.current_heap_bytes <= CHUNK_BYTES) return 2;
    for (size_t i = 0; i < SLOTS; ++i) hm_free(slots[i]);
    hm_stats empty;
    hm_get_stats(&empty);
    if (empty.current_live_bytes != 0 ||
        empty.current_heap_bytes != CHUNK_BYTES ||
        hm_check_heap() != 0) return 3;
    return 0;
}
