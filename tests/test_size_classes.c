/* Compile the implementation into this test translation unit so its private
 * size-class function can be checked without exporting a testing-only API. */
#include "../src/core.c"

#include <stdio.h>

int main(void)
{
    size_t expected = 0;
    for (size_t upper = 48; upper <= 256; upper += 16, ++expected) {
        if (size_class(upper) != expected ||
            (upper > 48 && size_class(upper - 1) != expected) ||
            size_class(upper + 1) != expected + 1) {
            fprintf(stderr, "small class boundary %zu\n", upper);
            return 1;
        }
    }
    size_t index = 14;
    for (size_t base = 256; base <= 65536; base *= 2) {
        size_t step = base / 4;
        for (size_t quarter = 1; quarter <= 4; ++quarter, ++index) {
            size_t upper = base + quarter * step;
            if (size_class(upper) != index ||
                size_class(upper + 1) != index + 1) {
                fprintf(stderr, "geometric class boundary %zu\n", upper);
                return 1;
            }
        }
    }
    return 0;
}
