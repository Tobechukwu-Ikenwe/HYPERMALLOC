#include <hypermalloc.h>

int main(void)
{
    void *ptr = hm_malloc(32);
    if (ptr == NULL || hm_check_heap() != 0) return 1;
    hm_free(ptr);
    return hm_check_heap();
}
