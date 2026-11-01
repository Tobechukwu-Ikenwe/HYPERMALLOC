# Heap invariants

`hm_check_heap()` returns zero when these invariants hold. A nonzero return is
an error code; `hm_check_heap_verbose()` writes the first failure's code,
address, expected value and actual value to standard error with `write(2)`.
Reproduce the corruption checks with `ctest --test-dir build -R checker
--output-on-failure` after a CMake build.

| Invariant | Established in `src/core.c` | Checked in `src/core.c` |
|---|---|---|
| Every ordinary block is at least 48 bytes and has a 16-byte multiple size. | `add_chunk`, `allocate_from_free`, `hm_realloc` split path | `check_locked` physical walk |
| The physical walk lands exactly on each epilogue; no chunk block crosses it. | `add_chunk`, split and coalesce paths | `check_locked` walk and size sum |
| Free block header and footer agree. | `write_footer` after free, split, and coalesce | `check_locked` free-block branch |
| No two adjacent ordinary free blocks remain. | `merge_and_insert` | `check_locked` previous-allocated state |
| Each block's previous-allocated bit matches the preceding block. | `add_chunk`, allocation, split and coalesce paths | `check_locked` physical walk and epilogue check |
| Every free-list node is a free chunk block in its size class, with matching backward link. | `insert_free`, `remove_free` | `check_locked` bounded list walk |
| The free-list bitmap agrees with nonempty lists and free-list counts match physical free counts. | `insert_free`, `remove_free` | `check_locked` bitmap/count comparison |
| Live and mapped byte accounting agrees with physical blocks and direct maps. | `account_alloc`, `account_free`, mapping paths, realloc update | `check_locked` accounting comparison |
| Process-wide atomic live and mapped totals agree with the sum of arena totals. | accounting helpers and mapping paths | `check_all_locked` aggregate comparison |
| Direct-map payloads have their recorded alignment and live map flag. | `allocate_direct` | `check_locked` direct-map walk |

`test_checker` deliberately changes a live block's header, a free block's
footer, and a free-list back pointer, checks that each mutation is detected,
then restores the original value. The checker is also called periodically by
`test_core`. With `HM_DEBUG=ON`, mutating operations check their selected
arena at the configured `HM_DEBUG_STRIDE`; explicit `hm_check_heap()` checks
every arena and the process-wide accounting totals.

The checker is diagnostic, not a hardened memory-safety boundary. A severely
corrupt pointer in the direct-map registry may make any heap walk unsafe. The
ordinary free path uses region and metadata plausibility checks; an aligned
interior pointer with forged metadata may still pass. These limits are
documented in [decisions.md](decisions.md).
