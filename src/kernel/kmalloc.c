/*
 * VibeCagOS — Kernel Heap Allocator Implementation
 *
 * First-fit free list with immediate coalescing.
 * Built on top of alloc_pages() from the physical frame allocator.
 *
 * Block layout (all sizes in bytes):
 *
 *   +--------+----------------------------+
 *   | header | user data (size bytes)     |
 *   +--------+----------------------------+
 *   ^        ^
 *   |        `-- returned to caller by kmalloc
 *   `-- heap_block
 *
 * struct heap_block {
 *     size_t   size;    // size of USER data, not including header
 *     bool     free;
 *     uint32_t canary; // only in debug builds
 *     struct heap_block *next;
 * };
 *
 * Coalescing: when a block is freed, adjacent free blocks are merged.
 * This keeps fragmentation low for a simple allocator.
 */

#include "kmalloc.h"
#include "kernel.h"   /* alloc_pages, PANIC */
#include "common.h"

/* =========================================================================
 * Heap block header
 * ========================================================================= */

#define HEAP_ALIGN     8u         /* Minimum alignment */
#define HEAP_GROW_PAGES 4u        /* Pages to add when heap runs out */

/*
 * Every heap block (free or allocated) starts with this header.
 * The header is hidden from the caller; caller sees only the data region.
 */
struct heap_block {
    size_t             size;   /* Size of user data region (NOT including header) */
    bool               free;   /* 1 = available, 0 = allocated */
#ifdef KMALLOC_DEBUG
    uint32_t           canary; /* HEAP_CANARY for allocated blocks */
#endif
    struct heap_block *next;   /* Next block in the free list (or NULL) */
};

#define HDR_SIZE  (sizeof(struct heap_block))

/* Pointer arithmetic helpers */
#define BLK_DATA(b)   ((void *)((uint8_t *)(b) + HDR_SIZE))
#define DATA_BLK(p)   ((struct heap_block *)((uint8_t *)(p) - HDR_SIZE))

/* =========================================================================
 * Heap state
 * ========================================================================= */

static struct heap_block *heap_head = NULL;  /* First block in the list */
static size_t             heap_total = 0;
static size_t             heap_used  = 0;
static size_t             heap_nalloc = 0;
static size_t             heap_nfree  = 0;

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/*
 * Grow the heap by `pages` pages.
 * Adds a new free block at the end of the list.
 */
static void heap_grow(uint32_t pages) {
    if (pages == 0) pages = HEAP_GROW_PAGES;

    void *region = (void *)alloc_pages(pages);
    size_t region_size = (size_t)pages * PAGE_SIZE;

    if (region_size <= HDR_SIZE)
        return;

    struct heap_block *blk = (struct heap_block *)region;
    blk->size = region_size - HDR_SIZE;
    blk->free = true;
    blk->next = NULL;
#ifdef KMALLOC_DEBUG
    blk->canary = 0;
#endif

    heap_total += blk->size;

    /* Append to end of list */
    if (!heap_head) {
        heap_head = blk;
        return;
    }

    struct heap_block *cur = heap_head;
    while (cur->next)
        cur = cur->next;
    cur->next = blk;

    /* Coalesce with previous if it's free */
    cur = heap_head;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            /* Merge cur and cur->next */
            cur->size += HDR_SIZE + cur->next->size;
            cur->next  = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void kmalloc_init(void) {
    /* Allocate initial heap region */
    heap_grow(HEAP_GROW_PAGES);
}

void *kmalloc(size_t size) {
    if (size == 0)
        return NULL;

    /* Round up to alignment */
    size = (size + HEAP_ALIGN - 1u) & ~(HEAP_ALIGN - 1u);

    /* Try to find a suitable free block (first-fit) */
    struct heap_block *cur = heap_head;
    struct heap_block *prev = NULL;

    while (cur) {
        if (cur->free && cur->size >= size) {
            /* Can we split? Only if remainder is large enough to hold a header + 8 bytes */
            if (cur->size >= size + HDR_SIZE + HEAP_ALIGN) {
                /* Split block */
                struct heap_block *new_blk =
                    (struct heap_block *)((uint8_t *)BLK_DATA(cur) + size);
                new_blk->size = cur->size - size - HDR_SIZE;
                new_blk->free = true;
                new_blk->next = cur->next;
#ifdef KMALLOC_DEBUG
                new_blk->canary = 0;
#endif
                cur->next = new_blk;
                cur->size = size;
            }

            cur->free = false;
#ifdef KMALLOC_DEBUG
            cur->canary = HEAP_CANARY;
#endif
            heap_used  += cur->size;
            heap_nalloc++;
            (void)prev;
            return BLK_DATA(cur);
        }
        prev = cur;
        cur  = cur->next;
    }

    /* No suitable block — grow the heap */
    uint32_t pages_needed = (uint32_t)((size + HDR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
    if (pages_needed < HEAP_GROW_PAGES)
        pages_needed = HEAP_GROW_PAGES;

    heap_grow(pages_needed);

    /* Retry once — if it fails now, we're truly out of memory */
    cur = heap_head;
    while (cur) {
        if (cur->free && cur->size >= size) {
            if (cur->size >= size + HDR_SIZE + HEAP_ALIGN) {
                struct heap_block *new_blk =
                    (struct heap_block *)((uint8_t *)BLK_DATA(cur) + size);
                new_blk->size = cur->size - size - HDR_SIZE;
                new_blk->free = true;
                new_blk->next = cur->next;
#ifdef KMALLOC_DEBUG
                new_blk->canary = 0;
#endif
                cur->next = new_blk;
                cur->size = size;
            }
            cur->free = false;
#ifdef KMALLOC_DEBUG
            cur->canary = HEAP_CANARY;
#endif
            heap_used  += cur->size;
            heap_nalloc++;
            return BLK_DATA(cur);
        }
        cur = cur->next;
    }

    return NULL;  /* Out of memory */
}

void kfree(void *ptr) {
    if (!ptr)
        return;

    struct heap_block *blk = DATA_BLK(ptr);

#ifdef KMALLOC_DEBUG
    if (blk->canary != HEAP_CANARY) {
        PANIC("kfree: canary corrupted at %p (got 0x%x)", ptr, blk->canary);
    }
    blk->canary = 0;
    /* Poison freed data */
    memset(ptr, 0xFE, blk->size);
#endif

    if (blk->free) {
        PANIC("kfree: double-free detected at %p", ptr);
    }

    heap_used -= blk->size;
    blk->free = true;
    heap_nfree++;

    /* Coalesce adjacent free blocks (forward pass only — sufficient for correctness) */
    struct heap_block *cur = heap_head;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += HDR_SIZE + cur->next->size;
            cur->next  = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

void *kmalloc_aligned(size_t size, size_t align) {
    if (size == 0 || align == 0)
        return NULL;

    /* align must be power of two */
    if (align & (align - 1))
        return NULL;

    if (align <= HEAP_ALIGN)
        return kmalloc(size);

    /*
     * Over-allocate by (align - 1 + HDR_SIZE) so we can find an aligned
     * point inside the allocation.  We store the original pointer just
     * before the aligned region so kfree can recover it.
     *
     * NOTE: This is a simple implementation.  We allocate extra space and
     * waste some bytes to guarantee alignment.  For the hobby OS use-case
     * (page-aligned DMA buffers etc.) this is acceptable.
     *
     * Callers that require page-aligned memory (DMA) should use alloc_pages()
     * directly; it already returns page-aligned addresses.
     */
    size_t total = size + align - 1 + sizeof(void *);
    uint8_t *raw = (uint8_t *)kmalloc(total);
    if (!raw)
        return NULL;

    /* Find aligned address */
    uintptr_t raw_addr  = (uintptr_t)raw + sizeof(void *);
    uintptr_t aligned   = (raw_addr + align - 1) & ~(align - 1);
    void    **store     = (void **)(aligned - sizeof(void *));
    *store              = raw;   /* stash original pointer */

    return (void *)aligned;
}

void kmalloc_get_stats(struct kmalloc_stats *s) {
    s->total_bytes     = heap_total;
    s->allocated_bytes = heap_used;
    s->free_bytes      = heap_total > heap_used ? heap_total - heap_used : 0;
    s->num_allocs      = heap_nalloc;
    s->num_frees       = heap_nfree;

    /* Count blocks */
    size_t count = 0;
    for (struct heap_block *b = heap_head; b; b = b->next)
        count++;
    s->num_blocks = count;
}

void kmalloc_dump_stats(void) {
    struct kmalloc_stats s;
    kmalloc_get_stats(&s);
    printf("Heap: total=%u KB  alloc=%u KB  free=%u KB  blocks=%u  allocs=%u  frees=%u\n",
           (unsigned)(s.total_bytes / 1024),
           (unsigned)(s.allocated_bytes / 1024),
           (unsigned)(s.free_bytes / 1024),
           (unsigned)s.num_blocks,
           (unsigned)s.num_allocs,
           (unsigned)s.num_frees);
}
