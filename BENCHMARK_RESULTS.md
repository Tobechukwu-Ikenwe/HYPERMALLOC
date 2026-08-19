# HyperMalloc Benchmark Results

## Test Configuration

- Allocations: 100,000
- Initial allocation size: 256 bytes
- Reallocation size: 512 bytes
- Comparison: HyperMalloc vs standard C memory API
- Timing: `CLOCK_MONOTONIC`

## Results

### C Memory API

| Operation | Time |
|---|---:|
| malloc allocation | 0.013630 s |
| malloc realloc | 0.011460 s |
| malloc free | 0.002563 s |

### HyperMalloc

| Operation | Time |
|---|---:|
| hyper_malloc allocation | 0.024289 s |
| hyper_realloc | 16.904879 s |
| hyper_free | 4.606983 s |

## Findings

HyperMalloc successfully completed the full allocation, reallocation,
and free workload.

Initial allocation performance is relatively close to the system allocator.

Reallocation and free operations are currently significantly slower.
The current implementation performs block lookup by traversing the
physical heap list, which may result in O(N) lookup for each operation.

This creates a potential O(N²) workload when repeatedly reallocating
or freeing many blocks.

## Next Optimization

Investigate replacing the linear `find_block()` lookup with O(1)
metadata recovery from the user pointer while preserving pointer
validation and allocator correctness.
