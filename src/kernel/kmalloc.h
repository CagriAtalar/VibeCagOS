#pragma once
#include "common.h"

/*
 * VibeCagOS — Kernel Heap Allocator
 *
 * A simple first-fit free-list heap built on top of the physical frame
 * allocator (alloc_pages).  Unlike the bump allocator, kmalloc/kfree
 * support reuse of freed blocks.
 *
 * Layout of each heap block:
 *
 *   [ struct heap_block header ][ user data ... ]
 *
 * The heap starts at the first 4 KiB page after the kernel image.
 * Additional pages are requested from alloc_pages() as needed.
 *
 * Concurrency contract (single CPU, today): kmalloc/kfree are NOT re-entrant
 * and must never be called from interrupt context. Every current caller is in
 * process/syscall context (pipe, exec, VFS). The kernel is not preemptible, so
 * a context switch can never land in the middle of an allocator operation. For
 * SMP, wrap with a spinlock.
 *
 * Lifecycle (important for leak accounting):
 *   - The heap grows on demand by whole PMM frames (never fewer than 4).
 *   - The heap NEVER returns frames to the PMM. A freed block is reused by later
 *     kmalloc calls, so heap_frames is a high-water mark, not a leak.
 *   - Therefore the leak invariants are:
 *       FramesFree + heap_frames          constant across a lifecycle
 *                                         (no process-owned frame leaked)
 *       allocated_bytes                   constant across a lifecycle
 *                                         (no heap block leaked)
 *     and heap_frames itself is bounded by the largest simultaneously live
 *     allocation set (e.g. one executable image), not by the number of
 *     processes created.
 *   - kmalloc_validate() walks every block and checks the structural
 *     invariants (alignment, no overlap, full coalescing, counters, canaries).
 *
 * Design constraints (no over-engineering rule):
 *   - First-fit free list
 *   - Minimum allocation: 8 bytes
 *   - Alignment: 8-byte (good for 32-bit kernel)
 *   - Coalesce on free
 *   - Canary in debug builds
 */

/* Debug mode: define KMALLOC_DEBUG before including this header */
#ifdef KMALLOC_DEBUG
#define HEAP_CANARY  0xDEADBEEFu
#endif

void  kmalloc_init(void);

/*
 * kmalloc — allocate at least `size` bytes of kernel memory.
 * Returns NULL on failure.
 * The returned pointer is 8-byte aligned.
 */
void *kmalloc(size_t size);

/*
 * kfree — release memory previously returned by kmalloc.
 * Passing NULL is safe.
 */
void  kfree(void *ptr);

/*
 * (kmalloc_aligned was removed: kfree() could not undo its alignment padding.
 *  Page-aligned/DMA memory comes from pmm_alloc_frames() directly.)
 */

/*
 * kmalloc_stats — fills in statistics about the heap.
 */
struct kmalloc_stats {
    size_t total_bytes;      /* Total heap capacity */
    size_t allocated_bytes;  /* Currently allocated (user data only) */
    size_t free_bytes;       /* Currently free */
    size_t num_allocs;       /* Total successful kmalloc calls */
    size_t num_frees;        /* Total kfree calls */
    size_t num_blocks;       /* Current block count (free + used) */
    size_t heap_frames;      /* PMM frames currently owned by the heap */
};

void kmalloc_get_stats(struct kmalloc_stats *s);

/* Check all heap invariants. 0 = consistent, else a distinct nonzero code
 * (1 misaligned, 2 bad size, 3 overlap, 4 uncoalesced, 5 canary, 6 used
 * counter, 7 capacity counter). */
int kmalloc_validate(void);

/* Print heap statistics via printf */
void kmalloc_dump_stats(void);
