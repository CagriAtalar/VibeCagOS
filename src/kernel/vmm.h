/*
 * VibeCagOS — Virtual Memory Manager (VMM)
 *
 * 2-level x86 paging management, virtual address space creation,
 * MMIO mapping, DMA buffer allocation, and page fault decoding.
 */

#pragma once
#include "common.h"
#include "kernel.h"

/* Page table entry flags */
#define VMM_FLAG_PRESENT       (1 << 0)
#define VMM_FLAG_WRITABLE      (1 << 1)
#define VMM_FLAG_USER          (1 << 2)
#define VMM_FLAG_WRITETHROUGH  (1 << 3)
#define VMM_FLAG_NOCACHE       (1 << 4)

/* Initialize virtual memory management and activate kernel paging */
void     vmm_init(void);

/* Create a new page directory with kernel space identity-mapped */
uint32_t *vmm_create_address_space(void);

/* Map a single virtual page to a physical frame */
void     vmm_map_page(uint32_t *pd, vaddr_t vaddr, paddr_t paddr, uint32_t flags);

/* Unmap a virtual page */
void     vmm_unmap_page(uint32_t *pd, vaddr_t vaddr);

/* Replace a mapped page's flags (PRESENT/USER/WRITABLE). Used by the VBIN
 * loader to demote text pages to read-only after the file bytes are copied. */
bool     vmm_set_page_flags(uint32_t *pd, vaddr_t vaddr, uint32_t flags);

/* Query physical address for a virtual address. Returns true if mapped. */
bool     vmm_get_mapping(uint32_t *pd, vaddr_t vaddr, paddr_t *out_paddr);

/* Map a range of virtual memory to physical memory */
void     vmm_map_range(uint32_t *pd, vaddr_t vaddr, paddr_t paddr, size_t size, uint32_t flags);

/* Map MMIO device registers (with cache-disable and write-through).
 * Returns mapped virtual address. */
void    *vmm_map_mmio(paddr_t paddr, size_t size);

/* Allocate physically contiguous, page-aligned DMA memory */
void    *dma_alloc(size_t size);

/* Free DMA memory */
void     dma_free(void *ptr, size_t size);

/* Free all user pages/tables + the directory (not valid for the active CR3). */
void     vmm_destroy_address_space(uint32_t *pd);

/* User-page query: PDE+PTE present and PTE has all `need` bits
 * (VMM_FLAG_USER, optionally | VMM_FLAG_WRITABLE). Fills *out_paddr. */
bool     vmm_user_lookup(uint32_t *pd, vaddr_t vaddr, uint32_t need, paddr_t *out_paddr);

/* Switch active page directory (CR3) */
void     vmm_switch_dir(uint32_t *pd);

/* Get kernel master page directory */
uint32_t *vmm_get_kernel_dir(void);

/* Page fault interrupt handler (called from vector 14) */
void     vmm_page_fault_handler(struct trap_frame *f);

/* Invalidate TLB entry for a single page */
static inline void vmm_invlpg(vaddr_t vaddr) {
    __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr) : "memory");
}
