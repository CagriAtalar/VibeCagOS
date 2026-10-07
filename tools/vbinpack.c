/*
 * vbinpack - host tool that converts a linked ELF32 executable into a VBIN
 * image (see docs/VBIN.md).
 *
 * This runs on the BUILD machine (plain host cc, no -m32): it parses the
 * linker's ELF output and extracts what the kernel needs. The kernel never
 * sees ELF; anything this tool rejects fails the build instead of becoming
 * a runtime loader bug.
 *
 *   usage: vbinpack in.elf out.vbin
 *
 * Exit 0 on success, 1 with a message on stderr on any rejection.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define USER_BASE   0x10000000u
#define STACK_LIMIT 0x1FFFC000u
#define VBIN_MAX_SPAN (1u << 20)

#define VBIN_MAGIC   0x4E494256u
#define VBIN_VERSION 1u

struct vbin_header {
    uint32_t magic;
    uint32_t version;
    uint32_t entry;
    uint32_t text_size;
    uint32_t data_size;
    uint32_t mem_size;
    uint32_t flags;
};

/* Minimal ELF32 reader (little-endian host assumed; the inputs are ours). */
typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elfhdr_t;

typedef struct {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} phdr_t;

#define PT_LOAD 1
#define PF_W    2

static uint8_t *file_data;
static size_t   file_len;

static void fail(const char *msg) {
    fprintf(stderr, "vbinpack: %s\n", msg);
    exit(1);
}

static void read_at(void *dst, size_t off, size_t n, const char *what) {
    if (off > file_len || n > file_len - off) {
        fprintf(stderr, "vbinpack: %s past end of file\n", what);
        exit(1);
    }
    memcpy(dst, file_data + off, n);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: vbinpack in.elf out.vbin\n");
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("vbinpack: open input"); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) fail("empty input");
    file_len = (size_t)len;
    file_data = malloc(file_len);
    if (!file_data) fail("out of memory");
    if (fread(file_data, 1, file_len, f) != file_len) fail("short read");
    fclose(f);

    elfhdr_t eh;
    read_at(&eh, 0, sizeof(eh), "ELF header");
    if (file_len < 52) fail("truncated ELF header");
    if (eh.ident[0] != 0x7F || eh.ident[1] != 'E' ||
        eh.ident[2] != 'L' || eh.ident[3] != 'F') fail("bad ELF magic");
    if (eh.ident[4] != 1) fail("not ELF32");
    if (eh.machine != 3)  fail("not EM_386");
    if (eh.type != 2)     fail("not ET_EXEC (refusing PIC/dynamic input)");
    if (eh.phentsize != sizeof(phdr_t)) fail("bad e_phentsize");

    /* Partition PT_LOADs into one RO span and one RW span. */
    uint32_t ro_start = 0xFFFFFFFFu, ro_end = 0, ro_fend = 0;
    uint32_t rw_start = 0xFFFFFFFFu, rw_end = 0, rw_fend = 0;
    int have_ro = 0, have_rw = 0;
    for (int i = 0; i < eh.phnum; i++) {
        phdr_t ph;
        read_at(&ph, eh.phoff + (size_t)i * sizeof(ph), sizeof(ph),
                "program header");
        if (ph.type != PT_LOAD) continue;
        if (ph.memsz == 0) fail("empty PT_LOAD");
        if (ph.filesz > ph.memsz) fail("p_filesz > p_memsz");
        if (ph.offset > file_len ||
            ph.filesz > file_len - ph.offset) fail("segment past end of file");
        if (ph.vaddr < USER_BASE) fail("segment below USER_BASE");
        if (ph.memsz > STACK_LIMIT - ph.vaddr) fail("segment reaches user stack");
        if (ph.flags & PF_W) {
            if (!have_rw) { rw_start = ph.vaddr; have_rw = 1; }
            if (ph.vaddr + ph.memsz > rw_end) rw_end = ph.vaddr + ph.memsz;
            if (ph.vaddr + ph.filesz > rw_fend) rw_fend = ph.vaddr + ph.filesz;
        } else {
            if (!have_ro) { ro_start = ph.vaddr; have_ro = 1; }
            if (ph.vaddr + ph.memsz > ro_end) ro_end = ph.vaddr + ph.memsz;
            if (ph.vaddr + ph.filesz > ro_fend) ro_fend = ph.vaddr + ph.filesz;
        }
    }
    if (!have_ro) fail("no read-only PT_LOAD (no text?)");
    if (ro_start != USER_BASE) fail("text does not start at USER_BASE");

    /* RO blob: full span, holes zero-filled, rounded up to a page so a data
     * region never shares a page with code. */
    uint32_t text_size = (ro_end - USER_BASE + 0xFFFu) & ~0xFFFu;
    uint8_t *text = calloc(1, text_size ? text_size : 1);
    if (!text) fail("out of memory");
    for (int i = 0; i < eh.phnum; i++) {
        phdr_t ph;
        read_at(&ph, eh.phoff + (size_t)i * sizeof(ph), sizeof(ph),
                "program header");
        if (ph.type != PT_LOAD || (ph.flags & PF_W) || ph.filesz == 0) continue;
        memcpy(text + (ph.vaddr - USER_BASE), file_data + ph.offset, ph.filesz);
    }

    /* RW blob: spans the writable segments; holes zero-filled. */
    uint32_t data_size = 0, mem_size = text_size;
    uint8_t *data = NULL;
    if (have_rw) {
        if (rw_start != USER_BASE + text_size)
            fail("writable span does not follow the text span page-aligned");
        data_size = rw_fend - rw_start;
        mem_size = rw_end - USER_BASE;
        data = calloc(1, data_size ? data_size : 1);
        if (!data) fail("out of memory");
        for (int i = 0; i < eh.phnum; i++) {
            phdr_t ph;
            read_at(&ph, eh.phoff + (size_t)i * sizeof(ph), sizeof(ph),
                    "program header");
            if (ph.type != PT_LOAD || !(ph.flags & PF_W) || ph.filesz == 0)
                continue;
            memcpy(data + (ph.vaddr - rw_start), file_data + ph.offset,
                   ph.filesz);
        }
    } else {
        /* No writable span: memory is just the (page-rounded) text. */
        mem_size = text_size;
    }
    if (mem_size == 0 || mem_size > VBIN_MAX_SPAN) fail("bad image span");
    if (text_size + data_size > mem_size) fail("internal span error");

    /* Entry must sit on file-backed text, not in a zero hole. */
    if (eh.entry < ro_start || eh.entry >= ro_fend)
        fail("entry point outside file-backed text");

    struct vbin_header h;
    h.magic      = VBIN_MAGIC;
    h.version    = VBIN_VERSION;
    h.entry      = eh.entry;
    h.text_size  = text_size;
    h.data_size  = data_size;
    h.mem_size   = mem_size;
    h.flags      = 0;

    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror("vbinpack: open output"); return 1; }
    if (fwrite(&h, 1, sizeof(h), o) != sizeof(h)) fail("short write");
    if (text_size && fwrite(text, 1, text_size, o) != text_size) fail("short write");
    if (data_size && fwrite(data, 1, data_size, o) != data_size) fail("short write");
    fclose(o);

    printf("vbinpack: %s -> %s: entry=0x%08x text=%u data=%u mem=%u\n",
           argv[1], argv[2], h.entry, h.text_size, h.data_size, h.mem_size);
    return 0;
}
