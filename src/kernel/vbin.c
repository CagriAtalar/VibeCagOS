/*
 * VibeCagOS - VBIN loader implementation. See vbin.h for the contract.
 *
 * Validation is deliberately paranoid: every size is checked against the real
 * file size with overflow-safe arithmetic BEFORE any page is mapped, so a
 * malformed image fails cleanly with -ENOEXEC and can never make the loader
 * read past the image or map outside the user window.
 */
#include "vbin.h"
#include "vmm.h"
#include "pmm.h"
#include "kernel.h"
#include "../abi/syscall.h"

#define VBIN_STACK_LIMIT 0x1FFFC000u   /* must stay below the user stack */

static char vbin_errbuf[64];

const char *vbin_error(void) {
    return vbin_errbuf[0] ? vbin_errbuf : "unknown";
}

static int fail(const char *why) {
    strncpy(vbin_errbuf, why, sizeof(vbin_errbuf) - 1);
    vbin_errbuf[sizeof(vbin_errbuf) - 1] = '\0';
    return -E_INVAL;
}

int vbin_load(uint32_t *pd, const void *image, size_t size, uint32_t *entry) {
    const uint8_t *img = (const uint8_t *)image;

    memset(vbin_errbuf, 0, sizeof(vbin_errbuf));

    if (!pd || !image || !entry) return fail("bad argument");
    if (pd == vmm_get_kernel_dir()) return fail("refusing to load into the kernel directory");
    if ((uint32_t)pd == read_cr3())  return fail("refusing to load into the active directory");

    if (size < sizeof(struct vbin_header)) return fail("truncated VBIN header");
    const struct vbin_header *h = (const struct vbin_header *)img;
    if (h->magic != VBIN_MAGIC)     return fail("bad VBIN magic");
    if (h->version != VBIN_VERSION) return fail("bad VBIN version");
    if (h->flags != 0)              return fail("bad VBIN flags");

    /* Overflow-safe: the file must contain everything the header promises. */
    if (h->text_size > size - sizeof(*h)) return fail("text past end of file");
    if (h->data_size > size - sizeof(*h) - h->text_size)
        return fail("data past end of file");

    if (h->mem_size == 0 || h->mem_size > VBIN_MAX_SPAN)
        return fail("bad mem_size");
    /* The memory image must hold text+data (the rest is BSS). Subtraction
     * first so a hostile text_size cannot wrap the addition. */
    if (h->text_size > h->mem_size) return fail("text larger than memory image");
    if (h->data_size > h->mem_size - h->text_size)
        return fail("text+data larger than memory image");

    /* The whole span must stay below the user stack. */
    if (h->mem_size > VBIN_STACK_LIMIT - USER_BASE)
        return fail("image reaches the user stack");

    if (h->text_size == 0) return fail("empty text");
    /* A shared RO/RW page would have to be writable, silently de-protecting
     * code — so whenever data exists the text must end on a page boundary.
     * (vbinpack guarantees this; a hand-built image must too.) */
    if (h->data_size > 0 && (h->text_size & (PAGE_SIZE - 1u)) != 0)
        return fail("text/data share a page");

    /* Entry must be inside the text region we actually mapped. */
    if (h->entry < USER_BASE || h->entry >= USER_BASE + h->text_size)
        return fail("entry point outside text");

    /* pass 1: map everything writable, copy exactly the file-backed bytes.
     * Every page starts at a page boundary (USER_BASE is page-aligned and so
     * is each blob start), so a page belongs to exactly one blob and the copy
     * is a single memcpy. BSS pages ([text+data, mem)) are mapped and left
     * zeroed — unlike the old ELF loader, nothing is ever copied past the
     * file-backed extent of a blob. */
    uint32_t vend = USER_BASE + h->mem_size;
    for (uint32_t va = USER_BASE; va < vend; va += PAGE_SIZE) {
        uint32_t off = va - USER_BASE;   /* offset inside the memory image */
        paddr_t frame = pmm_alloc_frame();          /* zero filled */
        if (!frame) return fail("out of memory mapping image");
        vmm_map_page(pd, va, frame, VMM_FLAG_USER | VMM_FLAG_WRITABLE);

        uint32_t fbase, mem_start, mem_len;
        if (off < h->text_size) {
            fbase = sizeof(*h); mem_start = 0; mem_len = h->text_size;
        } else if (off - h->text_size < h->data_size) {
            fbase = sizeof(*h) + h->text_size;
            mem_start = h->text_size; mem_len = h->data_size;
        } else {
            continue;   /* BSS: already zero */
        }
        uint32_t page_off = off - mem_start;   /* offset inside this blob */
        uint32_t n = mem_len - page_off;       /* blob bytes from here on */
        if (n > PAGE_SIZE) n = PAGE_SIZE;
        memcpy((void *)frame, img + fbase + page_off, n);
    }

    /* pass 2: demote every text page to read-only. */
    for (uint32_t va = USER_BASE; va < USER_BASE + h->text_size; va += PAGE_SIZE)
        vmm_set_page_flags(pd, va, VMM_FLAG_USER);

    *entry = h->entry;
    return 0;
}