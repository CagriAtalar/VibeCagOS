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
 * Thread-safety: NOT re-entrant.  All callers must hold no spinlock
 * that might be acquired again inside alloc_pages(), or must call from
 * a single-threaded context.  For SMP, wrap with a spinlock.
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
 * kmalloc_aligned — allocate memory aligned to `align` bytes.
 * align must be a power of two and >= 8.
 * Returns NULL on failure.
 */
void *kmalloc_aligned(size_t size, size_t align);

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
};

void kmalloc_get_stats(struct kmalloc_stats *s);

/* Print heap statistics via printf */
void kmalloc_dump_stats(void);
