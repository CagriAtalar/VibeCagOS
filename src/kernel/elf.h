/*
 * VibeCagOS - minimal ELF32 (i386) loader.
 *
 * Only what a static, non-shared, PT_LOAD-only executable needs. The loader
 * is deliberately strict: every field it trusts is checked against the real
 * image size, and every address it maps is checked against the user window.
 *
 * NOTE on NX: 32-bit paging without PAE has no "not executable" bit, so PF_X
 * cannot be enforced. It is still parsed so that the R/W/X split is visible in
 * the source and so an NX-capable machine can honour it later. What we DO
 * enforce is that a non-writable segment's pages are never left writable.
 */
#pragma once
#include "common.h"

/* e_ident indices */
#define ELF_NIDENT        16
#define EI_MAG0           0
#define EI_CLASS          4      /* ELFCLASS32 == 1 */
#define EI_DATA           5      /* ELFDATA2LSB == 1 */
#define EI_VERSION        6

#define ELFCLASS32        1
#define ELFDATA2LSB       1
#define EV_CURRENT        1

#define ET_EXEC           2      /* we refuse ET_DYN: no PIC, no ASLR */
#define EM_386            3

#define PT_NULL           0
#define PT_LOAD           1

#define PF_X              (1 << 0)
#define PF_W              (1 << 1)
#define PF_R              (1 << 2)

struct elf32_ehdr {
    uint8_t  e_ident[ELF_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    uint32_t p_type;
    uint32_t p_offset;    /* offset in the file of p_bytes */
    uint32_t p_vaddr;     /* virtual address in memory */
    uint32_t p_paddr;     /* unused (no physical load address) */
    uint32_t p_filesz;    /* bytes present in the file */
    uint32_t p_memsz;     /* bytes in memory; p_filesz < p_memsz == .bss */
    uint32_t p_flags;     /* PF_* */
    uint32_t p_align;
} __attribute__((packed));

/* Loader limits - a bigger value needs a bigger bitmap in elf.c. */
#define ELF_MAX_SEGS        8
#define ELF_MAX_IMAGE_SPAN  (16u << 20)     /* 16 MiB of virtual address space */
#define ELF_STACK_LIMIT     0x1FFFC000u     /* must stay below the user stack */

/*
 * elf_load - map an ELF32 executable into the page directory `pd`.
 *
 * `image`/`size` is the raw file (e.g. what vfs_read handed us). On success
 * stores the entry point in *entry and returns 0; on failure returns a
 * negative errno and leaves `pd` holding whatever was mapped so far (the
 * caller destroys the address space, so no unwinding is needed).
 *
 * Mapping rules per PT_LOAD:
 *   - pages are mapped PRESENT | USER, plus WRITABLE only for PF_W segments
 *   - p_filesz bytes are copied from the file, the rest (p_memsz - p_filesz,
 *     i.e. .bss) is left zero: pmm hands out pre-zeroed frames
 *   - a page shared by two segments is mapped once and gets the union of the
 *     requested permissions
 *   - anything outside [USER_BASE, ELF_STACK_LIMIT) is rejected
 */
int elf_load(uint32_t *pd, const void *image, size_t size, uint32_t *entry);

/* Human readable reason for the last elf_load() failure (for logs). */
const char *elf_error(void);