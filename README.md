# 🐊 alligator

A memory alligator (oops! I mean allocator) written in C, built from scratch as a deep-dive into how allocators actually work. Implements `malloc`, `free`, `realloc` and `calloc` directly on top of `mmap`/`munmap`, without libc heap and `sbrk`.

---

## How it works

Allocations are routed to one of three slab tiers based on request size (request + 16-byte header must fit in the slab):

```
≤ 16 × PAGE_SIZE             → small slab     (64 KB with 4 KB pages)
≤ 1024 × PAGE_SIZE           → medium slab    (4 MB)
≤ 32 × 1024 × PAGE_SIZE      → large slab     (128 MB)
anything bigger              → dedicated mmap per request
```

Slabs are allocated lazily. The first request per tier triggers the `mmap`, subsequent requests carve from the existing slab. Each tier has its own circular doubly-linked free list. When a slab is fully coalesced back to its original size it's returned to the OS via `munmap`, except the first empty slab of each tier, which is kept as a *reserved* slab so the next allocation doesn't have to `mmap` again.

Every slab is recorded in a **slab registry** which is a fixed array of `{base, size}` pairs (up to 16,384 live slabs). `free` and `realloc` check that a pointer falls inside a registered slab before reading its header.

### Block layout

Every block carries a 16-byte header. Free blocks additionally store `next`/`prev` pointers for free list linkage, making the free header 32 bytes.

```
┌─────────────────────────────────┐
│ is_free     : 1                 │
│ is_last     : 1                 │  Word 1 (8 bytes)
│ size        : 48                │
│ slab_id     : 14                │
├─────────────────────────────────┤
│ pblk_size   : 48                │
│ magic_id    : 15                │  Word 2 (8 bytes)
│ is_reserved : 1                 │
├─────────────────────────────────┤
│ next *  (free blocks only)      │
│ prev *  (free blocks only)      │
├─────────────────────────────────┤
│ payload                         │
└─────────────────────────────────┘
```

`size` is the payload size. For an allocated block that is the block's total size minus the 16-byte header; for a free block it is the total size minus the 32-byte free header.

`pblk_size` is the **total** size ((header + payload) or (free_header + available space)) of the immediately preceding block, and `0` for the first block in a slab. This is the boundary tag via which `free` finds the previous header at `blk - pblk_size` in O(1), without storing a footer at the end of every block. By not storing a footer, the design is simplified.

`slab_id` is the slab's index in the slab registry. It is set on every block carved from a slab and used to clear the registry entry when the slab is unmapped.

`magic_id` is set to `0x72F5` at allocation time and verified on every free. Catches wild pointers that land inside a slab but not on a block header.

`is_reserved` marks the one empty slab per tier that is kept instead of unmapped.

Every returned pointer is aligned to `ALIGNMENT` (`max(alignof(max_align_t), 16)`): request sizes are rounded up to it, slabs start on a page boundary and both headers are multiples of 16. That is 16 bytes on Linux (aarch64 and x86-64) and 8 bytes on Apple Silicon macOS.

### Allocation

1. Reject requests larger than `MAX_ALLOWED_SIZE` (2^48 − 32 bytes, the most the 48-bit `size` field can hold) with `NULL`.
2. Round the request up to `ALIGNMENT` and pick the tier (or a dedicated `mmap` if it's bigger than the large slab).
3. First-fit search over the free list for the target tier.
4. When a suitable block is found it's split and the **rear** portion is returned to the user.
5. The front stays on the free list with an updated size.
6. If splitting would leave nothing beyond a bare free header, the whole block is handed out instead.
7. If no block fits, a new slab is `mmap`'d and prepended to the list.
8. Allocating from the reserved slab clears its reservation.

`malloc(0)` returns a valid pointer to the smallest block.

### Free

1. `free(NULL)` is a no-op
2. Check the pointer is aligned to `ALIGNMENT` and falls inside a registered slab, then recover the header from `ptr - ALOC_H_SIZE`
3. Validate magic + double-free check. Failure → `abort()`
4. If the block covers a full slab (or is a dedicated `mmap`), `munmap` it directly
5. Coalesce with the next block if it's free, then with the previous block if it's free (the absorbed block's header is marked free, so a second `free` of the same pointer is still caught)
6. If the merged block equals the slab size: keep it as the tier's reserved slab if there isn't one yet, otherwise `munmap` the whole slab
7. Otherwise insert at the front of the free list

### Realloc

1. `realloc(ptr, size)` always allocates a new block, copies `min(old_size, new_size)` bytes, and frees the original.
2. So both grow and shrink are handled correctly — shrink truncates, grow copies what exists and leaves the rest uninitialised (consistent with standard `realloc` behaviour).
3. `realloc(NULL, size)` behaves like `malloc(size)`.
4. `realloc(ptr, 0)` frees `ptr` and returns `NULL`.
5. If the new block can't be allocated (e.g. the size is over the limit), `realloc` returns `NULL` and the original block is left untouched.

### Calloc

`calloc(n, size)` returns `NULL` if `n * size` would exceed `MAX_ALLOWED_SIZE` (checked before multiplying, so it can't overflow), otherwise it's `malloc(n * size)` followed by zeroing the block.

---

## API

```c
void * malloc  (size_t size);
void * realloc (void * mem, size_t size);
void * calloc  (size_t num_ele, size_t size);
void   free    (void * mem);
```

---

## Building

``main.c`` is the driver file for testing this library. Any new use cases can added to it when experimenting with any new features. It runs 19 test groups (tiers, coalescing, reserved slabs, slab registry, `calloc`, size limits, every `abort()` path and an interleaved stress test), prints a pass/fail summary and exits non-zero if any check fails.

```bash

make help               # To see make targets
make clean && make      # To clean, build and run the allocator via main.c driver
make lib                # To build the shared library bin/liballigator.so

```

To use alligator in place of the system allocator for any program, build the shared library and preload it (Linux):

```bash

make lib
LD_PRELOAD=$PWD/bin/liballigator.so <program>

```

Everything is built with `-fno-builtin`, so the compiler doesn't treat `malloc`/`free` calls specially (for example, removing allocations it thinks are unused).

No external dependencies — only POSIX (`mmap`, `munmap`, `sysconf`, `write`, `abort`, `memcpy`, `memset`, `strlen`).

> Developed on Apple Silicon (aarch64/macOS). Benchmarked with mimalloc-bench on Ubuntu Linux — an aarch64 VM (UTM on Apple M5) and x86-64 Linux (cloud VM). If `MAP_ANONYMOUS` isn't available on your platform, replace with `MAP_ANON`.

---

## Validation

- **Test driver (`main.c`):** 19 test groups covering every tier, coalescing, slab reclamation and the reserved slab, the slab registry, `calloc`, size limits, every `abort()` path, and a 200,000-operation interleaved `malloc`/`calloc`/`realloc`/`free` stress test with block contents verified at both ends.
- **Heap consistency:** during development, a checker that wraps the allocator and, after every call, walks every slab and free list (magic, `pblk_size`, `is_last`, `slab_id`, adjacent free blocks, list links, tier membership, reserved-slab rules, registry, alignment), run over real programs and a randomised stress test.

---

## Design decisions and Trade-offs

**Three slab tiers.**
A single slab size creates a bad fit between request size and slab waste. Three tiers let small frequent allocations share a compact slab while larger requests get proportionally sized slabs, minimizing the internal fragmentation and over-allocation.

**Slab sizes: 16 pages, 1024 pages, 32K pages.**
The small slab was originally one page. Growing it to 16 pages means about 16× fewer slabs, so fewer `mmap` calls and a much shorter registry scan on every `free`. The large slab was originally 1M pages (4 GiB); a slab that big rarely empties, so its pages were never returned to the OS. At 128 MB, slabs empty and get unmapped.

**Split from the rear.**
When splitting a free block, the allocated region is carved from the back. The free block header at the front doesn't move — it stays at the same address with the same free list position. The only updates needed are the front block's `size` and the `pblk_size` of the block immediately after the newly allocated region. Splitting from the front would require relinking the free list every time.

**Boundary tags without footers.**
Classic boundary-tag allocators store a copy of the header at the end of each block so the previous block can be found in O(1) during coalescing. Storing `pblk_size` in the current block's header achieves the same O(1) backward lookup without writing to the tail of every block — halving the metadata written per allocation and simplifies the colescing logic.

**Circular doubly-linked free list.**
Coalescing needs to remove an arbitrary node from the middle of the free list in O(1) — specifically the neighbour being absorbed. A doubly-linked list makes this trivial. Circular layout simplifies the traversal loop in `_find_f_blk` — no null checks mid-iteration.

**`mmap` over `sbrk`.**
Each slab is an independent memory range. When a slab is fully reclaimed, `munmap` returns exactly those pages to the OS immediately. With `sbrk` (which is risker and hence on path on deprecation in macOS) we can only shrink the heap from the top — any free slab buried under live allocations stays resident regardless.

**One reserved empty slab per tier.**
Unmapping every slab the moment it empties caused constant `mmap`/`munmap` churn: a program that fills and empties one slab repeatedly paid for a fresh `mmap` and page faults every time. Keeping the first empty slab per tier (and unmapping any others) removes most of that churn, while still returning memory beyond that one slab.

**Slab registry.**
Before reading any header, `free` and `realloc` check that the pointer lies inside a live slab. A pointer that doesn't belong to alligator (stack, static data, another allocator) is rejected without touching memory that may not be a header. `slab_id` in the header points back to the registry entry, so unmapping a slab doesn't need a search.

**`PAGE_SIZE` cached via a static local.**
`sysconf(_SC_PAGESIZE)` is a library call, and every slab size computation uses `PAGE_SIZE`. A `static size_t` inside `_get_page_size()` ensures the lookup happens exactly once.

**`abort()` on corruption, not silent recovery.**
If the pointer is misaligned, outside every slab, `magic_id` doesn't match, or `is_free` is already set, the allocator prints the reason and calls `abort()`. There's no safe way to recover from a corrupted header — attempting to continue would likely corrupt more state. Failing loudly makes bugs easier to find.

**Bitfields for the header.**
The header packs cleanly into two 64-bit words: `is_free(1) + is_last(1) + size(48) + slab_id(14)` in word 1, `pblk_size(48) + magic_id(15) + is_reserved(1)` in word 2. `magic_id` is 15 bits so `is_reserved` fits without growing the header past 16 bytes, which keeps both headers a multiple of the alignment. The tradeoff is thread safety — bitfield writes are not atomic, so current version (v1.1.0) of the allocator is intentionally single-threaded (for now).

---

## Benchmarking

Alligator was benchmarked via mimalloc-bench suite. 
Details TBA.

---

## Known limitations

- **Not thread-safe** — bitfield read-modify-write, free list pointer updates, and slab initialisation are all non-atomic sequences that require a lock. Multi-threaded programs crash.
- **First-fit only** — no best-fit or next-fit; each tier has a single free list holding blocks of all sizes, so `malloc` cost grows with the number of free blocks.
- **Linear registry lookup** — every `free` and `realloc` scans the slab registry, so cost grows with the number of live slabs.
- **`realloc` always copies** — no in-place grow even when the next block is free and the two together would fit
- **No aligned allocation or size query** — `posix_memalign`, `aligned_alloc`, `memalign` and `malloc_usable_size` are not implemented. Programs that call them get glibc's versions, and then crash or abort when the pointer reaches alligator (or when glibc reads an alligator pointer).
- **Dedicated `mmap` blocks are unmapped 16 bytes short** — when that crosses a page boundary, one page stays mapped.
- **Slab sizes scale with page size** — on 16 KB or 64 KB page kernels every slab is 4× or 16× bigger.

---

## Future enhancements

**Faster pointer validation.**
The slab registry provides the range check and the alignment check is in place. Remaining: replace the linear registry scan — for example by aligning each slab to its size so the slab base is `ptr & ~(slab_size - 1)`.

**Size-segregated free lists.**
Split each tier's free list by block size so `malloc` finds a fitting block without walking the whole list.

**Aligned allocation and `malloc_usable_size`.**
Implement `posix_memalign`, `aligned_alloc`, `memalign` and `malloc_usable_size` so programs that use them (redis, rocksdb, build tools like `ar`) can run on alligator.

**In-place realloc.**
Before allocating a new block, check if the next block is free and `current_size + next_size >= requested_size`. If so, absorb the next block and return the same pointer — zero copy, zero `mmap`. It's implementation would be different than the current _find_f_blk() as it would need forward split rather than the rear split.

**In-place shrink.**
For `realloc(ptr, size)` where `size < current_size`, split the current block in place and return the tail to the free list rather than copying. Currently shrink always copies needlessly.

**Thread safety.**
Introduce thread saftey to allow multi-threaded operations on the heap. This would need changes in the header structure as bitfield are not thread-safe.

---

## License

MIT — see [LICENSE](LICENSE).