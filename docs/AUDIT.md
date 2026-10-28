# HyperMalloc audit (working tree, 2026-10-28)

This audits the working tree, including pre-existing uncommitted source changes. The
baseline is commit `7444e12`; `git status --short` before the audit showed changes
to the build, header, allocator, README, and generated build files. None of those
changes were made by this audit. Reproduce the snapshot inventory with
`rg --files -g '!build*'` and `git status --short`.

## File and module map

| Path | Role/status |
|---|---|
| `include/hypermalloc.h` | Public `hyper_*` declarations and `check_heap`; no `hm_*` API. |
| `src/internal.h` | Block structure, region and free-list declarations. `get_sbrk_calls` is outside its include guard (line 76). |
| `src/heap/heap.c` | Anonymous `mmap` regions, bump cursor, region containment, recursive pthread lock. |
| `src/block/block.c` | Pointer recovery/validation, splitting, forward coalescing, partial heap checker. |
| `src/freelist/free_list.c` | Logarithmic segregated free lists, bitmap, first fitting candidate search. |
| `src/hypermalloc/{hypermalloc,free,realloc}.c` | Active allocation API, aligned allocation, free and realloc. |
| `src/hyperfree/hyperfree.c`, `src/hyperrealloc/hyperrealloc.c` | Unbuilt stale alternate implementations; duplicate public symbols if linked. |
| `CMakeLists.txt` | Static library and five benchmarks; no CTest registrations. |
| `tests/` | Legacy assignment tests and benchmark programs; several refer to obsolete `hypermalloc` or `hyper_malloc.h` and are not built. |
| `benchmarks/` | Two ad hoc libc comparisons; no repeated-run methodology or machine-readable results. |
| `README.md`, `BENCHMARK_RESULTS.md` | Introductory notes and historical timings. The timings are not reproduced by this audit and describe an older implementation. |
| `build/` | Generated files and binaries tracked by Git; many are modified in the working tree. |

The history (`git log --oneline -10`) progresses from a memory acquisition
experiment through block metadata, reuse, segregated lists and optimization.

## Current allocation strategy

`BlockHeader` stores payload size, physical previous/next links, free-list
previous/next links, a free flag, magic and `max_align_t` padding
(`src/internal.h:11-29`). Payload starts at `block + 1`. The layout has no
boundary tags or free-block footer. On this x86-64 build the header is 96 bytes
(reproduce with `printf '...sizeof(BlockHeader)...'` in a small program, or
inspect the type in a debugger); the exact size is ABI dependent. Requests are
rounded to `_Alignof(max_align_t)` in `src/hypermalloc/hypermalloc.c:14-19`.

`src/heap/heap.c:48-70` acquires anonymous mappings, at least 64 KiB each,
and places a region descriptor at each start. The cursor carves individual
blocks. Separate regions remain mapped forever. There is no distinct large
allocation path, prologue, epilogue, or trim policy. Physical links span
regions even though they are not contiguous.

Free blocks enter one of `sizeof(size_t) * 8` logarithmic classes, at the list
head, with a 64-bit occupancy bitmap (`src/freelist/free_list.c:11-106`). Search
starts at the requested class and returns the first block large enough; class
ranges can require linear scans. Splitting creates a new header when there is
room for a header and one byte (`src/block/block.c:93-131`). Free coalesces
physically contiguous neighbors immediately; the previous neighbor is found
via an explicit physical link (`src/hypermalloc/free.c:34-56`).

`hyper_malloc(0)` and zero-size aligned allocation return NULL. `hyper_calloc`
checks multiplication overflow and zeroes via `memset`. `hyper_realloc(NULL,n)`
allocates; `hyper_realloc(p,0)` frees and returns NULL. Realloc can consume a
free following block but cannot extend into the chunk's unused top. Alignment
beyond `max_align_t` is implemented by splitting an oversized block into a
free prefix and aligned allocated suffix. The process-wide recursive pthread
mutex serializes operations. `check_heap()` returns 1 for healthy, 0 for an
error, opposite the requested `hm_check_heap` contract.

## Reproduced baseline behavior

Commands run from the repository root:

```sh
cmake -S . -B /tmp/hm-audit-release -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/hm-audit-release -j 4
ctest --test-dir /tmp/hm-audit-release --output-on-failure
timeout 20 /tmp/hm-audit-release/benchmark_reuse_mixed
cmake -S . -B /tmp/hm-audit-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build /tmp/hm-audit-asan -j 4
cc -Iinclude /tmp/hm-audit-probe.c /tmp/hm-audit-release/libhypermalloc.a -pthread -o /tmp/hm-audit-probe
/tmp/hm-audit-probe
valgrind --error-exitcode=99 --leak-check=full /tmp/hm-audit-probe
cc -Iinclude -fsanitize=address,undefined -fno-omit-frame-pointer /tmp/hm-audit-probe.c /tmp/hm-audit-asan/libhypermalloc.a -pthread -o /tmp/hm-audit-probe-asan
ASAN_OPTIONS=detect_leaks=0 /tmp/hm-audit-probe-asan
```

Both builds succeeded without compiler warnings. CTest reported **No tests
were found**. The release reuse benchmark exited successfully and printed
`HyperMalloc reuse allocation: 0.004822 seconds` for its one run; it is not a
meaningful comparison. The probe allocated two blocks, shrank the first from
1024 to 64 bytes, and printed `shrink_usable=1024 check_heap=1`, then
`post_alloc_check_heap=1` and `post_free_check_heap=1`. Valgrind reported
`ERROR SUMMARY: 0 errors from 0 contexts` for that probe. ASan+UBSan also
completed the probe without a diagnostic with leak detection disabled.
`ASAN_OPTIONS=detect_leaks=1` failed before the probe with
`LeakSanitizer has encountered a fatal error` and its ptrace incompatibility
hint; the sanitizer benchmark attempt failed the same way. These small probes
do not establish general allocator correctness.

## Bugs, undefined behavior and invariant gaps

1. **Realloc shrink is undone.** `src/hypermalloc/realloc.c:150-155` splits,
   then calls `coalesce_next(block)` on the *allocated* left block, absorbing
   the free remainder back into it. The probe above demonstrates the bug.
2. **Unsafe validity checks.** `src/block/block.c:27-44` accepts any in-region
   address whose preceding bytes resemble a valid header; it does not establish
   that the pointer is an allocation start. Invalid interior pointers can
   corrupt free lists. `src/freelist/free_list.c:117-130` dereferences list
   pointers before proving they lie in a region, and has no cycle bound.
3. **Incomplete heap checker.** `src/block/block.c:46-75` follows physical
   links rather than walking exact region layouts. It lacks block-size,
   footer, prev-allocated-bit, per-region accounting, free-list-count, cycle,
   and mapped-block checks. `free_list_contains` has an unbounded traversal.
4. **Alignment path contract mismatch.**
   `src/hypermalloc/hypermalloc.c:170-181` calls `hyper_aligned_alloc`, which
   rejects a size not divisible by the alignment at lines 105-110. POSIX
   `posix_memalign` permits such sizes.
5. **Failure/errno inconsistency.** `src/hypermalloc/hypermalloc.c:57-60`
   returns NULL after `mmap` failure without setting `ENOMEM` itself.
   `src/hypermalloc/realloc.c:135-145` invokes `perror` inside allocator code,
   violating the no-allocating-libc-call requirement.
6. **Non-minimal metadata and no OS return.** `src/internal.h:11-29` keeps
   physical and free-list links, free flag and magic on every allocated block.
   `src/heap/heap.c:48-70` never unmaps regions; large blocks are not direct
   mapped blocks.
7. **Split policy is weak.** `src/block/block.c:97-107` permits a one-byte
   residual payload and relies on caller alignment. There is no explicit
   minimum free-block invariant.
8. **Stale sources and tests.** `src/hyperfree/hyperfree.c` and
   `src/hyperrealloc/hyperrealloc.c` would conflict with the active API;
   `tests/test_splitting.c:59` and `tests/test_free_reuse.c:18` call the removed
   `hypermalloc` symbol. The stale files should be retained as history until
   replacements are tested, then removed in a separate commit.
9. **Unreliable historical performance claim.** `BENCHMARK_RESULTS.md` lacks
   machine/commit provenance and documents an older O(N) lookup. It should be
   archived or replaced by measured, reproducible results.

The core is unsuitable for extension by incremental metadata patches: the
explicit physical chain, oversized allocated headers, absent footers, and
region cursor semantics conflict with the required boundary-tag layout. Phase
2 should replace the metadata core while preserving useful API ideas, the
`mmap` source, segregated lists, bitmap lookup, reuse and immediate coalescing.

## Missing relative to the target

There is no `hm_*` API, requested stats, boundary tags, fine-grained classes,
large direct maps, O(1) previous coalescing via footer, robust checker,
registered unit/stress/thread tests, differential fuzzer, sanitizer presets,
shared preload library, real-program validation, reproducible benchmark suite,
profiling experiment, CI, or complete design/validation documentation.

## Migration plan

| Phase | Planned files |
|---|---|
| 2 | Extend `include/hypermalloc.h`; replace metadata and region operations in `src/internal.h`, `src/block/`, `src/heap/`, `src/freelist/`, `src/hypermalloc/`; retain `hyper_*` wrappers if useful; add layout and decisions docs. |
| 3 | Implement region/free-list/accounting checker near the metadata core; add corruption probes. |
| 4 | Replace stale tests with CTest executables under `tests/`, retaining any valid scenarios. |
| 5 | Add `tests/fuzz_ops.c`, CMake presets/options and check/fuzz scripts; record actual runs. |
| 6 | Add shared-library symbol wrappers and preload validation scripts. |
| 7 | Replace ad hoc benchmarks with a common-workload harness, result files and plotting script. |
| 8 | Profile, run one controlled change, document before/after or revert. |
| 9 | Add CI, hygiene files, development guide; remove tracked build artifacts only in a separate, explicit cleanup. |
| 10–11 | Complete implementation-derived docs and final clean-clone verification. |

Each phase must finish with a passing build and a focused commit. Historical
timings must not be copied into new results without reproduction.
