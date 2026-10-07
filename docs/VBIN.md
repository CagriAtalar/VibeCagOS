# VibeCagOS — VBIN Executable Format

> **Status:** implemented. VBIN is the one and only runtime executable format.
> The kernel has no ELF loader; host ELF appears only as a build intermediate.

## Why a custom format

The kernel used to parse ELF32 program headers at runtime. That worked, but it
put a general-purpose object-format parser on the most security-sensitive path
in the system (loading untrusted bytes into page tables). VBIN moves every
decision the linker already made into a host-side packer, so the kernel loader
only does what a kernel must do: validate a flat header, map pages, copy
bytes, zero BSS.

```
hello.c
  |
  v                  HOST (build machine)
assembler/linker -----> hello.elf (ET_EXEC, i386, intermediate)
  |
  v
vbinpack -------------> hello.vbin (VBIN, what the OS ships)
  |
  v                  TARGET (VibeCagOS)
VibeFS /bin/hello
  |
  v
vbin_load() ----------> Ring 3
```

## Layout

```
+----------------------+
| vbin_header (28 B)   |
+----------------------+
| text+rodata          |  text_size bytes, mapped read-only
+----------------------+
| data                 |  data_size bytes, mapped read-write
+----------------------+
```

Everything after `text_size + data_size` up to `mem_size` is BSS and is left
zero (the PMM hands out zeroed frames, so the loader copies nothing there).

```c
#define VBIN_MAGIC   0x4E494256u   /* 'VBIN', little endian */
#define VBIN_VERSION 1

struct vbin_header {
    uint32_t magic;       /* VBIN_MAGIC */
    uint32_t version;     /* VBIN_VERSION */
    uint32_t entry;       /* absolute virtual address of _start */
    uint32_t text_size;   /* RO bytes at file offset 28, loaded at USER_BASE */
    uint32_t data_size;   /* RW bytes after text, loaded at USER_BASE+text_size */
    uint32_t mem_size;    /* total memory span from USER_BASE; tail is BSS */
    uint32_t flags;       /* must be 0 */
};
```

## Rules the loader enforces (`vbin_load`)

- `magic == VBIN_MAGIC`, `version == 1`, `flags == 0`.
- `28 + text_size + data_size` does not overflow and fits in the file;
  `text_size + data_size <= mem_size`; `mem_size` in `(0, 1 MiB]`.
- The whole span `[USER_BASE, USER_BASE + mem_size)` stays below the user
  stack (`0x1FFFC000`) — no stack collision by construction.
- `entry` is inside the text region.
- If `data_size > 0`, `text_size` is page-aligned, so no page is ever shared
  between the RO and RW regions (a shared page would have to be writable,
  silently de-protecting code).
- Text pages map `PRESENT|USER`, data pages `PRESENT|USER|WRITABLE`.

Any violation returns `-ENOEXEC` and maps nothing; for `exec()` the old
address space is untouched, so the calling process keeps running.

What VBIN deliberately has no room for: dynamic linking, shared libraries,
PIE, ASLR, TLS, relocations, an interpreter. One OS, one architecture, one
ABI, one format.

## The packer (`tools/vbinpack`, host program)

Built with the host compiler (no `-m32`, it runs on the build machine) from
the linked `ET_EXEC` ELF the existing `src/user/user.ld` produces:

1. parse the ELF header and program headers,
2. collect `PT_LOAD` segments into a read-only span and a read-write span
   (holes zero-filled; entry must have file backing, not land in a hole),
3. require the RO span to start at `USER_BASE` and the RW span to start
   page-aligned where the RO span ends,
4. write the VBIN header plus the two blobs.

It rejects anything it does not understand (wrong class/machine/type,
unexpected flags, overlapping spans, entry outside text). A rejected program
fails the build instead of becoming a kernel bug report at runtime.

## Limits

- One RW span: programs have a single data/BSS region (true for everything we
  ship; the packer errors otherwise).
- 1 MiB maximum image span; 60 KiB VibeFS file cap applies on top.
- No execute protection beyond R/W: 32-bit paging without PAE has no NX bit,
  so `PF_X`-style distinctions are parsed nowhere — code is non-writable,
  which is the half we can enforce.
