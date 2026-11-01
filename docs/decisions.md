# Design decisions

## `mmap` chunks and direct maps

The allocator uses anonymous `mmap` for both 256 KiB ordinary chunks and
direct mappings at 128 KiB or above. This preserves the repository's existing
OS-memory choice and avoids `sbrk` coordination with another allocator.
Alternative: use `sbrk` for small allocations; rejected because process-wide
program break ownership is harder to reason about, especially with preload.
At most one empty ordinary chunk per active arena remains mapped for reuse;
further empty chunks in that arena are unmapped. Direct mappings are unmapped
on free. `test_trim` verifies that releasing ordinary chunks updates mapped-byte
accounting.

## Free-only footers and immediate coalescing

Allocated blocks carry only a 16-byte header. Free blocks add an eight-byte
footer, which gives O(1) access to the previous block's size when it is free.
Alternative: store explicit physical predecessor pointers in every header,
as the audited implementation did; rejected because it increases live-block
overhead. Alternative: deferred coalescing; rejected for now because it
complicates the no-adjacent-free-block invariant and increases fragmentation
uncertainty. The checker verifies the immediate-coalescing invariant.

## Size classes, bitmap and insertion order

Fine 16-byte classes cover blocks through 256 bytes. Quarter-power classes
cover larger blocks. This is a conservative compromise between exact-size
lists (many heads) and the original power-of-two classes (longer searches).
The free-list bitmap and `ctz` skip empty classes. LIFO insertion is retained
from the original implementation.

## Direct-map lookup experiment

The first implementation searched a single direct-map list for every free,
realloc, and usable-size query. The large-allocation benchmark exposed this
linear lookup cost. A fixed 1,024-bucket pointer hash now finds direct maps,
while doubly linked registry nodes allow constant-time unlinking. The full
registry remains available to the checker. The before and after raw runs are
in `benchmarks/results/`; see `BENCHMARK_RESULTS.md` for medians and limits.

## Independent arenas

Up to eight arenas each own chunks, direct maps, free lists, a bitmap, stats,
and a statically initialized mutex. Threads choose a home arena once. Free,
realloc, and usable-size queries probe the home arena first and then other
arenas, so cross-thread frees remain valid. The heap checker, stats snapshot,
and fork handlers lock every arena in a fixed order. This reduces contention
for concurrent small and mixed allocations, at the cost of more retained
empty chunks and a search when another thread owns a pointer. The benchmark,
arena-count comparison, and limitations are in `optimizations.md`.

## Invalid-pointer handling

The pointer must be aligned and inside a known mapping. Direct maps require
an exact registry match; ordinary chunks check the candidate header and next
block's previous-allocated flag. This is best-effort detection: an aligned
interior pointer with forged metadata may pass. Debug builds write a short
message with `write(2)` and abort; release builds ignore an invalid free.
Alternative: a full physical walk on every free; rejected because it makes
freeing many blocks quadratic. Debug or hardened validation could add a
separate pointer index later.

## Zero-size allocation

`hm_malloc(0)` returns a distinct, freeable minimum-sized allocation while
it remains live. This is easier for drop-in use than returning NULL.
`hm_realloc(p,0)` frees and returns NULL.
