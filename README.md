# HyperMalloc

HyperMalloc is an experimental Linux allocator with a static `hm_*` API and
a shared library for `LD_PRELOAD`. It uses segregated free lists, boundary
tags, 256 KiB chunks, and direct mappings for requests at least 128 KiB or
alignment above 16 bytes. Each arena can keep one empty ordinary chunk
for reuse; other empty chunks and direct maps are unmapped on free.

## Build and test

```sh
cmake --preset release
cmake --build out/release -j 4
ctest --test-dir out/release --output-on-failure
```

Install with `cmake --install out/release --prefix /your/prefix`. Consumer CMake
projects can use `find_package(HyperMalloc 0.1 CONFIG REQUIRED)` and link
`HyperMalloc::hypermalloc`. The install includes the preload shared library.

`scripts/check.sh` accepts `release`, `debug`, `asan`, or `tsan`. See
[docs/DEVELOPING.md](docs/DEVELOPING.md) for fuzzing, preload, and benchmark
commands. The public declarations and API contracts are in
[`include/hypermalloc.h`](include/hypermalloc.h).

## Design and results

The allocator returns 16-byte aligned payloads, splits reusable blocks, and
coalesces adjacent free blocks immediately. Up to eight independent arenas
each protect their metadata with a mutex. `hm_check_heap()` checks physical
blocks, free lists, boundary tags, direct mappings, and accounting. Debug
builds abort on invalid-pointer use; release builds ignore invalid frees.
These checks are diagnostic and do not make invalid pointers safe.

See [docs/memory-layout.md](docs/memory-layout.md) for the layout,
[docs/invariants.md](docs/invariants.md) for checker coverage,
[BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) for earlier measurements, and
[optimizations.md](optimizations.md) for the arena benchmark and decision.

## Comparison with glibc malloc

In a matched seven-run benchmark of 2,000 large-allocation shrink cycles,
HyperMalloc's median time fell from 0.3715 s to 0.0189 s after an in-place
`realloc` optimization (19.6× faster than its previous implementation). glibc
`malloc` took 0.0175 s on the same workload, so HyperMalloc was about 8% slower.

Broader allocation benchmarks also compare against glibc. In the current
ten-run small-object test, HyperMalloc took 0.0078 s versus glibc's 0.0014 s
for 50,000 operations on one thread; it remains slower on the measured small,
mixed, and cross-thread workloads. These are synthetic whole-workload timings,
not isolated allocator-call latencies or a general performance guarantee. See
[optimizations.md](optimizations.md) for methods, other workloads, and raw
results. Correctness is checked separately with seeded fuzzing, CTest, and
sanitizer builds; see [docs/DEVELOPING.md](docs/DEVELOPING.md).
