/*
 * VibeCagOS - minimal ELF32 (i386) loader implementation.
 *
 * Loading a PT_LOAD segment means: map a frame for every page it touches,
 * copy the file bytes in, and make sure a page nobody asked to write to ends
 * up read-only. x86-32 has no NX bit, so PF_X is parsed but not enforced.
 *
 * Two passes are needed because pages are mapped WRITABLE while the file bytes
 * are copied in, and only afterwards demoted to read-only:
 *
 *   pass 1  validate the whole image, allocate frames, map USER|WRITABLE,
 *           copy p_filesz bytes (the tail up to p_memsz stays zero)
 *   pass 2  for every page that no PF_W segment covers, clear the WRITE bit
 *
 * A page touched by two segments is mapped once and gets the union of the
 * permissions, which is what a real linker would have produced anyway.
 *
 * The page directory handed in must NOT be the active CR3: pass 2 edits PTEs
 * and there is no TLB shootdown for a directory we are not using.
 */
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "kernel.h"
#include "../abi/syscall.h"

#define ELF_PAGES      (ELF_MAX_IMAGE_SPAN / PAGE_SIZE)
#define BIT_WORD_BITS  32
#define BIT_WORDS      ((ELF_PAGES + BIT_WORD_BITS - 1) / BIT_WORD_BITS)

static uint32_t mapped_bitmap[BIT_WORDS];   /* page is allocated            */
static uint32_t writable_bitmap[BIT_WORDS];  /* page must stay writable      */
static char     elf_errbuf[64];

static inline uint32_t page_index(uint32_t va) {
    return (va - USER_BASE) / PAGE_SIZE;
}
static inline bool bit_test(const uint32_t *bm, uint32_t i) {
    return (bm[i / BIT_WORD_BITS] >> (i % BIT_WORD_BITS)) & 1u;
}
static inline void bit_set(uint32_t *bm, uint32_t i) {
    bm[i / BIT_WORD_BITS] |= 1u << (i % BIT_WORD_BITS);
}

const char *elf_error(void) {
    return elf_errbuf[0] ? elf_errbuf : "unknown";
}

static int fail(const char *why) {
    strncpy(elf_errbuf, why, sizeof(elf_errbuf) - 1);
    elf_errbuf[sizeof(elf_errbuf) - 1] = '\0';
    return -E_INVAL;
}

/* ---- header validation -------------------------------------------------- */

static bool header_ok(const struct elf32_ehdr *eh, size_t size) {
    if (size < sizeof(*eh))                 return fail("truncated ELF header");
    if (eh->e_ident[EI_MAG0] != 0x7F ||
        eh->e_ident[EI_MAG0 + 1] != 'E' ||
        eh->e_ident[EI_MAG0 + 2] != 'L' ||
        eh->e_ident[EI_MAG0 + 3] != 'F')   return fail("bad ELF magic");
    if (eh->e_ident[EI_CLASS] != ELFCLASS32)return fail("not ELF32");
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB)return fail("not little endian");
    if (eh->e_ident[EI_VERSION] != EV_CURRENT) return fail("bad ident version");
    if (eh->e_type != ET_EXEC)              return fail("not ET_EXEC");
    if (eh->e_machine != EM_386)            return fail("not EM_386");
    if (eh->e_version != EV_CURRENT)        return fail("bad e_version");

    if (eh->e_phentsize != sizeof(struct elf32_phdr)) return fail("bad e_phentsize");
    if (eh->e_phnum == 0 || eh->e_phnum > ELF_MAX_SEGS) return fail("bad e_phnum");
    if (eh->e_phoff < sizeof(*eh))          return fail("e_phoff overlaps header");
    /* Integer-safe: the program header table must lie inside the image. */
    if (eh->e_phoff > size)                 return fail("e_phoff past end of file");
    if ((uint32_t)eh->e_phnum > (size - eh->e_phoff) / sizeof(struct elf32_phdr))
        return fail("program headers past end of file");
    return true;
}

/* Collect PT_LOAD segments, rejecting anything we cannot honour. */
static int collect_loads(const struct elf32_ehdr *eh, const uint8_t *img, size_t size,
                         struct elf32_phdr *out, int *count) {
    const struct elf32_phdr *ph = (const struct elf32_phdr *)(img + eh->e_phoff);
    int n = 0;

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_filesz > ph[i].p_memsz)     return fail("p_filesz > p_memsz");
        if (ph[i].p_offset > size ||
            (uint32_t)ph[i].p_filesz > size - ph[i].p_offset)
            return fail("segment file range past end of file");
        if (ph[i].p_memsz == 0)                 return fail("empty segment");

        /* Both ends must be inside the user window and below the user stack.
         * Checked with subtraction so a hostile p_memsz cannot wrap us. */
        uint32_t start = ph[i].p_vaddr & ~(PAGE_SIZE - 1u);
        if (ph[i].p_vaddr < USER_BASE)          return fail("segment below USER_BASE");
        if (ph[i].p_memsz > ELF_STACK_LIMIT - ph[i].p_vaddr)
            return fail("segment reaches the user stack");
        uint32_t end   = (ph[i].p_vaddr + ph[i].p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (end > ELF_STACK_LIMIT)              return fail("segment overlaps user stack");
        if (end - start > ELF_MAX_IMAGE_SPAN)   return fail("segment too large");
        if (page_index(end) >= ELF_PAGES)       return fail("segment outside image span");
        if (n >= ELF_MAX_SEGS)                  return fail("too many PT_LOAD segments");

        /* Page-congruent p_offset keeps the copy arithmetic trivial. */
        if ((ph[i].p_offset & (PAGE_SIZE - 1)) != (ph[i].p_vaddr & (PAGE_SIZE - 1)))
            return fail("segment offset/address not congruent");

        out[n++] = ph[i];
    }
    if (n == 0) return fail("no PT_LOAD segments");
    *count = n;
    return 0;
}

/* ---- loading ------------------------------------------------------------ */

int elf_load(uint32_t *pd, const void *image, size_t size, uint32_t *entry) {
    const uint8_t *img = (const uint8_t *)image;
    struct elf32_phdr segs[ELF_MAX_SEGS];
    int nsegs = 0;

    memset(elf_errbuf, 0, sizeof(elf_errbuf));
    memset(mapped_bitmap, 0, sizeof(mapped_bitmap));
    memset(writable_bitmap, 0, sizeof(writable_bitmap));

    if (!pd || !image || !entry) return fail("bad argument");
    if (pd == vmm_get_kernel_dir()) return fail("refusing to load into the kernel directory");
    if ((uint32_t)pd == read_cr3())  return fail("refusing to load into the active directory");

    const struct elf32_ehdr *eh = (const struct elf32_ehdr *)img;
    if (header_ok(eh, size) < 0) return -E_INVAL;
    if (collect_loads(eh, img, size, segs, &nsegs) < 0) return -E_INVAL;

    /* The entry point must land inside a segment we actually mapped. */
    bool entry_ok = false;
    for (int i = 0; i < nsegs; i++)
        if (eh->e_entry >= segs[i].p_vaddr &&
            eh->e_entry <  segs[i].p_vaddr + segs[i].p_memsz)
            entry_ok = true;
    if (!entry_ok) return fail("entry point outside every PT_LOAD segment");

    /* pass 1: map writable, copy the file bytes in */
    for (int i = 0; i < nsegs; i++) {
        uint32_t va    = segs[i].p_vaddr & ~(PAGE_SIZE - 1u);
        uint32_t vend  = (segs[i].p_vaddr + segs[i].p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint32_t fsrc  = segs[i].p_offset - (segs[i].p_vaddr - va);  /* == congruent */
        uint32_t fend  = fsrc + segs[i].p_memsz;

        for (; va < vend; va += PAGE_SIZE, fsrc += PAGE_SIZE) {
            uint32_t idx = page_index(va);
            paddr_t frame;
            if (!bit_test(mapped_bitmap, idx)) {
                frame = pmm_alloc_frame();          /* zero filled */
                if (!frame) return fail("out of memory mapping segment");
                bit_set(mapped_bitmap, idx);
                vmm_map_page(pd, va, frame, VMM_FLAG_USER | VMM_FLAG_WRITABLE);
            }
            if (segs[i].p_flags & PF_W) bit_set(writable_bitmap, idx);

            /* Copy whatever part of this page the file actually provides. */
            uint32_t fpage_end = fsrc + PAGE_SIZE;
            uint32_t copy_end  = fend < fpage_end ? fend : fpage_end;
            if (copy_end > fsrc) {
                uint32_t bytes = copy_end - fsrc;
                /* p_offset/p_filesz were validated against `size`. */
                memcpy((void *)(frame + (va & (PAGE_SIZE - 1))), img + fsrc, bytes);
            }
            /* bytes between p_filesz and p_memsz stay zero: fresh frames are 0 */
        }
    }

    /* pass 2: demote every page nobody needs to write to */
    for (uint32_t idx = 0; idx < ELF_PAGES; idx++) {
        if (!bit_test(mapped_bitmap, idx) || bit_test(writable_bitmap, idx)) continue;
        uint32_t va = USER_BASE + idx * PAGE_SIZE;
        vmm_set_page_flags(pd, va, VMM_FLAG_USER);
    }

    *entry = eh->e_entry;
    return 0;
}