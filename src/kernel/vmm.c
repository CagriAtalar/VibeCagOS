/*
 * VibeCagOS — Virtual Memory Manager (VMM) Implementation
 *
 * 2-level paging on x86 (Page Directory + Page Tables).
 */

#include "vmm.h"
#include "pmm.h"
#include "kernel.h"
#include "klog.h"

extern char __kernel_base[], __free_ram_end[];

static uint32_t *kernel_page_dir = NULL;

void vmm_map_page(uint32_t *pd, vaddr_t vaddr, paddr_t paddr, uint32_t flags) {
    uint32_t pde_idx = vaddr >> 22;
    uint32_t pte_idx = (vaddr >> 12) & 0x3FF;

    if ((pd[pde_idx] & VMM_FLAG_PRESENT) == 0) {
        paddr_t pt_paddr = pmm_alloc_frame();
        if (!pt_paddr) {
            PANIC("VMM: Out of memory allocating page table for vaddr 0x%08x", vaddr);
        }
        pd[pde_idx] = pt_paddr | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | (flags & VMM_FLAG_USER);
    }

    uint32_t *page_table = (uint32_t *)(pd[pde_idx] & ~0xFFFu);
    page_table[pte_idx]  = (paddr & ~0xFFFu) | (flags & 0xFFFu) | VMM_FLAG_PRESENT;
    vmm_invlpg(vaddr);
}

void vmm_unmap_page(uint32_t *pd, vaddr_t vaddr) {
    uint32_t pde_idx = vaddr >> 22;
    uint32_t pte_idx = (vaddr >> 12) & 0x3FF;

    if ((pd[pde_idx] & VMM_FLAG_PRESENT) == 0) return;

    uint32_t *page_table = (uint32_t *)(pd[pde_idx] & ~0xFFFu);
    page_table[pte_idx] = 0;
    vmm_invlpg(vaddr);
}

bool vmm_get_mapping(uint32_t *pd, vaddr_t vaddr, paddr_t *out_paddr) {
    uint32_t pde_idx = vaddr >> 22;
    uint32_t pte_idx = (vaddr >> 12) & 0x3FF;

    if ((pd[pde_idx] & VMM_FLAG_PRESENT) == 0) return false;

    uint32_t *page_table = (uint32_t *)(pd[pde_idx] & ~0xFFFu);
    if ((page_table[pte_idx] & VMM_FLAG_PRESENT) == 0) return false;

    if (out_paddr) {
        *out_paddr = (page_table[pte_idx] & ~0xFFFu) | (vaddr & 0xFFFu);
    }
    return true;
}

void vmm_map_range(uint32_t *pd, vaddr_t vaddr, paddr_t paddr, size_t size, uint32_t flags) {
    vaddr_t va = vaddr & ~0xFFFu;
    paddr_t pa = paddr & ~0xFFFu;
    size_t end = (vaddr + size + PAGE_SIZE - 1) & ~0xFFFu;

    while (va < end) {
        vmm_map_page(pd, va, pa, flags);
        va += PAGE_SIZE;
        pa += PAGE_SIZE;
    }
}

void *vmm_map_mmio(paddr_t paddr, size_t size) {
    /* Identity-map MMIO region with Cache Disable & Write-Through flags */
    uint32_t flags = VMM_FLAG_WRITABLE | VMM_FLAG_NOCACHE | VMM_FLAG_WRITETHROUGH;
    vmm_map_range(kernel_page_dir, paddr, paddr, size, flags);
    return (void *)paddr;
}

void *dma_alloc(size_t size) {
    if (size == 0) return NULL;
    size_t frames = (size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    paddr_t p = pmm_alloc_frames(frames);
    return (void *)p;
}

void dma_free(void *ptr, size_t size) {
    if (!ptr || size == 0) return;
    size_t frames = (size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    pmm_free_frames((paddr_t)ptr, frames);
}

uint32_t *vmm_create_address_space(void) {
    paddr_t pd_paddr = pmm_alloc_frame();
    if (!pd_paddr) return NULL;
    uint32_t *pd = (uint32_t *)pd_paddr;

    /* Copy kernel space mappings from master page directory */
    for (int i = 0; i < 1024; i++) {
        pd[i] = kernel_page_dir[i];
    }
    return pd;
}

void vmm_switch_dir(uint32_t *pd) {
    load_cr3((uint32_t)pd);
}

uint32_t *vmm_get_kernel_dir(void) {
    return kernel_page_dir;
}

void vmm_page_fault_handler(struct trap_frame *f) {
    uint32_t fault_addr = read_cr2();
    uint32_t err = f->err_code;

    const char *cause = (err & 1) ? "protection violation" : "page not present";
    const char *action = (err & 2) ? "write" : "read";
    const char *mode = (err & 4) ? "user" : "kernel";

    KERROR("VMM", "PAGE FAULT at 0x%08x [eip=0x%08x err=0x%x: %s on %s in %s mode]",
           fault_addr, f->eip, err, action, cause, mode);

    printf("\n*** PAGE FAULT ***\n");
    printf("  Faulting linear address : 0x%08x\n", fault_addr);
    printf("  Instruction pointer     : 0x%08x\n", f->eip);
    printf("  Error code              : 0x%04x (%s on %s in %s mode)\n",
           err, action, cause, mode);

    if ((err & 4) != 0) {
        /* User-mode fault */
        printf("Terminating user process.\n");
        process_exit(-1);
    } else {
        /* Kernel fault is fatal */
        PANIC("Fatal kernel page fault at 0x%08x", fault_addr);
    }
}

void vmm_init(void) {
    paddr_t pd_paddr = pmm_alloc_frame();
    if (!pd_paddr) {
        PANIC("VMM: Failed to allocate kernel page directory");
    }
    kernel_page_dir = (uint32_t *)pd_paddr;

    /* Identity map 0 through __free_ram_end */
    vaddr_t limit = (vaddr_t)__free_ram_end;
    for (vaddr_t p = 0; p < limit; p += PAGE_SIZE) {
        vmm_map_page(kernel_page_dir, p, p, VMM_FLAG_WRITABLE);
    }

    /* Also map VGA buffer at 0xB8000 explicitly */
    vmm_map_page(kernel_page_dir, 0xB8000, 0xB8000, VMM_FLAG_WRITABLE);

    vmm_switch_dir(kernel_page_dir);
    enable_paging();

    KINFO("VMM", "Paging enabled with 2-level page directory (identity-mapped to %x)", limit);
}
