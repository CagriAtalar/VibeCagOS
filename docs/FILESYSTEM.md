# VibeCagOS — VibeFS

> **Status:** implemented. This document describes the on-disk format and the
> block-map code as they are. It superseded the v1 (direct-blocks-only) layout;
> v1 images are reformatted on mount.

VibeFS is the persistent hierarchical filesystem, reached from userspace only
through the VFS and the syscall boundary (`open`/`read`/`write`/`lseek`/
`truncate`/`mkdir`/`unlink`/... in `src/abi/syscall.h`). Userspace never sees a
`vibefs_*` symbol.

## On-disk layout (512-byte sectors)

```
sector 0            superblock
sectors 1 .. 128    inode table (VIBEFS_MAX_INODES = 128, one inode per sector)
sectors 129 ..      data blocks
```

The disk image is 16 MiB (`VIBEFS_TOTAL_SECTORS = 32768`), so there are
`32639` data sectors. `Makefile`'s `disk.img` target recreates the image when
its size does not match, so filesystem code never writes past the file.

## Inode = one sector

A `struct vibefs_inode` is padded to exactly `VIBEFS_BLOCK_SIZE`, so exactly one
inode fits per sector and the inode table I/O is a flat sector array. The block
map uses the 120 `u32` slots left after the scalar fields:

```
blocks[0   .. 117]   118 direct data blocks
blocks[118]          single-indirect block  (128 pointers)
blocks[119]          double-indirect block  (128 * 128 pointers)
```

```
max file size = (118 + 128 + 128*128) * 512 = 8,514,560 bytes  (~8 MiB)
```

Directories only ever use the direct range: they stop scanning at the first
zero pointer and never allocate an indirect block, so a directory tops out at
118 blocks = 944 entries. Files use all three levels.

## Block map (`vibefs_bmap`)

`vibefs_bmap(inode, logical_block, alloc, &is_new)` resolves a file-relative
block index to a disk sector:

* allocates the data block, and any indirect block needed to reach it, when
  `alloc` is set; returns 0 when the block is absent (`alloc` off) or on ENOSPC;
* reports via `is_new` whether the data block was freshly allocated, so the
  writer knows the block need not be read (a fresh partial block is zeroed, a
  fresh whole-block write is filled outright, and an existing block is read).

New indirect blocks are zeroed on allocation so an unread pointer reads as 0.

## Free-space tracking

`struct vibefs_state.block_bitmap` is a one-bit-per-data-sector set. It is:

* cleared on format and filled as the root directory allocates its blocks;
* **rebuilt once at mount** by walking every live inode's direct and indirect
  blocks (`mark_inode_blocks`);
* updated incrementally by `vibefs_alloc_block` / `vibefs_free_block`.

The old code rebuilt the whole bitmap on *every* allocation, which does not
scale once a file has thousands of blocks. At mount the free-block count is
also recomputed from the bitmap, so `sb.free_blocks` self-heals after an
unclean shutdown.

## Truncate

`SYS_TRUNCATE(path, size)` (and `O_TRUNC`) reaches `vibefs_vfs_truncate`, which
converts the new size to a kept-block count and calls `truncate_blocks`:

* frees direct blocks at/after the cut;
* frees single-indirect entries at/after the cut, dropping the indirect block
  when it becomes empty;
* does the same one level deeper for the double-indirect block.

The prefix below the new size is untouched, so data survives a shrink.

## Reliability notes

* Not journaled and not crash-safe: multi-step operations can leave a partially
  updated filesystem if the machine dies mid-write.
* No permission enforcement: `mode` is stored but not checked (audit #8).
* `nlink` is always 1; there are no timestamps; there are no hard links.
* Block writes go straight to the PIO IDE driver (no cache, no write-back
  queue), so a successful write is on disk.

## Tested

`make test-fs` runs `utest 28`, which writes a 1.5 MiB file (past the direct and
single-indirect ranges, deep into the double-indirect region), reads it back
byte-for-byte, overwrites 32 bytes deep in the double-indirect region without
changing the size, truncates to 200 KiB and verifies the surviving prefix and
EOF, then unlinks. `utest 15` covers create/read/write/append/seek/stat/
mkdir/rmdir/unlink on small files.
