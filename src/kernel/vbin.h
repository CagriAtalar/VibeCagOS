/*
 * VibeCagOS - VBIN runtime loader (the ONE executable loader, docs/VBIN.md).
 *
 * A VBIN image is two flat blobs: read-only text at USER_BASE and read-write
 * data right after it, with zero BSS up to mem_size. Loading is two passes
 * because text pages are mapped WRITABLE while the file bytes are copied in
 * and only afterwards demoted to read-only (x86-32 without PAE has no NX
 * bit, but code must at least be non-writable):
 *
 *   pass 1  validate the header, map every page USER|WRITABLE, copy the file
 *           bytes that exist (text_size + data_size); BSS pages are mapped
 *           and left alone because PMM frames arrive zeroed
 *   pass 2  clear the WRITE bit on every text page
 *
 * The regions are disjoint by construction (text_size must be page-aligned
 * whenever data exists), so no page is ever mapped twice and no bitmap is
 * needed. Anything the header promises that the file does not contain is a
 * rejection, never a read past the image.
 *
 * The page directory handed in must NOT be the active CR3: pass 2 edits PTEs
 * and there is no TLB shootdown for a directory we are not using.
 */
#pragma once
#include "common.h"

#define VBIN_MAGIC   0x4E494256u   /* 'VBIN', little endian */
#define VBIN_VERSION 1u

struct vbin_header {
    uint32_t magic;       /* VBIN_MAGIC */
    uint32_t version;     /* VBIN_VERSION */
    uint32_t entry;       /* absolute virtual address of _start */
    uint32_t text_size;   /* RO bytes at file offset 28, loaded at USER_BASE */
    uint32_t data_size;   /* RW bytes after text, loaded at USER_BASE+text_size */
    uint32_t mem_size;    /* total memory span from USER_BASE; tail is BSS */
    uint32_t flags;       /* must be 0 */
} __attribute__((packed));

/* Largest image span we will map (1 MiB of user address space). */
#define VBIN_MAX_SPAN (1u << 20)

/* Load a VBIN image into the page directory `pd`. On success stores the entry
 * point in *entry and returns 0; on failure returns a negative errno and maps
 * nothing the caller must unwind (the caller destroys the address space). */
int vbin_load(uint32_t *pd, const void *image, size_t size, uint32_t *entry);

/* Human readable reason for the last vbin_load() failure (for logs). */
const char *vbin_error(void);