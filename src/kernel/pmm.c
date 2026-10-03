/*
 * VibeCagOS — Physical Memory Manager (PMM) Implementation
 *
 * Page frame allocator with bitmap tracking.
 */

#include "pmm.h"
#include "kernel.h"
#include "klog.h"

static uint8_t *bitmap = NULL;
static paddr_t  base_addr = 0;
static paddr_t  limit_addr = 0;
static size_t   total_frames_count = 0;
static size_t   used_frames_count = 0;

/* Global for legacy compatibility with procfs and diagnostics */
paddr_t next_paddr = 0;

static inline void bitmap_set(size_t frame) {
    bitmap[frame / 8] |= (uint8_t)(1u << (frame % 8));
}

static inline void bitmap_clear(size_t frame) {
    bitmap[frame / 8] &= (uint8_t)~(1u << (frame % 8));
}

static inline bool bitmap_test(size_t frame) {
    return (bitmap[frame / 8] & (1u << (frame % 8))) != 0;
}

void pmm_init(paddr_t mem_start, paddr_t mem_end) {
    base_addr = (mem_start + PMM_FRAME_SIZE - 1) & ~(PMM_FRAME_SIZE - 1);
    limit_addr = mem_end & ~(PMM_FRAME_SIZE - 1);

    if (limit_addr <= base_addr) {
        PANIC("PMM: invalid physical memory range 0x%x - 0x%x", mem_start, mem_end);
    }

    total_frames_count = (limit_addr - base_addr) / PMM_FRAME_SIZE;

    /* Bitmap resides at the start of managed physical memory */
    bitmap = (uint8_t *)base_addr;
    size_t bitmap_bytes = (total_frames_count + 7) / 8;
    size_t bitmap_frames = (bitmap_bytes + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;

    /* Initialize all frames as free */
    memset(bitmap, 0, bitmap_bytes);

    /* Mark the frames occupied by the bitmap itself as used */
    for (size_t i = 0; i < bitmap_frames; i++) {
        bitmap_set(i);
    }

    used_frames_count = bitmap_frames;
    next_paddr = base_addr + (bitmap_frames * PMM_FRAME_SIZE);

    KINFO("PMM", "Initialized %u frames (%u KB total, %u frames reserved for bitmap)",
          total_frames_count, (total_frames_count * 4), bitmap_frames);
}

paddr_t pmm_alloc_frame(void) {
    return pmm_alloc_frames(1);
}

paddr_t pmm_alloc_frames(size_t count) {
    if (count == 0) return 0;
    if (used_frames_count + count > total_frames_count) return 0;

    size_t consecutive = 0;
    size_t start_frame = 0;

    for (size_t i = 0; i < total_frames_count; i++) {
        if (!bitmap_test(i)) {
            if (consecutive == 0) start_frame = i;
            consecutive++;
            if (consecutive == count) {
                /* Mark frames allocated */
                for (size_t j = 0; j < count; j++) {
                    bitmap_set(start_frame + j);
                }
                used_frames_count += count;

                paddr_t paddr = base_addr + (start_frame * PMM_FRAME_SIZE);
                memset((void *)paddr, 0, count * PMM_FRAME_SIZE);

                /* Update legacy pointer for stats */
                paddr_t highest = paddr + (count * PMM_FRAME_SIZE);
                if (highest > next_paddr) {
                    next_paddr = highest;
                }

                return paddr;
            }
        } else {
            consecutive = 0;
        }
    }

    return 0;
}

void pmm_free_frame(paddr_t paddr) {
    pmm_free_frames(paddr, 1);
}

void pmm_free_frames(paddr_t paddr, size_t count) {
    if (paddr < base_addr || paddr >= limit_addr || count == 0) return;

    size_t start_frame = (paddr - base_addr) / PMM_FRAME_SIZE;
    for (size_t i = 0; i < count; i++) {
        size_t f = start_frame + i;
        if (f < total_frames_count && bitmap_test(f)) {
            bitmap_clear(f);
            if (used_frames_count > 0) {
                used_frames_count--;
            }
        }
    }
}

size_t pmm_get_total_frames(void) { return total_frames_count; }
size_t pmm_get_free_frames(void)  { return total_frames_count - used_frames_count; }
size_t pmm_get_used_frames(void)  { return used_frames_count; }

size_t pmm_get_total_bytes(void)  { return total_frames_count * PMM_FRAME_SIZE; }
size_t pmm_get_used_bytes(void)   { return used_frames_count * PMM_FRAME_SIZE; }
size_t pmm_get_free_bytes(void)   { return (total_frames_count - used_frames_count) * PMM_FRAME_SIZE; }

paddr_t alloc_pages(uint32_t n) {
    paddr_t p = pmm_alloc_frames(n);
    if (!p) {
        PANIC("Out of physical memory (%u pages requested)", n);
    }
    return p;
}

void free_pages(paddr_t paddr, uint32_t n) {
    pmm_free_frames(paddr, n);
}
