# Developing HyperMalloc

Configure and run the four supported test builds with `scripts/check.sh
release`, `scripts/check.sh debug`, `scripts/check.sh asan`, and
`scripts/check.sh tsan`. ASan disables leak detection in this environment
because LeakSanitizer cannot attach under ptrace. The preload smoke test runs
only in unsanitized builds; ASan and TSan runtimes must be loaded before an
instrumented preload library.

Run `scripts/fuzz-quick.sh` for 100,000 seeded operations or
`out/release/fuzz_ops --seed 1 100 10000` for 1,000,000. The fuzzer replays
deterministic operations against HyperMalloc and libc, comparing contents,
alignment, overlap, and heap invariants. `--file tests/corpus/basic.txt`
replays the checked-in byte corpus. The optional Clang libFuzzer target uses
`-DHM_LIBFUZZER=ON` with a Clang build. Copy `tests/corpus/` to `out/fuzz-corpus/`
before running libFuzzer so generated inputs stay out of the source tree.

Run `python3 scripts/bench.py` to rebuild Release and collect seven matched
runs per workload. The script writes raw CSV and a median SVG chart to
`out/benchmarks/` by default; pass `--output-dir` to choose another path. It
alternates allocator order and checks that each pair
produces the same checksum. Timings use `CLOCK_MONOTONIC`; see
`benchmarks/bench_compare.c` for operation generation and memory touches.

For concurrency measurements, build Release and run
`python3 scripts/bench-threads.py --output out/benchmarks/threads.csv`.
This compares libc and HyperMalloc with 1, 2, 4, and 8 threads on small,
mixed, direct-map, and cross-thread handoff workloads. `HM_ARENAS` configures 1-8 active arenas;
the default is 8. See `optimizations.md` for recorded comparisons.

`libhypermalloc_preload.so` wraps the common glibc allocation entry points.
To exercise another program, use `LD_PRELOAD=out/release/libhypermalloc_preload.so
program args`. This is an experimental Linux preload build. The test suite
checks `malloc`, `free`, `calloc`, `realloc`, aligned allocation, and usable
size through the shared object.

Each active arena can retain one empty ordinary chunk. Separate arena mutexes
let threads allocate concurrently when they use different arenas. It
registers `pthread_atfork` handlers to lock every arena before fork and
prevent a child process from inheriting a locked allocator mutex; the static
and preload fork tests exercise this with a concurrent allocating thread.
Invalid-pointer checks are best effort and not a memory-safety boundary. The
preload build is not supported for setuid programs.
