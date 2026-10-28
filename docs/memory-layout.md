# Memory layout (core implementation at `src/core.c`)

Reproduce the checks in this document with:

```sh
cmake -S . -B /tmp/hm-layout -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/hm-layout
ctest --test-dir /tmp/hm-layout --output-on-failure
```

The allocator assumes an ordinary 64-bit ABI where the `Block` and `Chunk`
structures in `src/core.c` occupy 16 and 32 bytes respectively. Every returned
payload begins at a 16-byte boundary. `test_core` checks this for sizes 1–4096
and for requested alignments up to 64 KiB.

## Boundary tags

Every block starts with two machine words:

```text
offset 0: requested bytes (size_t)
offset 8: total block size | flags (size_t)
offset 16: payload begins
```

The low four bits of the second word are available because block sizes are
multiples of 16. Bit 0 is `allocated`, bit 1 is `previous block allocated`, and
bit 2 marks a direct mapping. Bits 3 and 4–63 are unused and size respectively.
The total block size includes the 16-byte header. A free block also has a
copy of its header word in the final eight bytes, permitting O(1) lookup of a
free previous neighbor. Its first 16 payload bytes hold intrusive `prev` and
`next` free-list pointers:

```text
allocated: [requested:8 | size+flags:8 | user bytes ...]
free:      [requested:8 | size+flags:8 | prev:8 | next:8 | ... | footer:8]
```

Minimum block size is 48 bytes: 16 bytes of header, 16 bytes of links, eight
bytes of footer, and eight bytes of alignment padding. Splitting requires a
remainder of at least 48 bytes. The allocated-block metadata overhead is 16
bytes plus alignment padding; free blocks use another eight bytes for a
footer but no separate free-list node allocation. `hm_usable_size` reports
block size minus 16. `test_realloc_shrink` checks that shrinking actually
releases a viable tail.

## Chunk mappings

The ordinary path maps 256 KiB chunks. A 32-byte `Chunk` descriptor is at
the mapping start, followed by a 16-byte allocated prologue, a free block,
and a 16-byte allocated epilogue:

```text
mapping base
  [Chunk:32][prologue:16][ordinary blocks ...][epilogue:16]
```

The initial free block occupies the span between the prologue and epilogue.
The epilogue has size zero. All ordinary blocks in a chunk are contiguous;
heap walking must land exactly on the epilogue. The last free block is the
heap top, so realloc can grow into it if sufficient space remains. Chunks
stay mapped for reuse; there is currently no trim operation.

Requests of at least 128 KiB, or alignments above 16 bytes, get a direct
mapping. The `DirectMap` descriptor lives at its mapping start. The payload
address is rounded up to the requested alignment, with a 16-byte block header
immediately before it. The mapping is page rounded and unmapped on free.
Direct mappings are linked in a registry so the free and checker paths can
distinguish them from ordinary chunk blocks. `test_core` checks that freeing a
large direct mapping reduces `current_heap_bytes`.

## Segregated classes

Classes 0–13 have upper boundaries 48, 64, 80, ..., 256 bytes. Later classes
divide each doubling interval into quarters: 320, 384, 448, 512; 640, 768,
896, 1024; and so on. The final class catches any larger block. The bitmap
records which of the 64 lists are nonempty. Search uses `ctz` to find the
first eligible nonempty class, then first-fit within a class. Each list uses
LIFO insertion. `hm_check_heap` verifies class membership, list links,
bitmap agreement and free-block counts. Class-boundary unit tests are still
needed.

## Accounting

`current_live_bytes` sums requested bytes, including the allocator's one-byte
minimum for a zero-size allocation. `internal_fragmentation_bytes` sums each
live block's usable capacity minus its requested bytes. `current_heap_bytes`
sums chunk and page-rounded direct mapping lengths. The heap checker compares
all three against a fresh walk. This is mapped virtual memory, not RSS.
