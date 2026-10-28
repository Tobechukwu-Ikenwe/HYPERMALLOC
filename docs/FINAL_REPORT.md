# Interim report (2026-10-28)

This repository does **not** yet meet the task's definition of done. Work
stopped after the audit and one reviewed core bug fix. No performance claim in
this report should be read as a finished benchmark result.

## Completed work

| Phase | Outcome |
|---|---|
| 1 — audit | Completed in `83abc44`. [AUDIT.md](AUDIT.md) maps the working-tree modules, records baseline execution, defects and a migration plan. |
| 2 — core | Started. Commit `ae0b500` fixes realloc shrink swallowing its free tail and corrects `hyper_posix_memalign` for sizes that are not alignment multiples. It adds a focused CTest regression. Boundary tags and the requested `hm_*` API are **not** implemented. |
| 3–11 | Not completed. |

The working tree had substantial pre-existing modifications before the audit.
The second commit included those pre-existing modifications in the two source
files and `CMakeLists.txt` that it touched. Generated build files in `build/`
remain tracked and modified. Do not assume those changes were introduced by
this task.

## Exact verification and observed results

The commands below were run from the repository root. The `/tmp` probe used
during the audit was temporary; its source and original output are documented
in [AUDIT.md](AUDIT.md).

```sh
cmake -S . -B /tmp/hm-audit-release -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/hm-audit-release -j 4
ctest --test-dir /tmp/hm-audit-release --output-on-failure
cmake -S . -B /tmp/hm-audit-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build /tmp/hm-audit-asan -j 4
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir /tmp/hm-audit-asan --output-on-failure
```

Both builds succeeded. After the fix, CTest reported one passing test and
zero failures in both configurations. Before the fix, CTest found no tests.
The original probe printed `shrink_usable=1024`; after the fix it printed
`shrink_usable=64` and `check_heap=1`. Valgrind on the original probe reported
zero errors, but this does not cover the allocator's full operation space.
LeakSanitizer failed to initialize in this environment because of ptrace;
ASan+UBSan testing therefore used `ASAN_OPTIONS=detect_leaks=0`.

One baseline execution of `timeout 20 /tmp/hm-audit-release/benchmark_reuse_mixed`
completed and printed `HyperMalloc reuse allocation: 0.004822 seconds`. This
single-run number is not comparable to libc and is deliberately excluded from
portfolio results. No headline benchmark numbers or résumé bullets can be
defended yet.

## Remaining work

1. Finish Phase 2 with boundary tags, region/large-map handling, documented
   semantics, statistics, and the `hm_*` API. Keep the build passing while
   replacing the unsound metadata core identified in the audit.
2. Build the complete heap checker and corruption tests (Phase 3).
3. Implement and run all required unit, stress, thread and invalid-use tests
   under Release, ASan+UBSan and TSan (Phase 4).
4. Implement the differential fuzzer and run/document the real soak count
   (Phase 5).
5. Build and validate the preload library (Phase 6).
6. Build a reproducible, repeated-run benchmark harness with real results and
   graphs (Phase 7), then conduct one controlled optimization experiment
   (Phase 8).
7. Finish CI, hygiene, implementation-derived docs and clean-clone verification
   (Phases 9–11).

Known limitations remain those in [AUDIT.md](AUDIT.md), particularly the
partial checker, oversized metadata, no direct-map large allocation path, no
return of memory to the OS, and unregistered legacy tests. The current
regression test is evidence only for its two targeted cases.
