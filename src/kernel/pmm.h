/*
 * VibeCagOS — Physical Memory Manager (PMM)
 *
 * Page frame allocator using a bitmap to track physical 4 KiB frames.
 * Supports single frame and contiguous multi-frame allocations,
 * frame deallocation, and memory statistics.
 */

#pragma once
#include "common.h"

#define PMM_FRAME_SIZE 4096u
#define PMM_FRAME_SHIFT 12u

/* Initialize the physical memory allocator with physical address boundaries */
void     pmm_init(paddr_t mem_start, paddr_t mem_end);

/* Allocate a single 4KB physical frame (zeroed). Returns 0 on OOM. */
paddr_t  pmm_alloc_frame(void);

/* Allocate 'count' contiguous 4KB physical frames (zeroed). Returns 0 on OOM. */
paddr_t  pmm_alloc_frames(size_t count);

/* Free a previously allocated physical frame */
void     pmm_free_frame(paddr_t paddr);

/* Free 'count' contiguous physical frames */
void     pmm_free_frames(paddr_t paddr, size_t count);

/* Statistics */
size_t   pmm_get_total_frames(void);
size_t   pmm_get_free_frames(void);
size_t   pmm_get_used_frames(void);
size_t   pmm_get_total_bytes(void);
size_t   pmm_get_used_bytes(void);
size_t   pmm_get_free_bytes(void);

/* Backward-compatible wrappers for alloc_pages */
paddr_t  alloc_pages(uint32_t n);
void     free_pages(paddr_t paddr, uint32_t n);
