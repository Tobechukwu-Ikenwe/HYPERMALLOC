# HyperMalloc

HyperMalloc is an experimental Linux allocator with a static `hm_*` API and
a shared library for `LD_PRELOAD`. It uses segregated free lists, boundary
tags, 256 KiB chunks, and direct mappings for requests at least 128 KiB or
alignment above 16 bytes. One empty ordinary chunk is kept for reuse; other
empty chunks and direct maps are unmapped on free.

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
coalesces adjacent free blocks immediately. It protects global metadata with
a pthread mutex. `hm_check_heap()` checks physical blocks, free lists,
boundary tags, direct mappings, and accounting. Debug builds abort on
invalid-pointer use; release builds ignore invalid frees. These checks are
diagnostic and do not make invalid pointers safe.

See [docs/memory-layout.md](docs/memory-layout.md) for the layout,
[docs/invariants.md](docs/invariants.md) for checker coverage, and
[BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) for measured results and limits.
The older `hyper_*` API remains available as compatibility wrappers.
