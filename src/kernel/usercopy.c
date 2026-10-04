/*
 * VibeCagOS - user memory access layer. See usercopy.h for the contract.
 *
 * Kernel runs with the identity map, so after translating a user page to its
 * physical frame the kernel can touch it directly: no CR3 tricks and no
 * kernel-mode page faults are possible while copying.
 */
#include "usercopy.h"
#include "kernel.h"
#include "vmm.h"
#include "../abi/syscall.h"

static uint32_t *cur_pd(void) {
    return current_proc ? current_proc->page_table : NULL;
}

/* Validate [addr, addr+len) page by page. */
static bool range_ok(uint32_t addr, size_t len, bool write) {
    uint32_t *pd = cur_pd();
    if (!pd) return false;
    if (len == 0) return true;
    uint32_t end = addr + (uint32_t)len;
    if (end < addr) return false;                      /* wraps */
    if (addr < USER_BASE || end > USER_END) return false;

    uint32_t need = VMM_FLAG_USER | (write ? VMM_FLAG_WRITABLE : 0);
    for (uint32_t page = addr & ~0xFFFu; page < end; page += PAGE_SIZE) {
        if (!vmm_user_lookup(pd, page, need, NULL)) return false;
    }
    return true;
}

bool user_range_valid(const void *p, size_t len, bool write) {
    return range_ok((uint32_t)p, len, write);
}

bool user_ptr_valid(const void *p, bool write) {
    return range_ok((uint32_t)p, 1, write);
}

static void xfer(void *k, uint32_t uaddr, size_t len, bool to_user) {
    uint32_t *pd = cur_pd();
    uint8_t  *kp = (uint8_t *)k;
    while (len) {
        paddr_t pa;
        (void)vmm_user_lookup(pd, uaddr, VMM_FLAG_USER, &pa);   /* validated */
        size_t chunk = PAGE_SIZE - (uaddr & 0xFFFu);
        if (chunk > len) chunk = len;
        if (to_user) memcpy((void *)pa, kp, chunk);
        else         memcpy(kp, (void *)pa, chunk);
        kp += chunk; uaddr += (uint32_t)chunk; len -= chunk;
    }
}

int copy_from_user(void *kdst, const void *usrc, size_t len) {
    if (!range_ok((uint32_t)usrc, len, false)) return -E_FAULT;
    xfer(kdst, (uint32_t)usrc, len, false);
    return 0;
}

int copy_to_user(void *udst, const void *ksrc, size_t len) {
    if (!range_ok((uint32_t)udst, len, true)) return -E_FAULT;
    xfer((void *)ksrc, (uint32_t)udst, len, true);
    return 0;
}

int strncpy_from_user(char *kdst, const char *usrc, size_t max) {
    uint32_t *pd = cur_pd();
    uint32_t  ua = (uint32_t)usrc;
    for (size_t i = 0; i < max; i++, ua++) {
        paddr_t pa;
        if (ua < USER_BASE || ua >= USER_END ||
            !vmm_user_lookup(pd, ua, VMM_FLAG_USER, &pa))
            return -E_FAULT;
        char c = *(const char *)pa;
        kdst[i] = c;
        if (c == '\0') return (int)i;
    }
    return -E_INVAL;
}
