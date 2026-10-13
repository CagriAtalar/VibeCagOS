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
} __attribute__((aligned(HEAP_ALIGN)));   /* header size must keep data 8-aligned */

#define HDR_SIZE  (sizeof(struct heap_block))

/* i386: {size,free,next} is 12 bytes without the pad above, which made every
 * returned pointer 4 mod 8. Never let that regress silently. */
_Static_assert(sizeof(struct heap_block) % HEAP_ALIGN == 0,
               "heap header must be a multiple of the heap alignment");

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
static size_t             heap_frames = 0;   /* PMM frames currently owned by the heap */

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
    heap_frames += pages;

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
    s->heap_frames     = heap_frames;

    /* Count blocks */
    size_t count = 0;
    for (struct heap_block *b = heap_head; b; b = b->next)
        count++;
    s->num_blocks = count;
}

/*
 * Walk the whole block list and check the allocator's invariants. Returns 0
 * when the heap is consistent, otherwise a distinct positive code per
 * violated invariant (so a failing test says *what* broke).
 */
int kmalloc_validate(void) {
    size_t used = 0, total = 0;
    for (struct heap_block *b = heap_head; b; b = b->next) {
        if (((uintptr_t)b & (HEAP_ALIGN - 1u)) != 0) return 1;      /* misaligned header */
        if (b->size == 0 || (b->size & (HEAP_ALIGN - 1u)) != 0 ) {
            /* region tails are page-multiples minus HDR_SIZE; HDR_SIZE is a
             * multiple of HEAP_ALIGN, so every size is too */
            return 2;
        }
        if (b->next) {
            /* next must lie after this block's data (no overlap) */
            if ((uintptr_t)b->next < (uintptr_t)BLK_DATA(b) + b->size) return 3;
            /* fully coalesced: two adjacent-in-memory free blocks never coexist */
            if (b->free && b->next->free && blk_contiguous(b)) return 4;
        }
#ifdef KMALLOC_DEBUG
        if (!b->free && b->canary != HEAP_CANARY) return 5;
#endif
        total += b->size + HDR_SIZE;
        if (!b->free) used += b->size;
    }
    if (used != heap_used) return 6;      /* allocated-bytes counter drifted */
    /* Every byte of every region is either a header or block data. */
    if (total != heap_frames * (size_t)PAGE_SIZE) return 7;
    return 0;
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
