# HyperMalloc optimization investigation (30 October 2026)

## Executive summary

I audited the existing eight-arena allocator, its preload adapter, public API,
tests, benchmark programs, build settings, and earlier arena measurements. The
first isolated optimization removes a costly copy and replacement mapping when
a direct-mapped allocation is resized within its existing capacity. Shrinks
release complete trailing pages with `munmap`. A matched seven-run benchmark
of 2,000 512-to-256 KiB resizes improved from 0.3715 to 0.01894 seconds
(94.9% less wall time); the release, ASan/UBSan, and TSan suites pass. This is
a targeted result, not a claim of general allocator superiority. A second,
separately measured change on 31 October reused the block already found during
free, cutting a repeated lookup; the one-thread small workload improved in
18 of 20 paired runs. HyperMalloc still trails glibc and jemalloc in measured
small-object workloads, and cross-thread ownership probing remains expensive.

The sections below document the earlier arena work as historical context. The
new investigation, raw results, and decisions follow that record.

## Earlier optimization record: independent arenas

## Decision

Use eight independent allocator arenas by default (`HM_ARENAS=8`). Each arena
has its own mutex, chunks, free lists, bitmap, direct-map registry, and local
accounting. A thread chooses a home arena on its first allocation. Most
same-thread frees find their allocation there; cross-thread free, realloc, and
usable-size calls probe the other arenas when needed. The heap checker, stats
snapshot, and fork handlers lock every arena in a fixed order.

This replaces one process-wide mutex. It improves the tested concurrent small
and mixed workloads while keeping the existing 16-byte block header, boundary
tags, coalescing behavior, direct-map policy, and public API. It does not make
HyperMalloc generally faster than libc, and it increases memory retained by
threads that have used separate arenas.

Multiple arenas and local caches are established ways to reduce allocator
contention; see [jemalloc's tuning guide](https://github.com/jemalloc/jemalloc/blob/dev/TUNING.md)
and [TCMalloc's design](https://github.com/google/tcmalloc/blob/master/docs/design.md).
The implementation here uses arenas without adding per-thread object caches,
which would require another policy for cached memory and cross-thread frees.

## Benchmark method

`benchmarks/bench_threads.c` runs worker threads with 64 pointer slots each.
For small, mixed, and direct workloads, each worker frees and allocates in
its own slots and touches the first and last byte. The handoff workload has
each worker allocate a batch that another worker frees after a barrier. The
benchmark checks the resulting heap and compares deterministic checksums.
`scripts/bench-threads.py` runs five repeats per case, rotates allocator order,
and records wall-clock time from `CLOCK_MONOTONIC`. The workloads are:

| Workload | Request sizes | Operations per thread |
|---|---:|---:|
| small | 16–256 bytes | 50,000 |
| mixed | 16–4,096 bytes | 30,000 |
| direct | 128–192 KiB | 1,000 |
| handoff | 64–127 bytes, freed by another thread | 32,768 |

The machine was a 13th Gen Intel Core i7-1355U running Linux
`7.0.0-38-generic`. These are complete workload times, not isolated lock
waits. `test_threads` separately checks cross-thread free, realloc, and
usable-size correctness. CPU scheduling,
other machine activity, and workload shape can change the result.

### One mutex versus eight arenas

Medians from the matched before/after run, in seconds. Lower is better. The
`before` program was saved from this workspace immediately before the arena
change; the `after` program uses eight arenas. Raw runs and checksums are in
`benchmarks/results/threads_before.csv` and
`benchmarks/results/threads_after.csv`.

| Workload | Threads | libc | Before | Eight arenas | Before / after |
|---|---:|---:|---:|---:|---:|
| small | 1 | 0.0012 | 0.0066 | 0.0076 | 0.86× |
| small | 2 | 0.0013 | 0.0718 | 0.0242 | 2.97× |
| small | 4 | 0.0018 | 0.1325 | 0.0468 | 2.83× |
| small | 8 | 0.0022 | 0.4817 | 0.0798 | 6.03× |
| mixed | 1 | 0.0050 | 0.0075 | 0.0082 | 0.91× |
| mixed | 2 | 0.0058 | 0.0658 | 0.0171 | 3.86× |
| mixed | 4 | 0.0064 | 0.1287 | 0.0267 | 4.81× |
| mixed | 8 | 0.0072 | 0.4246 | 0.0535 | 7.94× |
| direct | 1 | 0.0034 | 0.0101 | 0.0134 | 0.76× |
| direct | 2 | 0.0036 | 0.0340 | 0.0255 | 1.34× |
| direct | 4 | 0.0049 | 0.0810 | 0.0781 | 1.04× |
| direct | 8 | 0.0069 | 0.1878 | 0.1757 | 1.07× |

The benefit is concentrated in shared-chunk workloads. Direct allocations
still call `mmap` and `munmap` for each allocation; those calls likely limit
the benefit of arenas, though this benchmark did not isolate their cost. The
four- and eight-thread direct results barely changed. The extra arena
selection and pointer ownership lookup also regress the one-thread cases.

The cross-thread handoff comparison used the same five-repeat method. Raw
measurements are in `benchmarks/results/handoff.csv`:

| Threads | libc | One mutex | 2 arenas | 4 arenas | 8 arenas |
|---:|---:|---:|---:|---:|---:|
| 1 | 0.0013 s | **0.0051 s** | 0.0060 s | 0.0060 s | 0.0055 s |
| 2 | 0.0067 s | 0.0409 s | **0.0303 s** | 0.0361 s | 0.0368 s |
| 4 | 0.0087 s | 0.1039 s | **0.0689 s** | 0.0714 s | 0.0867 s |
| 8 | 0.0130 s | 0.5287 s | 0.2805 s | **0.1899 s** | 0.2112 s |

At one thread, handoff frees the caller's own allocations because no other
worker exists. Each timing includes the batch synchronization barriers.

More arenas make cross-thread ownership searches longer. Four arenas are
slightly faster on this eight-thread handoff case, while eight are materially
faster on the same-thread small and mixed cases. The default prioritizes
those measured small and mixed workloads; applications dominated by handoff
or memory retention can use `HM_ARENAS=4` or fewer.

### Why eight arenas?

We built the same implementation with 2, 4, and 8 active arenas and ran a
separate five-repeat comparison. Selected medians are below; the complete
results, including mapped-byte measurements, are in
`benchmarks/results/arena_counts.csv`.

| Workload | Threads | 2 arenas | 4 arenas | 8 arenas |
|---|---:|---:|---:|---:|
| small | 4 | 0.1026 s | **0.0398 s** | 0.0441 s |
| small | 8 | 0.2375 s | 0.1304 s | **0.0880 s** |
| mixed | 4 | 0.0787 s | **0.0248 s** | 0.0259 s |
| mixed | 8 | 0.1931 s | 0.0851 s | **0.0535 s** |

Eight arenas give the best measured eight-thread throughput, while four are
slightly faster in these four-thread runs. The memory cost is visible after
all small-workload allocations are freed: two arenas retain 0.5 MiB, four
retain 1 MiB, and eight retain 2 MiB, because each used arena keeps one empty
256 KiB chunk. These are mapped virtual bytes, not physical RAM residency.
`HM_ARENAS` permits choosing a lower count when that memory
cost matters more than eight-thread throughput.

## Correctness and limits

The per-arena checker compares each arena's physical blocks with its own
free lists and accounting. Process-wide live and mapped peaks use atomic
counters; `hm_get_stats()` locks every arena to return a consistent aggregate.
The fork handlers lock all arena mutexes before fork. Cross-thread pointer
operations search the caller's home arena first, then other arenas, so they
can be slower than same-thread operations. Invalid-pointer detection remains
best effort as before.

Release and debug suites passed 12/12 tests. AddressSanitizer and
ThreadSanitizer builds passed 10/10 tests each; their preload tests are
intentionally excluded by CMake. The updated thread test covers cross-arena
free, realloc, and usable-size calls. These tests do not establish that this
arena count is optimal for other applications.

## Reproducing the comparison

```sh
cmake --preset release
cmake --build out/release -j 4
python3 scripts/bench-threads.py --output out/benchmarks/threads.csv
```

To compare arena counts and the earlier one-mutex implementation:

```sh
cmake -S . -B /tmp/hm-arena2 -DCMAKE_BUILD_TYPE=Release -DHM_ARENAS=2
cmake --build /tmp/hm-arena2 --target bench_threads -j 4
cmake -S . -B /tmp/hm-arena4 -DCMAKE_BUILD_TYPE=Release -DHM_ARENAS=4
cmake --build /tmp/hm-arena4 --target bench_threads -j 4
git show 1ca88e1:src/core.c > /tmp/hm-core-before.c
cc -std=c11 -O3 -DNDEBUG -pthread -Iinclude /tmp/hm-core-before.c \
   benchmarks/bench_threads.c -o /tmp/hm-bench-before
python3 scripts/bench-threads.py \
  --baseline-program /tmp/hm-bench-before \
  --variant arena2=/tmp/hm-arena2/bench_threads \
  --variant arena4=/tmp/hm-arena4/bench_threads \
  --output out/benchmarks/arena-comparison.csv
```

The script accepts both the earlier two-column and current four-column
benchmark output formats. This replays the test cases; timings can differ
from the recorded run as machine conditions change.

## Current architecture and execution paths

`src/core.c` owns the allocation policy; `src/preload.c` exports conventional
libc names and forwards to `hm_*`. The static library does not interpose libc.
Each thread receives one of eight arenas round-robin. An arena mutex protects
its chunk registry, 64 doubly linked free lists and nonempty bitmap, direct-map
registry and 1,024 hash heads, and local statistics. Global live/mapped totals
and peaks use relaxed atomics; checks and stats snapshots lock all arenas in
index order. `pthread_atfork` locks all arenas before fork.

An ordinary 16-byte-aligned block has a 16-byte header (`requested`, packed
size/flags). A free block reuses its payload for two list links and ends with
a footer. The `PREV_ALLOC` bit allows immediate boundary-tag coalescing without
reading a nonexistent footer. A 256 KiB chunk has a descriptor, prologue,
ordinary blocks, and epilogue. Each arena retains at most one completely free
chunk. Large requests (at least 128 KiB) and alignments above 16 use one
`mmap` region each. A `DirectMap` descriptor records its base, length,
payload, alignment, registry links, and hash chain. The hash is only for exact
direct-payload lookup; ordinary allocations are found by scanning known chunk
ranges. Cross-thread free/realloc/usable-size probes arenas one lock at a time.

| API | Actual path |
|---|---|
| `hm_malloc` | Lock home arena; normalize zero to one byte; direct-map or find first fitting free block via bitmap; map a chunk if needed; split, account, unlock. |
| `hm_free` | Find and lock owner; direct map is unlinked and unmapped; ordinary block coalesces on both sides and may release a whole chunk. |
| `hm_calloc` | Check multiplication overflow, call `hm_malloc`, zero requested bytes. |
| `hm_realloc` | Handle NULL/zero and overflow; find owner; ordinary block may split or absorb its next free neighbor; otherwise allocate-copy-free. Direct capacity can now be reused and a shrink trims pages. Failure preserves the old allocation. |
| `hm_memalign` | Validate power-of-two alignment; ordinary path at at most 16, direct path above 16. |
| `hm_usable_size` | Find owner and report payload capacity of the block, not its requested length. |
| `reallocarray` | Preload wrapper checks count × size overflow, then calls `hm_realloc`. |
| `aligned_alloc` | Preload wrapper enforces size multiple of alignment, then calls `hm_memalign`. |
| `posix_memalign` | Preload wrapper validates alignment, returns an error number, writes output only on success, preserves `errno`. |
| `memalign`, `valloc` | Preload wrappers call `hm_memalign`; `valloc` supplies the OS page size. |
| `malloc_usable_size` | Preload wrapper calls `hm_usable_size`. |

The public contract treats invalid pointers as undefined use; detection is
best effort and aborts in debug mode. Zero-sized `hm_malloc` is a distinct
freeable allocation; `hm_realloc(ptr, 0)` frees and returns NULL. This is a
documented choice in the project and can differ from a particular libc's
zero-size behavior. Pointer ownership is an arena-level property, never a
thread-level one. The home arena's mutex does not protect another arena's
blocks: owner lookup locks the candidate arena before reading its registry.

## External allocator research and applicability

| Allocator | Mechanism and purpose | HyperMalloc evidence and decision |
|---|---|---|
| [jemalloc](https://github.com/jemalloc/jemalloc/blob/dev/TUNING.md) | Multiple arenas reduce lock sharing; per-thread tcaches serve common sizes without arena locks; size classes and extent management distinguish small and large objects. Dirty/muzzy decay and optional background purging trade CPU for resident memory. | The existing eight arenas already address lock sharing but the 1-thread small case is 6× slower than glibc. A tcache may help, but requires a bounded cache, flush on thread exit, and coherent remote frees. Deferred pending measured cache and retention costs. Eager direct-map trimming is a smaller, testable first step. |
| [mimalloc](https://github.com/microsoft/mimalloc) | Per-page size-class free lists improve locality and reduce long global lists. Separate local and concurrent free lists allow remote frees with an atomic operation. Empty pages can be purged while retaining virtual address space. | Current typical small/mixed searches are about one to 1.35 nodes per allocation in the earlier probe, so changing the entire free-list structure lacks evidence. The page-local idea is reserved for a measured locality or adversarial fragmentation case. |
| [snmalloc](https://github.com/microsoft/snmalloc) | Remote frees are batched and passed to the owning allocator, avoiding a lock for each handoff. Large address ranges reduce metadata lookup overhead. | HyperMalloc's handoff benchmark is slower than libc, so the problem is real. Implementing queues requires ownership metadata, atomic publication/draining, a policy for orphaned threads, and harder verification. Deferred until per-call owner/lock costs are measured. |
| [rpmalloc](https://github.com/mjansson/rpmalloc) | Thread-owned spans and caches keep hot allocations local; cross-thread frees defer to an owning heap. Its README explicitly describes producer-consumer gains and page underutilization when many sizes are sparsely used. | A span-per-size-class rewrite might speed small blocks but could multiply retained pages across eight arenas. Existing 256 KiB chunks can mix sizes; keep that advantage until a footprint study justifies changing it. |
| [glibc malloc tunables](https://sourceware.org/glibc/manual/latest/html_node/Memory-Allocation-Tunables.html) | Arenas, tcache counts, mmap threshold, and trimming policies expose throughput versus footprint trade-offs. | glibc is a measured comparison, not a template. HyperMalloc's fixed 128 KiB direct threshold is visibly costly for repeated direct maps. The isolated resize optimization avoids remapping without changing this threshold. |

These mechanisms answer different problems. Per-thread caches would target
lock overhead on small allocations; page-local lists would target search and
locality; remote-free queues would target handoff; decay/purging would target
retention. No such mechanism is adopted merely because another allocator uses
it. The present optimization instead uses Linux's documented ability to
[unmap a page-aligned subrange](https://man7.org/linux/man-pages/man2/mmap.2.html).

### Ranked opportunities after the baseline

| Rank | Candidate | Evidence | Expected benefit and proof | Cost / status |
|---:|---|---|---|---|
| 1 | Direct-map in-place resize and tail trim | 0.37 s/2,000 shrink cycles, two maps and a copy per cycle | Large resize throughput and lower mapped bytes; direct-resize benchmark plus stats test | Small localized change; **implemented** |
| 2 | Owner lookup metadata or bounded cache | Handoff 8-thread baseline about 0.20 s, vs libc 0.013 s | Handoff throughput and lock waits; instrument probe counts/lock time | Moderate to high synchronization risk; deferred |
| 3 | Bounded per-thread small-object cache | Small 1-thread baseline 0.0073 s vs libc 0.0012 s for 50k operations | Small allocate/free latency; same workload, footprint after thread exit | High retention/ownership complexity; deferred |
| 4 | Tune chunk and direct threshold | Direct allocation baseline far behind glibc | Reduce mapping count without unacceptable retained memory; threshold sweep | Moderate fragmentation trade-off; deferred |
| 5 | Change class spacing or page-local free lists | Typical `search_steps` 1–1.35 per allocation | Needs adversarial size mix showing long scans and fragmentation gain | High architecture cost, weak current evidence; deferred |

The prior temporary search probe counted 50,000 steps for 50,000 small
allocations and 40,609 for 30,000 mixed allocations. These counts are from a
different short diagnostic workload, not a claim that all free-list searches
are constant time. Hardware counters and lock wait times were not obtainable
from the existing benchmark suite; the table does not substitute guesses for
measurements.

## Original baseline and method

Baseline means the dirty working tree **before the direct-resize change**;
it already contains the eight-arena work recorded above. The original source
snapshot was saved before editing. CPU: Intel Core i7-1355U, 12 logical CPUs,
one NUMA node. OS: Linux `7.0.0-38-generic`, x86-64. Compiler: GCC 15.2.0,
glibc 2.43. CMake Release defaults apply (see `CMakeLists.txt` and generated
`out/research_baseline/compile_commands.json`); `HM_STATS=ON`, `HM_DEBUG=OFF`,
`HM_ARENAS=8`. The installed jemalloc is
`/usr/lib/x86_64-linux-gnu/libjemalloc.so.2`; no mimalloc library was found,
so mimalloc was **not benchmarked**. Release baseline tests: 12/12 pass.

`bench_compare` has 4,096 live slots with deterministic random operations and
seven repeats. The paired `bench_threads` tests 64 slots per worker, one to
16 threads, deterministic sizes, and a barriered handoff pattern. It times
whole worker phases including synchronization. `bench_direct_resize` repeats
512-to-256 KiB `realloc` 2,000 times, verifies preserved bytes, and frees.
The runner rotates allocator order and rejects mismatched checksums. Its
`/usr/bin/time` fields are process user/system CPU seconds and peak **RSS** in
KiB. The optional direct-resize latency mode times each `realloc` call with
`CLOCK_MONOTONIC` and reports p50/p95/p99 within each run; timer overhead is
included. `hm_stats.current_heap_bytes` is HyperMalloc's currently mapped
**virtual address space**, not committed or resident memory. Neither number
alone measures peak live payload. Other workloads report whole-run time, not
per-operation latency or cache misses.

| Baseline workload | glibc median | HyperMalloc median | Repeats |
|---|---:|---:|---:|
| single-thread small, 100k operations | 0.004854 s | 0.009160 s | 7 |
| single-thread mixed, 100k | 0.014601 s | 0.036424 s | 7 |
| single-thread large, 20k | 0.192713 s | 0.587454 s | 7 |
| small, 1 thread × 50k | 0.001201 s | 0.007249 s | 5 |
| small, 8 threads × 50k | 0.001967 s | 0.081022 s | 5 |
| mixed, 8 threads × 30k | 0.006838 s | 0.050685 s | 5 |
| direct, 8 threads × 1k | 0.006805 s | 0.163575 s | 5 |
| handoff, 8 threads × 32,768 | 0.012477 s | 0.197522 s | 5 |
| direct shrink, 2,000 cycles | 0.017510 s* | 0.371510 s | 7 |

*The direct-shrink glibc figure comes from the later matched before/after
session. The independent baseline session reported 0.018421 s glibc and
0.397583 s HyperMalloc. The later matched session is used for the change
calculation. Raw files are retained under `benchmarks/results/`.

## Optimization 1: direct-map resize within capacity

1. **Problem.** `hm_realloc` moved every direct allocation, even when the
   existing mapping contained enough space. A large shrink mapped fresh pages,
   copied hundreds of KiB, and unmapped the original.
2. **Existing implementation.** The ordinary-block in-place branch was gated
   by `direct == NULL`; all direct cases fell to `malloc_locked`, `memcpy`,
   `free_locked`.
3. **Evidence.** The baseline direct-shrink median was 0.371510 s/2,000
   cycles in the matched run, versus 0.017510 s for glibc. The two mapping
   operations and full data copy follow directly from the code path.
4. **External research.** Allocators such as jemalloc manage large extents
   separately, and glibc exposes a tunable mmap threshold. Linux permits
   partial `munmap` of page-aligned ranges. This supports a localized change.
5. **Design.** If a direct block's payload capacity covers the new request,
   retain its pointer. For shrink, round the needed mapping end to a page and
   unmap any complete trailing pages. Preserve the descriptor and hash key.
6. **Implementation.** `hm_realloc` updates its block size, descriptor length,
   per-arena mapped/live/fragmentation counters, and global atomic totals
   only after a successful tail unmap. If `munmap` fails, the resize still
   succeeds in place without trimming. Growth into page-rounding slack also
   remains in place. The existing copy fallback handles requests exceeding
   capacity.
7. **Files.** `src/core.c`, `tests/test_direct_shrink.c`, `CMakeLists.txt`,
   `benchmarks/bench_direct_resize.c`, `scripts/bench-direct-resize.py`; the
   preload API test was also extended for API coverage.
8. **Correctness.** Release 13/13, Debug 13/13, ASan/UBSan 11/11 with leak detection off,
   TSan 11/11. The new test checks pointer identity, data, alignment, `errno`,
   stats, heap invariants, failure preservation, and mapped-byte reclamation.
   Valgrind Memcheck reports zero errors on the test, but reports zero heap
   allocations because it does not treat these direct `mmap` regions as malloc
   allocations.
9. **Benchmark.** Seven rotated runs, same machine and Release flags, 2,000
   cycles each, identical checksum. Raw samples include wall, user/system CPU,
   and peak RSS. The candidate and baseline programs were built from the
   current and saved pre-change code respectively.
10. **Before/after.** HyperMalloc median 0.371510 s → 0.018937 s. Absolute
    reduction 0.352573 s. Throughput 5,383 → 105,612 cycles/s. Wall time
    decreased 94.9%, or throughput increased about 1,862%. Median measured
    system CPU time fell from 0.32 s to 0.01 s. The seven baseline times span
    0.350883–0.400461 s; candidate 0.018402–0.023328 s.
    A separate five-run sampled-latency session measured the `realloc` call
    itself (median of each run's percentile, nanoseconds):

    | Allocator | p50 | p95 | p99 |
    |---|---:|---:|---:|
    | original HM | 170,498 | 229,989 | 278,073 |
    | final HM | 2,221 | 3,741 | 9,752 |
    | glibc | 2,046 | 4,011 | 8,284 |
    | jemalloc | 357 | 379 | 409 |

    These are instrumented-call latencies with timer overhead, not the
    uninstrumented whole-cycle timings above. Raw samples are in
    `benchmarks/results/research_direct_latency.csv`.
11. **Memory.** The test confirms mapped virtual bytes fall on shrink and
    return to their starting value after free. Median peak RSS was 2,028 KiB
    before and 1,924 KiB after in this narrow workload. RSS is noisy at this
    scale; this is no claim of a general 104 KiB memory saving. Whole-page
    trimming reduces direct-block internal fragmentation when possible.
12. **Trade-offs.** A shrink now makes one `munmap` syscall instead of
    allocate/copy/free. The mapping can still retain up to one partial page of
    slack. The pointer remains stable; software that relied on realloc always
    moving was already outside the allocation contract.
13. **Decision.** **Kept.** The targeted gain is far larger than sample
    variability. Unrelated thread workloads are reviewed separately below.

## Benchmark comparison after Optimization 1 and regression review

The following medians are whole-workload seconds, five repeats per allocator
and configuration, lower is better. `bench_threads` uses the same binary's
standard `malloc` path for glibc and jemalloc (the latter via `LD_PRELOAD`),
and its `hm_*` path for the baseline and final builds. Each cell has the same
operation count and checksum. The final build was the direct-resize candidate;
these workloads do not call realloc, so their A/B differences cannot be
attributed to the new resize branch. They show normal run and code-layout
variation, not a general throughput gain.

| Workload | Threads | Ops/thread | glibc | jemalloc | Original HM | Final HM |
|---|---:|---:|---:|---:|---:|---:|
| small | 1 | 50,000 | 0.001218 | 0.001490 | 0.007269 | 0.007351 |
| small | 8 | 50,000 | 0.002173 | 0.003087 | 0.084322 | 0.085901 |
| small | 16 | 50,000 | 0.005038 | 0.004004 | 0.181724 | 0.185830 |
| mixed | 1 | 30,000 | 0.004367 | 0.001856 | 0.007270 | 0.007790 |
| mixed | 8 | 30,000 | 0.006706 | 0.003473 | 0.048762 | 0.052314 |
| mixed | 16 | 30,000 | 0.013252 | 0.006234 | 0.105662 | 0.115622 |
| direct | 1 | 1,000 | 0.003016 | 0.003322 | 0.010992 | 0.010711 |
| direct | 8 | 1,000 | 0.006469 | 0.006379 | 0.154732 | 0.158256 |
| direct | 16 | 1,000 | 0.014462 | 0.012285 | 0.347133 | 0.365216 |
| handoff | 1 | 32,768 | 0.001234 | 0.001462 | 0.005426 | 0.005649 |
| handoff | 8 | 32,768 | 0.012367 | 0.013395 | 0.180852 | 0.158922 |
| handoff | 16 | 32,768 | 0.048423 | 0.040612 | 0.382567 | 0.390631 |

The initial five-run comparison suggested a 9.4% mixed 16-thread regression.
A 15-repeat follow-up found 0.110810 s baseline vs 0.114664 s final (3.5%
slower), with heavily overlapping ranges of 0.104036–0.119164 s and
0.105102–0.122680 s. Direct 16-thread medians reversed direction in the
follow-up (0.375569 vs 0.372996 s). Handoff 16-thread medians were
0.385967 vs 0.355059 s, also with wide overlapping ranges. These short,
contended tests do not resolve small changes reliably. No unrelated
performance improvement or regression is claimed. The large-resize result is
separately decisive: 0.371510 → 0.018937 s in the matched seven-run session;
glibc 0.017510 s and jemalloc 0.001371 s in that same session. Mimalloc was
unavailable and is absent from the table.

The raw data are `benchmarks/results/research_baseline_single.csv`,
`research_baseline_threads.csv`, `research_baseline_direct_resize.csv`,
`research_final_direct_resize.csv`, `research_final_threads.csv`, and
`research_regression_followup.csv` in that directory. The source difference
between the exact saved original and final `src/core.c` is
`benchmarks/results/research_direct_resize.patch`. To reconstruct the
pre-optimization implementation in a **separate copy** of this dirty tree,
apply that patch in reverse there; never apply it to a workspace containing
new user changes. For example, from the separate copy's root:

```sh
git apply -R benchmarks/results/research_direct_resize.patch
cmake -S . -B out/research_baseline -DCMAKE_BUILD_TYPE=Release -DHM_ARENAS=8
cmake --build out/research_baseline -j 8
```

Rebuild the final tree without reversing the patch and rerun
`scripts/bench-direct-resize.py` and `scripts/bench-threads.py` using their
`--help` options for the exact arguments. `strace` was available but ptrace
was denied in this environment, so syscall counts were inferred from the
readable allocation path rather than reported as measured syscall totals.
`perf stat` was also denied (`perf_event_paranoid=4`), so there are no cache
miss, cycle, or branch-counter claims. The targeted direct-resize benchmark
samples p50/p95/p99 `realloc` latency; the broader suites do not sample
individual-operation latency or mutex wait time. These remain measurement
gaps, rather than inferred results.

To reproduce the matched runs, build one separate checkout with the patch
reversed as above and the working tree normally in Release mode, then run:

```sh
python3 scripts/bench-direct-resize.py \
  --baseline /tmp/hm-baseline/out/research_baseline/bench_direct_resize \
  --candidate out/research_candidate/bench_direct_resize \
  --jemalloc /usr/lib/x86_64-linux-gnu/libjemalloc.so.2 \
  --output out/research/direct_resize.csv
python3 scripts/bench-threads.py \
  --program out/research_candidate/bench_threads \
  --baseline-program /tmp/hm-baseline/out/research_baseline/bench_threads \
  --preload jemalloc=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 \
  --threads 1 --threads 4 --threads 8 --threads 16 \
  --output out/research/threads.csv --repeats 5
python3 scripts/bench-workloads.py \
  --baseline /tmp/hm-baseline/out/research_baseline/bench_workloads \
  --candidate out/research_candidate/bench_workloads \
  --jemalloc /usr/lib/x86_64-linux-gnu/libjemalloc.so.2 \
  --output out/research/workloads.csv --repeats 5
```

The `/tmp/hm-baseline` path is illustrative; use the actual separate copy.
Run these suites sequentially on an otherwise idle machine. No run in this
report was CPU pinned or isolated from other system activity, so very short
workloads require more repeats before small differences can be trusted.

### Broader workload matrix

`bench_workloads` adds immediate small/medium/large/zero allocations,
requests straddling the 128 KiB direct threshold, batches with many
simultaneous live allocations, alternating live/free holes, over-aligned
allocation, and a record-cache simulation with three differently sized fields
per record. Five rotated repeats compare the saved original archive build,
final build, glibc, and jemalloc; every case has matching checksums. These are
transaction counts, not always individual allocation counts: `records` can
allocate three fields per transaction, and `holes` replaces half the objects.
Raw observations, CPU time, peak RSS, and HyperMalloc mapped-byte counters are
in `benchmarks/results/research_workloads.csv`.

| Case | Transactions | glibc | jemalloc | Original HM | Final HM |
|---|---:|---:|---:|---:|---:|
| immediate small | 50,000 | 0.001056 | 0.001182 | 0.006983 | 0.007805 |
| immediate medium | 20,000 | 0.001205 | 0.001381 | 0.004038 | 0.004178 |
| immediate large | 500 | 0.000181 | 0.000439 | 0.006204 | 0.006405 |
| immediate zero | 50,000 | 0.000855 | 0.000979 | 0.007202 | 0.008219 |
| threshold ±512 B | 500 | 0.000053 | 0.000267 | 0.003734 | 0.003459 |
| batch | 30,000 | 0.006973 | 0.007101 | 0.034091 | 0.030591 |
| alternating holes | 30,000 | 0.011842 | 0.004911 | 0.020941 | 0.019806 |
| aligned | 1,000 | 0.000182 | 0.000148 | 0.007528 | 0.008093 |
| record cache | 20,000 | 0.005857 | 0.004493 | 0.012516 | 0.011455 |

Times are median seconds. The apparent small and zero regressions in five
runs were checked with 15 more paired repeats: small 0.007483 → 0.007545 s,
zero 0.009186 → 0.009169 s, both within ordinary run variation. Those raw
runs are in `research_workloads_followup.csv`. The other cases do not resize
direct maps and are not claimed as gains or regressions of Optimization 1.
The near-threshold case alternates ordinary and direct allocation paths; its
small transaction count makes sub-millisecond comparisons particularly noisy.

After all objects are freed, HyperMalloc retains 262,144 mapped **virtual**
bytes in the small, medium, zero, threshold, batch, holes, and record cases:
the arena's one empty chunk. It retains zero mapped bytes after the immediate
large and aligned cases, which use direct maps. The batch peak was 2,359,296
mapped bytes and the holes peak 1,310,720 bytes in both versions. These
figures are HyperMalloc metadata counters; the glibc/jemalloc zeroes in those
CSV fields mean **not measured**, not zero memory use. Peak RSS is measured
separately for all allocators. The suite still lacks synchronized peak live
and peak internal-fragmentation samples, so it does not prove optimal chunk
packing or a fragmentation win.

## Correctness audit, validation, and limits

The audit followed overflow checks in `hm_malloc`, `hm_calloc`, `hm_realloc`,
`reallocarray`, and direct mapping length calculations; split and coalescing
flags; free-block footer placement; hash unlink on direct free; owner lock
selection; alignment and standard wrapper error handling; and stats/checker
reconstruction. The new tests cover large shrink/growth and preload wrappers
that were previously untested (`reallocarray`, `memalign`, `valloc`, invalid
`posix_memalign`). Existing `test_core`, `test_size_classes`, `test_trim`,
`test_threads`, and seeded `fuzz_ops` cover other boundaries and races. This
audit found a clear performance defect, not a proven preexisting corruption
bug. Invalid-pointer detection remains best effort; metadata can still be
forged by a malformed interior pointer. The checker does not make arbitrary
invalid input safe.

An initial ASan invocation failed at process exit because LeakSanitizer cannot
run under this environment's ptrace wrapper. `ASAN_OPTIONS=detect_leaks=0`
allows AddressSanitizer and UndefinedBehaviorSanitizer checks and all 11 tests
pass. TSan's 11 tests pass. Sanitized CMake intentionally omits preload tests
because allocator interposition conflicts with sanitizer runtimes. Valgrind
observes address accesses but does not provide an object-leak census for the
custom mapped heap. Hardware `perf` events and direct lock-wait metrics were
not gathered; no claims are made for them.

## Architecture after Optimization 1 and remaining opportunities

The original arena/chunk/free-list structure remains. Direct maps now support
in-place size changes within current payload capacity; shrinking releases
complete trailing pages and keeps the same descriptor and hash bucket. Requests
exceeding capacity retain the old allocate-copy-free fallback. The mutex still
covers direct metadata changes. No new lock order or atomic publication path
was introduced.

The 31 October continuation below adds a record-cache simulation and removes
the redundant free-path lookup. Further investigations should measure
owner-probe counts and lock waits on handoff, bound and test a thread cache
with explicit retention limits, sweep the direct threshold with RSS and mmap
counts, and construct an adversarial free-list search workload before changing
size classes. Broader application traces and per-operation latency sampling
beyond direct resize would improve external validity.

## References

- [jemalloc tuning guide](https://github.com/jemalloc/jemalloc/blob/dev/TUNING.md), [arena implementation](https://github.com/jemalloc/jemalloc/blob/dev/src/arena.c), [tcache implementation](https://github.com/jemalloc/jemalloc/blob/dev/src/tcache.c)
- [mimalloc repository and design description](https://github.com/microsoft/mimalloc), [purging options](https://microsoft.github.io/mimalloc/environment.html)
- [snmalloc repository and design description](https://github.com/microsoft/snmalloc), [remote-deallocation source](https://github.com/microsoft/snmalloc/blob/main/src/snmalloc/mem/corealloc.h)
- [rpmalloc repository and design description](https://github.com/mjansson/rpmalloc), [change log](https://github.com/mjansson/rpmalloc/blob/develop/CHANGELOG)
- [glibc malloc tunables](https://sourceware.org/glibc/manual/latest/html_node/Memory-Allocation-Tunables.html)
- [Linux mmap/munmap manual](https://man7.org/linux/man-pages/man2/mmap.2.html)

## 31 October continuation: small allocations and cross-thread frees

### The measured flow

For an ordinary small allocation, `hm_malloc` chooses the thread's arena,
locks it, rounds the request plus the 16-byte header, uses `size_class` and
`free_bitmap` to find a candidate, unlinks that block, splits it if the tail
can hold a 48-byte minimum block, updates accounting, and unlocks. The free
path does more work: `hm_free` calls `lock_owner`, which locks candidate arenas
one at a time, checks the direct-map hash, and scans ordinary chunk ranges to
validate the pointer. The original `free_locked` then repeated the direct
hash or chunk lookup **under the same lock**, even though `lock_owner` had
already returned the validated `Block` and `DirectMap`. On a cross-thread
free, preceding nonowner arenas also incur lock/unlock and registry probes.

Hardware `perf` remains unavailable in this environment. I used Valgrind
Callgrind instead to count executed instructions (not wall time or cache
misses) in the unmodified 30 October build. The one-thread 5,000-operation
small workload executed 3,424,923 instructions. `lookup_chunk_block` alone
accounted for 350,035 (10.2%); `lock_owner` for 305,000 (8.9%); libc mutex
lock/unlock for 540,998 (15.8%). A two-thread, 4,096-operation-per-thread
handoff run executed 12,492,134 instructions: `lookup_chunk_block` 897,094
(7.2%), `lock_owner` 1,187,840 (9.5%), and libc mutex lock/unlock 2,655,273
(21.3%). These figures overlap in causal interpretation: they are exclusive
function instruction counts, not proof of elapsed-time share. Calls to
`remove_free` and `insert_free` consumed another 39.5% of handoff instructions,
reflecting frequent coalescing/list updates. The profile supports removing
the redundant lookup first; it does not show that the free list itself should
be redesigned.

The saved pre-change timing baseline is
`benchmarks/results/lookup_before.csv`: ten runs of small, mixed, and handoff
at one, four, eight, and sixteen threads, plus glibc control runs. The binary
is the prior final `out/research_candidate/bench_threads`, before this change.
The exact pre-change `src/core.c` was also copied to a temporary checkpoint
before editing. Paired after measurements and the keep/revert decision follow
below.

### Optimization 2: pass validated ownership into the free path

1. **Problem.** Every ordinary `hm_free` walked its owning arena's chunk list
   twice, and direct frees looked up their hash bucket twice. `hm_realloc`'s
   moving fallback did the same when freeing the old pointer.
2. **Existing implementation.** `lock_owner(ptr, &block, &direct)` already
   identified a valid pointer under the owner mutex. The former
   `free_locked(ptr)` discarded those results and called `lookup_direct` or
   `lookup_chunk_block` again before freeing. The caller also checked the
   allocation bit, so the second lookup added no validation guarantee.
3. **Evidence.** Callgrind's `lookup_chunk_block` count was 350,035
   instructions (10.2% of total) in the small probe and 897,094 (7.2%) in
   the two-thread handoff probe. The duplicate call is visible in the source.
   The ten-run pre-change timings are in `lookup_before.csv`.
4. **Research connection.** Established allocators use ownership metadata to
   avoid repeated pointer classification on the hot path; see the
   [mimalloc](https://github.com/microsoft/mimalloc) and
   [snmalloc](https://github.com/microsoft/snmalloc) design references above.
   This change does **not** adopt their page metadata or remote-free queues;
   it only reuses HyperMalloc's already validated result.
5. **Proposed solution.** Pass `Block *` and `DirectMap *` from `lock_owner`
   through `hm_free` or `hm_realloc` to `free_locked` while the owner mutex
   remains held. Use `direct->prev` for the registry unlink. Keep the existing
   validity and allocation-bit checks in the public callers.
6. **Implementation.** `free_locked(void *ptr, Block *block,
   DirectMap *direct)` now consumes those validated arguments. It no longer
   scans the chunk list or direct hash to rediscover the same object. Direct
   hash-chain unlink still follows the matching bucket link, because that
   link itself must be updated. No lock or allocation policy changed.
7. **Files changed.** `src/core.c`; this record and raw results in
   `optimizations.md` and `benchmarks/results/`. The isolated source delta is
   `benchmarks/results/lookup_change.patch`; reverse it in a separate copy to
   reconstruct the prior build. No public API was changed.
8. **Correctness.** Release 13/13, Debug 13/13, ASan/UBSan 11/11 with
   `ASAN_OPTIONS=detect_leaks=0`, and TSan 11/11 pass. Existing tests cover
   direct and ordinary frees, invalid/double frees, cross-thread frees,
   realloc moves, heap invariants, and preload use. No test was added solely
   to mirror a private function signature.
9. **Benchmark method.** GCC Release, eight arenas, same machine and
   settings as the prior section. `scripts/bench-threads.py` ran ten matched
   repeats across 1/4/8/16 threads with rotated allocator order and identical
   checksums; twenty-repeat follow-ups tested the most relevant/noisy cases.
   `scripts/bench-workloads.py` ran ten matched repeats on small, medium,
   zero, batch, holes, and record-cache cases. Callgrind used identical
   5,000-operation small and 2 × 4,096-operation handoff probes before/after.
10. **Before/after.** Callgrind totals: small 3,424,923 → 3,134,926
    instructions (289,997 fewer, 8.5%); handoff 12,492,134 → 12,016,996
    (475,138 fewer, 3.8%). `lookup_chunk_block` itself fell from 350,035 to
    175,035 small-probe instructions and 897,094 to 610,374 handoff
    instructions. These are deterministic instruction counts under Valgrind,
    not native elapsed time.

    | Native workload | Repeats | Previous median | New median | Wall-time change | Paired new wins |
    |---|---:|---:|---:|---:|---:|
    | small, 1 thread × 50k | 20 | 0.008112 s | 0.007532 s | 7.2% lower | 18/20 |
    | small, 8 threads × 50k | 20 | 0.087199 s | 0.086059 s | 1.3% lower | 12/20 |
    | small, 16 threads × 50k | 20 | 0.178636 s | 0.176459 s | 1.2% lower | 11/20 |
    | handoff, 8 threads × 32,768 | 20 | 0.188396 s | 0.139884 s | 25.7% lower | 16/20 |
    | handoff, 16 threads × 32,768 | 20 | 0.403581 s | 0.409844 s | 1.6% higher | 11/20 |
    | immediate small, 50k | 10 | 0.009351 s | 0.008360 s | 10.6% lower | 9/10 |
    | record cache, 20k transactions | 10 | 0.013316 s | 0.010473 s | 21.4% lower | 9/10 |

    One-thread small throughput rose from about 6.16 million to 6.64 million
    benchmark iterations/s (7.7%). The native handoff results have much
    wider dispersion: eight-thread previous runs span 0.131–0.269 s and new
    runs 0.117–0.216 s; the median paired previous/new ratio is 1.22. The
    16-thread handoff median reversal is within noise (11 of 20 paired runs
    still favor the new version). Five other application-style cases and all
    per-repeat values are in `lookup_workloads.csv`; do not infer a universal
    21% application gain from this one record-cache simulation.
11. **Memory and fragmentation.** Across matched `bench_workloads` runs,
    HyperMalloc's mapped-after and peak-mapped counters were unchanged for
    every case: for example, both versions retained one 262,144-byte empty
    chunk after the small workload, and both peaked at 2,359,296 mapped bytes
    in the batch/record cases. Peak RSS differences were a few KiB and do not
    support a memory improvement claim. Block sizes and coalescing are
    unchanged.
12. **Trade-offs.** `free_locked` now relies explicitly on its caller's
    owner-locked, validated arguments; that invariant is documented at the
    function. This removes repeated work but leaves the original cross-arena
    probe and mutex contention. The extra direct-map argument is private to
    `src/core.c`; it does not affect the public API or metadata size.
13. **Decision.** **Kept.** Deterministic instruction savings and consistent
    one-thread small gains justify the simple change. The eight-thread
    handoff gain is promising but variable; 16-thread throughput is
    inconclusive. No broad claim that HyperMalloc now matches glibc or
    jemalloc follows from these results.

Raw files: `benchmarks/results/lookup_before.csv`, `lookup_after.csv`,
`lookup_followup.csv`, `lookup_followup_8.csv`, `lookup_workloads.csv`, and
`lookup_change.patch`. The old and new Callgrind profiles are in ignored
`out/profile_*` build artifacts; the exact totals and commands are recorded
above. Reproduce the focused comparison with:

```sh
python3 scripts/bench-threads.py \
  --program out/lookup_candidate/bench_threads \
  --baseline-program out/research_candidate/bench_threads \
  --workload small --workload mixed --workload handoff \
  --threads 1 --threads 4 --threads 8 --threads 16 \
  --repeats 10 --output out/lookup-comparison.csv
```

The paths name the two saved build directories from this investigation. To
rebuild the previous version later, use a separate tree with
`lookup_change.patch` applied in reverse. The benchmarks are the same as the
previous section, so their remaining limitations (no cache counters or lock
wait timings, scheduling variability, synthetic workload mix) still apply.

### Current comparison with glibc and jemalloc

A separate ten-repeat matched session used the same current binary for
HyperMalloc and its standard `malloc` path, with jemalloc selected only by
`LD_PRELOAD`. Raw runs and checksums are in
`benchmarks/results/lookup_comparison.csv`. Median whole-workload seconds:

| Workload | Threads | glibc | jemalloc | Before Optimization 2 | Current HyperMalloc |
|---|---:|---:|---:|---:|---:|
| small | 1 | 0.001389 | 0.001739 | 0.008291 | 0.007777 |
| small | 8 | 0.002085 | 0.002990 | 0.084940 | 0.087011 |
| mixed | 1 | 0.004845 | 0.002043 | 0.008106 | 0.008009 |
| mixed | 8 | 0.007197 | 0.003758 | 0.053923 | 0.054921 |
| handoff | 1 | 0.001316 | 0.001451 | 0.005999 | 0.005843 |
| handoff | 8 | 0.013396 | 0.015626 | 0.203737 | 0.167460 |

HyperMalloc remains substantially slower than both comparators in these
cases. The small eight-thread before/after ordering reverses across sessions,
which reinforces the conservative interpretation above. Jemalloc is the only
third-party comparator installed here; mimalloc was unavailable.

### Current hot-path flow and next measurement

```text
hm_malloc(size)
  choose thread's home arena -> lock it
  round header + payload -> find class/bitmap fit (or map a chunk)
  unlink and possibly split a free block -> account -> unlock -> return

hm_free(ptr)
  try home arena, then other arenas until owner is found
    lock candidate -> check direct hash -> check ordinary chunk ranges
    unlock nonowner candidate; retain owner lock
  validate allocated bit once -> pass found Block/DirectMap to free_locked
    direct: unlink hash/registry, account, munmap
    ordinary: mark free, coalesce neighbors, insert on class list,
              possibly release an empty chunk
  unlock owner arena
```

The remaining cross-thread cost is now more visible: each nonowner arena
requires a mutex acquisition and a negative ownership probe. The handoff
profile also spends many instructions maintaining free lists after rapid
allocate/free cycles. A future ownership index or remote-free queue would
change synchronization invariants and retained metadata, so it needs direct
probe-count and lock-wait measurements plus a memory-footprint comparison
before implementation. This investigation leaves that larger change open.

### Real-program preload smoke test (31 October)

The current Release build was also run with an unmodified `/usr/bin/sort`.
A deterministic 300,000-line, 16.2 MB input was sorted with four requested
workers and a 64 MB buffer, once with glibc and once with
`LD_PRELOAD=out/release/libhypermalloc_preload.so`. Both processes exited zero;
`cmp` found identical output and both output SHA-256 hashes were
`57b7e6b18a638ccccb42ad9d06dffee4bff8ee36cceb9bbec7b49777b61a9aae`.
`LD_DEBUG=libs` confirmed the loader called HyperMalloc's shared-library
initializer. The release CTest suite passed 13/13 immediately beforehand.

| Run | Wall time | User CPU | System CPU | Peak RSS |
|---|---:|---:|---:|---:|
| glibc | 0.16 s | 0.26 s | 0.03 s | 31,220 KiB |
| HyperMalloc preload | 0.15 s | 0.23 s | 0.04 s | 31,348 KiB |

This is one functional smoke-test run, not a throughput comparison: the
duration is short and only one sample was taken. The input and outputs were
written under `/tmp` and are not needed by the repository. A minimal repeat
of the preload part with any input file is:

```sh
cmake --preset release
cmake --build out/release -j 4
LD_PRELOAD="$PWD/out/release/libhypermalloc_preload.so" \
  /usr/bin/sort --parallel=4 --buffer-size=64M input.txt -o sorted.txt
```
