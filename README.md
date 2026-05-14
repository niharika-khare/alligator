# 🐊 alligator

A memory alligator (oops! I mean allocator) written in C, built from scratch as a deep-dive into how allocators actually work. Implements `mm_alloc`, `mm_free`, and `mm_realloc` directly on top of `mmap`/`munmap` — no libc heap, no `sbrk`.

---

## How it works

Allocations are routed to one of three slab tiers based on request size:

```
≤ 1 × PAGE_SIZE              → small slab
≤ 1024 × PAGE_SIZE           → medium slab
≤ 1024 × 1024 × PAGE_SIZE    → large slab
anything bigger              → dedicated mmap per request
```

Slabs are allocated lazily — the first request per tier triggers the `mmap`, subsequent requests carve from the existing slab. Each tier has its own circular doubly-linked free list. When a slab is fully coalesced back to its original size it's returned to the OS via `munmap`.

### Block layout

Every block carries a 16-byte header. Free blocks additionally store `next`/`prev` pointers for free list linkage, making the free header 32 bytes.

```
┌─────────────────────────────────┐
│ is_free : 1                     │
│ is_last : 1                     │  Word 1 (8 bytes)
│ size    : 48                    │
├─────────────────────────────────┤
│ pblk_size : 48                  │  Word 2 (8 bytes)
│ magic_id  : 16                  │
├─────────────────────────────────┤
│ next *  (free blocks only)      │
│ prev *  (free blocks only)      │
├─────────────────────────────────┤
│ payload                         │
└─────────────────────────────────┘
```

`pblk_size` is the payload size of the immediately preceding block. This is the boundary tag — it lets `mm_free` locate the previous block header in O(1) without storing a footer at the end of every block.

`magic_id` is set to `0xA2F5` at allocation time and verified on every free. Catches most wild pointer and use-after-free bugs.

### Allocation

1. First-fit search over the free list for the target tier. 
2. When a suitable block is found it's split — the **rear** portion is returned to the user.
3. The front stays on the free list with an updated size. 
4. If no block fits, a new slab is `mmap`'d and prepended to the list.

### Free

1. Recover header from `ptr - ALOC_H_SIZE`
2. Validate magic + double-free check. Failure → `abort()`
3. `mm_free(NULL)` is a no-op
4. If block covers a full slab size, `munmap` it directly
5. Coalesce with the next block if it's free, then with the previous block if it's free
6. If the merged block equals the slab size, `munmap` the whole slab
7. Otherwise insert at the front of the free list

### Realloc

1. `mm_realloc(ptr, size)` always allocates a new block, copies `min(old_size, new_size)` bytes, and frees the original. 
2. So both grow and shrink are handled correctly — shrink truncates, grow copies what exists and leaves the rest uninitialised (consistent with standard `realloc` behaviour). 
3. `mm_realloc(NULL, size)` behaves like `mm_alloc(size)`.

---

## API

```c
void * mm_alloc   (size_t size);
void * mm_realloc (void * mem, size_t size);
void   mm_free    (void * mem);
```

---

## Building

```bash

make help               # To see make targets
make clean && make      # To clean, build and run the allocator via main.c driver

```

No external dependencies — only POSIX (`mmap`, `munmap`, `sysconf`, `write`, `abort`, `memcpy`).

> Developed and tested on Apple Silicon (aarch64/macOS). If `MAP_ANONYMOUS` isn't available on your platform, replace with `MAP_ANON`.

---

## Design decisions

**Three slab tiers.**
A single slab size creates a bad fit between request size and slab waste. Three tiers let small frequent allocations share a compact one-page slab while larger requests get proportionally sized slabs, minimizing the internal fragmentation and over-allocation.

**Split from the rear.**
When splitting a free block, the allocated region is carved from the back. The free block header at the front doesn't move — it stays at the same address with the same free list position. The only updates needed are the front block's `size` and the `pblk_size` of the block immediately after the newly allocated region. Splitting from the front would require relinking the free list every time.

**Boundary tags without footers.**
Classic boundary-tag allocators store a copy of the header at the end of each block so the previous block can be found in O(1) during coalescing. Storing `pblk_size` in the current block's header achieves the same O(1) backward lookup without writing to the tail of every block — halving the metadata written per allocation.

**Circular doubly-linked free list.**
Coalescing needs to remove an arbitrary node from the middle of the free list in O(1) — specifically the neighbour being absorbed. A doubly-linked list makes this trivial. Circular layout simplifies the traversal loop in `_find_f_blk` — no null checks mid-iteration.

**`mmap` over `sbrk`.**
Each slab is an independent memory range. When a slab is fully reclaimed, `munmap` returns exactly those pages to the OS immediately. With `sbrk` (which is risker and hence on path on deprecation in macOS) we can only shrink the heap from the top — any free slab buried under live allocations stays resident regardless.

**`PAGE_SIZE` cached via a static local.**
`sysconf(_SC_PAGESIZE)` is a syscall. Since every slab size computation uses `PAGE_SIZE`, calling it on every macro expansion would mean 6+ syscalls per `mm_alloc`. A `static size_t` inside `_get_page_size()` ensures the syscall happens exactly once.

**`abort()` on corruption, not silent recovery.**
If `magic_id` doesn't match or `is_free` is already set, the allocator calls `abort()`. There's no safe way to recover from a corrupted header — attempting to continue would likely corrupt more state. Failing loudly makes bugs easier to find.

**Bitfields for the header.**
The header packs cleanly into two 64-bit words: `is_free(1) + is_last(1) + size(48)` in word 1, `pblk_size(48) + magic_id(16)` in word 2. The tradeoff is thread safety — bitfield writes are not atomic, so the allocator is intentionally single-threaded.

---

## Known limitations

- **Not thread-safe** — bitfield read-modify-write, free list pointer updates, and slab initialisation are all non-atomic sequences that require a lock.
- **First-fit only** — no best-fit or next-fit;
- **`mm_realloc` always copies** — no in-place grow even when the next block is free and the two together would fit
- **`_is_valid_blk` doesn't check alignment or range** — a corrupted pointer that passes the magic check by coincidence won't be caught.

---

## Future enhancements

**`_is_valid_blk` alignment and range checks.**
Before reading any header fields, verify that the pointer is naturally aligned to `_Header` and falls within a known `mmap`'d region. Catching wild pointer frees before reading garbage header fields would make the allocator much more robust. To accomplish this, "slab registry" needs to be maintained:
a compact array of `{base, size}` pairs for every live slab. `_is_valid_blk` can then do an O(n_slabs) range check before trusting the magic. With a small number of slabs this is cheap and can catches nearly all invalid frees.
[With only magic number check, the buffer overflow scenarios, even when caught, cannot be checked for correct error type]

**In-place realloc.**
Before allocating a new block, check if the next block is free and `current_size + next_size >= requested_size`. If so, absorb the next block and return the same pointer — zero copy, zero `mmap`. It's implementation would be different than the current _find_f_blk() as it would need forward split rather than the rear split.

**In-place shrink.**
For `mm_realloc(ptr, size)` where `size < current_size`, split the current block in place and return the tail to the free list rather than copying. Currently shrink always copies needlessly.

**Thread safety.**
Introduce thread saftey to allow multi-threaded operations on the heap. This would need changes in the header structure as bitfield are not thread-safe.