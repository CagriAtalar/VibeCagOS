/*
 * VibeCagOS — User memory access helpers
 */

#include "uaccess.h"
#include "kernel.h"
#include "vmm.h"

extern struct process *current_proc;

bool user_range_ok(const void *ptr, size_t len, bool write) {
    uint32_t start = (uint32_t)ptr;
    uint32_t end   = start + (uint32_t)len;

    if (!current_proc || !current_proc->page_table)
        return false;
    if (len == 0)
        return true;
    if (end < start)                          /* wrap-around */
        return false;
    if (start < USER_BASE || end > USER_LIMIT)
        return false;

    for (uint32_t page = start & ~(PAGE_SIZE - 1u); page < end; page += PAGE_SIZE) {
        if (!vmm_check_user(current_proc->page_table, page, write))
            return false;
    }
    return true;
}

int user_strncpy(char *dst, const char *usrc, size_t max) {
    if (max == 0) return -1;

    uint32_t addr = (uint32_t)usrc;
    for (size_t i = 0; i < max; i++, addr++) {
        /* Re-validate whenever we enter a new page */
        if (i == 0 || (addr & (PAGE_SIZE - 1u)) == 0) {
            if (addr < USER_BASE || addr >= USER_LIMIT ||
                !current_proc || !current_proc->page_table ||
                !vmm_check_user(current_proc->page_table, addr, false))
                return -1;
        }
        char c = *(const char *)addr;
        dst[i] = c;
        if (c == '\0')
            return (int)i;
    }
    dst[max - 1] = '\0';
    return -1;                                /* too long */
}
