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

static inline bool is_user_va(vaddr_t va) {
    return va >= USER_BASE && va < USER_END;
}

/*
 * Map one page. The USER bit is only honoured inside [USER_BASE, USER_END);
 * kernel addresses can never become user-accessible, even by mistake.
 * A page-directory entry gets USER|WRITABLE only for user-region slots
 * (the real permission is decided per PTE).
 */
void vmm_map_page(uint32_t *pd, vaddr_t vaddr, paddr_t paddr, uint32_t flags) {
    uint32_t pde_idx = vaddr >> 22;
    uint32_t pte_idx = (vaddr >> 12) & 0x3FF;

    if ((flags & VMM_FLAG_USER) && !is_user_va(vaddr)) {
        KERROR("VMM", "refusing USER mapping of kernel address 0x%08x", vaddr);
        flags &= ~VMM_FLAG_USER;
    }

    if ((pd[pde_idx] & VMM_FLAG_PRESENT) == 0) {
        paddr_t pt_paddr = pmm_alloc_frame();
        if (!pt_paddr) {
            PANIC("VMM: Out of memory allocating page table for vaddr 0x%08x", vaddr);
        }
        uint32_t pde_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        if (is_user_va(vaddr)) pde_flags |= VMM_FLAG_USER;
        pd[pde_idx] = pt_paddr | pde_flags;
    }

    uint32_t *page_table = (uint32_t *)(pd[pde_idx] & ~0xFFFu);
    page_table[pte_idx]  = (paddr & ~0xFFFu) | (flags & 0xFFFu) | VMM_FLAG_PRESENT;
    vmm_invlpg(vaddr);
}

/* Look up a user page. Succeeds only if the PDE and PTE are present and the
 * PTE carries every bit in `need` (VMM_FLAG_USER [| VMM_FLAG_WRITABLE]).
 * On success *out_paddr is the physical address of the byte at `vaddr`. */
bool vmm_user_lookup(uint32_t *pd, vaddr_t vaddr, uint32_t need, paddr_t *out_paddr) {
    if (!pd || !is_user_va(vaddr)) return false;
    uint32_t pde = pd[vaddr >> 22];
    if ((pde & (VMM_FLAG_PRESENT | VMM_FLAG_USER)) != (VMM_FLAG_PRESENT | VMM_FLAG_USER))
        return false;
    uint32_t *pt  = (uint32_t *)(pde & ~0xFFFu);
    uint32_t  pte = pt[(vaddr >> 12) & 0x3FF];
    need |= VMM_FLAG_PRESENT;
    if ((pte & need) != need) return false;
    if (out_paddr) *out_paddr = (pte & ~0xFFFu) | (vaddr & 0xFFFu);
    return true;
}

/* Free every user page, user page table and the directory itself.
 * Must not be called while `pd` is the active CR3. */
void vmm_destroy_address_space(uint32_t *pd) {
    if (!pd || pd == kernel_page_dir) return;
    for (uint32_t i = USER_BASE >> 22; i < (USER_END >> 22); i++) {
        if (!(pd[i] & VMM_FLAG_PRESENT)) continue;
        uint32_t *pt = (uint32_t *)(pd[i] & ~0xFFFu);
        for (int j = 0; j < 1024; j++) {
            if (pt[j] & VMM_FLAG_PRESENT)
                pmm_free_frame(pt[j] & ~0xFFFu);
        }
        pmm_free_frame((paddr_t)pt);
    }
    pmm_free_frame((paddr_t)pd);
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

    /* Kernel slots: share the master page tables (supervisor-only).
     * User slots: empty and private. */
    for (uint32_t i = 0; i < 1024; i++) {
        bool user_slot = (i >= (USER_BASE >> 22)) && (i < (USER_END >> 22));
        pd[i] = user_slot ? 0 : kernel_page_dir[i];
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
    bool from_user = TF_FROM_USER(f);

    if (from_user) {
        /* A user fault only kills the offending process. */
        printf("[vmm] pid %d (%s): user page fault at 0x%08x (%s, %s) eip=0x%08x - killed\n",
               current_proc ? current_proc->pid : -1,
               current_proc ? current_proc->name : "?",
               fault_addr, action, cause, f->eip);
        KWARN("VMM", "user page fault at 0x%08x eip=0x%08x err=0x%x", fault_addr, f->eip, err);
        process_exit(-14);   /* does not return */
    }

    KERROR("VMM", "KERNEL PAGE FAULT at 0x%08x [eip=0x%08x err=0x%x]", fault_addr, f->eip, err);
    printf("\n*** KERNEL PAGE FAULT ***\n");
    printf("  Faulting linear address : 0x%08x\n", fault_addr);
    printf("  Instruction pointer     : 0x%08x\n", f->eip);
    printf("  Error code              : 0x%04x (%s on %s)\n", err, action, cause);
    PANIC("Fatal kernel page fault at 0x%08x", fault_addr);
}

void vmm_init(void) {
    paddr_t pd_paddr = pmm_alloc_frame();
    if (!pd_paddr) {
        PANIC("VMM: Failed to allocate kernel page directory");
    }
    kernel_page_dir = (uint32_t *)pd_paddr;

    /* Identity map 0 through __free_ram_end */
    vaddr_t limit = (vaddr_t)__free_ram_end;
    if (limit > USER_BASE)
        PANIC("VMM: kernel image/RAM pool (0x%x) overlaps USER_BASE", limit);
    for (vaddr_t p = 0; p < limit; p += PAGE_SIZE) {
        vmm_map_page(kernel_page_dir, p, p, VMM_FLAG_WRITABLE);
    }

    /* Also map VGA buffer at 0xB8000 explicitly */
    vmm_map_page(kernel_page_dir, 0xB8000, 0xB8000, VMM_FLAG_WRITABLE);

    vmm_switch_dir(kernel_page_dir);
    enable_paging();

    /* CR0.WP: make ring 0 honour read-only PTEs too. */
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1u << 16);
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));

    KINFO("VMM", "Paging enabled with 2-level page directory (identity-mapped to %x)", limit);
}
