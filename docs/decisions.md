# Design decisions

## `mmap` chunks and direct maps

The allocator uses anonymous `mmap` for both 256 KiB ordinary chunks and
direct mappings at 128 KiB or above. This preserves the repository's existing
OS-memory choice and avoids `sbrk` coordination with another allocator.
Alternative: use `sbrk` for small allocations; rejected because process-wide
program break ownership is harder to reason about, especially with preload.
Ordinary chunks remain mapped for reuse. Direct mappings are unmapped on free.
Trimming is deferred until the checker, tests and preload path are stronger.

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
from the original implementation; address-ordered insertion is a candidate
for the required optimization experiment, after reproducible benchmarks exist.

## Global pthread mutex

A statically initialized mutex protects all allocator state. It is available
before constructors run and does not require a heap allocation for setup.
Alternative: per-thread arenas or caches; deferred because their ownership,
cross-thread free, and fork behavior require much more validation. The global
lock makes the core correct under threads but limits throughput under
contention. Fork handlers for the preload build are still outstanding.

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
it remains live. This is easier for drop-in use than returning NULL. The old
`hyper_malloc(0)` wrapper retains its historical NULL behavior for existing
clients. `hm_realloc(p,0)` frees and returns NULL.
