#pragma once
#include "common.h"

/* Same signatures as src/kernel/pmm.h; backed by a fake frame arena in
 * test_heap.c that can run out of frames and can return non-adjacent regions. */
paddr_t pmm_alloc_frames(size_t count);
void    pmm_free_frames(paddr_t paddr, size_t count);
