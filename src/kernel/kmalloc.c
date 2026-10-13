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
#include "kernel.h"   /* PANIC */
#include "pmm.h"      /* pmm_alloc_frames / pmm_free_frames */
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
 * Two list-adjacent blocks may be merged ONLY if they are also adjacent in
 * memory. heap_grow() appends regions obtained from the frame allocator, which
 * (being a bitmap) need not hand out contiguous frames; merging across a gap
 * would fuse in foreign pages (page tables, user frames...).
 */
static inline bool blk_contiguous(const struct heap_block *a) {
    return (const uint8_t *)BLK_DATA(a) + a->size == (const uint8_t *)a->next;
}

static void heap_coalesce(void) {
    struct heap_block *cur = heap_head;
    while (cur && cur->next) {
        if (cur->free && cur->next->free && blk_contiguous(cur)) {
            cur->size += HDR_SIZE + cur->next->size;
            cur->next  = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

/*
 * Grow the heap by `pages` pages.
 * Adds a new free block at the end of the list. Returns false (heap
 * unchanged) when the frame allocator cannot satisfy the request.
 */
static bool heap_grow(uint32_t pages) {
    if (pages == 0) pages = HEAP_GROW_PAGES;

    void *region = (void *)pmm_alloc_frames(pages);   /* 0 on failure */
    if (!region)
        return false;
    size_t region_size = (size_t)pages * PAGE_SIZE;

    if (region_size <= HDR_SIZE) {
        pmm_free_frames((paddr_t)region, pages);
        return false;
    }

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
        return true;
    }

    struct heap_block *cur = heap_head;
    while (cur->next)
        cur = cur->next;
    cur->next = blk;

    heap_coalesce();
    return true;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void kmalloc_init(void) {
    /* Allocate initial heap region */
    if (!heap_grow(HEAP_GROW_PAGES))
        PANIC("kmalloc_init: no physical memory for the initial heap");
}

void *kmalloc(size_t size) {
    if (size == 0)
        return NULL;

    /* Overflow-safe: reject anything that cannot be rounded/headered without
     * wrapping (a wrapped size would hand out a tiny block for a huge ask). */
    if (size > (size_t)-1 - HDR_SIZE - PAGE_SIZE - HEAP_ALIGN)
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

    if (!heap_grow(pages_needed))
        return NULL;   /* out of physical memory: the caller must cope */

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

    heap_coalesce();
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
