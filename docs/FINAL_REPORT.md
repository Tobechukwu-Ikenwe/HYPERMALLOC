# HyperMalloc implementation report (2026-10-28)

The allocator now provides a boundary-tag `hm_*` API, segregated free lists,
direct maps, heap checking, test builds, a differential operation fuzzer, a
Linux preload library, and a repeated benchmark harness. The original audit
remains in `AUDIT.md`; it describes the pre-rewrite working tree and should
not be read as the current implementation.

## Verified behavior

The current test suite covers allocation sizes and classes, alignment,
calloc overflow, realloc growth and shrinkage, coalescing, invalid frees,
heap corruption detection, cross-thread use, seeded operations, and preload
entry points. Release and Debug have nine passing CTest cases each. ASan with
UBSan and TSan have eight passing allocator cases each; their preload CTest is
excluded because sanitizer runtimes must load before an instrumented preload
library. A seeded differential run completed 1,000,000 operations. The
unsanitized preload build also ran `/usr/bin/ls` and `/usr/bin/sort` to
completion. Valgrind's focused preload program exited without reported errors.

Reproduce with:

```sh
scripts/check.sh release
scripts/check.sh debug
scripts/check.sh asan
scripts/check.sh tsan
out/release/fuzz_ops --seed 1 100 10000
LD_PRELOAD="$PWD/out/release/libhypermalloc_preload.so" /usr/bin/sort /etc/passwd
python3 scripts/bench.py
```

The benchmark runs seven repeats per allocator and workload, alternates
execution order, and verifies identical checksums. Raw CSV, plots, and the
before/after direct-map lookup experiment are in `benchmarks/results/` and
summarized in `BENCHMARK_RESULTS.md`. The large-workload median improved from
0.8885 s to 0.3317 s in the recorded runs. HyperMalloc remains slower than
libc on all three recorded workloads. The measurements are workload-specific
and were not collected on an otherwise idle machine.

## Scope and limitations

Ordinary chunks remain mapped for reuse. The preload build does not install
fork handlers, and the global mutex limits thread scalability. Invalid-pointer
checks are best effort; the checker is diagnostic rather than a hardened
security boundary. The preload library covers the common glibc allocation
entry points tested here, not every libc extension. Further work would be
needed before using it as a general system allocator.
