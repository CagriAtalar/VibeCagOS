#pragma once
#include "common.h"
#include "vfs.h"

/*
 * VibeCagOS — VibeFS: Hierarchical Disk Filesystem
 *
 * An inode-based filesystem with proper directory support.
 * Replaces the flat SimpleFS namespace with a real directory tree.
 *
 * Disk Layout (512-byte sectors):
 *
 *   Sector 0          : Superblock
 *   Sectors 1–N       : Inode table  (VIBEFS_MAX_INODES inodes)
 *   Sectors N+1–M     : Data blocks  (fixed-size 512-byte blocks)
 *
 * Inode structure:
 *
 *   - Regular file: blocks[] point to data sectors
 *   - Directory: blocks[] point to sectors full of vibefs_dirent structs
 *
 * Directory entry format:
 *
 *   struct vibefs_dirent {
 *       uint32_t  ino;       // 0 = free slot
 *       uint8_t   type;      // VFS_TYPE_*
 *       char      name[55];  // null-terminated
 *   };  // = 64 bytes, 8 per sector
 *
 * Root inode number: VIBEFS_ROOT_INO (= 1)
 *
 * Features:
 *   - Arbitrary directory depth
 *   - Up to VIBEFS_MAX_INODES files/directories total
 *   - Direct blocks only (VIBEFS_INODE_BLOCKS * 512 bytes per file)
 *   - Persistent via IDE disk
 *
 * Known limitations:
 *   - No journaling (crash unsafe during multi-step operations)
 *   - No hard links (nlink always 1)
 *   - No timestamps
 *   - No permissions enforcement (mode stored but not checked)
 *   - Maximum file size: VIBEFS_INODE_BLOCKS * 512 bytes
 */

/* =========================================================================
 * Constants
 * ========================================================================= */

#define VIBEFS_MAGIC           0x56494245u   /* "VIBE" */
#define VIBEFS_VERSION         1u

#define VIBEFS_BLOCK_SIZE      512u          /* Must match sector size */
#define VIBEFS_MAX_INODES      128u          /* Inode table capacity */
#define VIBEFS_ROOT_INO        1u            /* Root directory inode */

/*
 * Direct block pointers per inode. The inode struct is padded to exactly one
 * sector (see struct vibefs_inode) so INODES_PER_SECTOR is 1 and a sector
 * never straddles two inodes - that keeps the inode table I/O trivial.
 *
 *   max file size = VIBEFS_INODE_BLOCKS * 512 = 120 * 512 = 60 KiB
 *
 * 60 KiB is what makes ELF user programs storable: sh.elf is ~17 KiB and
 * utest.elf ~29 KiB, and both have to fit in a single file for exec() to be
 * able to read them in one go.
 */
#define VIBEFS_INODE_BLOCKS    120u
#define VIBEFS_MAX_FILENAME    55u           /* Bytes, excludes null terminator */
#define VIBEFS_DIRENT_SIZE     64u           /* Must be power of two */
#define VIBEFS_DIRENTS_PER_BLK (VIBEFS_BLOCK_SIZE / VIBEFS_DIRENT_SIZE)  /* 8 */

/* Derived layout offsets */
/* Number of sectors the inode table uses */
#define VIBEFS_INODE_SECTORS \
    (((VIBEFS_MAX_INODES * sizeof(struct vibefs_inode)) + VIBEFS_BLOCK_SIZE - 1) \
     / VIBEFS_BLOCK_SIZE)

/* First data sector */
#define VIBEFS_DATA_START (1u + VIBEFS_INODE_SECTORS)

/* The disk image is 2 MiB = 4096 sectors (see Makefile's disk.img target). */
#define VIBEFS_TOTAL_SECTORS   4096u

/* Everything after the superblock and the inode table is data. Derived, not
 * hard-coded: the inode table grew when VIBEFS_INODE_BLOCKS grew, and a stale
 * constant here would hand the allocator sectors that overlap the table. */
#define VIBEFS_DATA_SECTORS \
    (VIBEFS_TOTAL_SECTORS - VIBEFS_DATA_START)

/* =========================================================================
 * On-disk structures
 * ========================================================================= */

/* Superblock — sector 0 */
struct vibefs_superblock {
    uint32_t magic;
    uint32_t version;
    uint32_t total_sectors;
    uint32_t inode_start;     /* Always 1 */
    uint32_t inode_sectors;   /* VIBEFS_INODE_SECTORS */
    uint32_t data_start;      /* VIBEFS_DATA_START */
    uint32_t data_sectors;    /* VIBEFS_DATA_SECTORS */
    uint32_t free_inodes;
    uint32_t free_blocks;
    uint32_t root_ino;        /* Always VIBEFS_ROOT_INO */
    uint8_t  padding[VIBEFS_BLOCK_SIZE - 40];
} __attribute__((packed));

/*
 * Inode — file/directory metadata, padded to exactly VIBEFS_BLOCK_SIZE so
 * one inode occupies one sector. See VIBEFS_INODE_BLOCKS for why.
 */
struct vibefs_inode {
    uint32_t ino;              /* Inode number (0 = free) */
    uint8_t  type;             /* VFS_TYPE_* */
    uint16_t mode;             /* Permission bits */
    uint32_t size;             /* Bytes for files, entries count for dirs */
    uint32_t nlink;            /* Hard link count */
    uint32_t blocks[VIBEFS_INODE_BLOCKS];  /* Sector numbers (0 = unused) */
    /* 15 bytes of scalar fields (ino/type/mode/size/nlink) + the block array
     * must be padded out to exactly one sector. */
    uint8_t  padding[VIBEFS_BLOCK_SIZE - 15 - 4 * VIBEFS_INODE_BLOCKS];
} __attribute__((packed));

/* Directory entry — stored in data blocks of a directory inode */
struct vibefs_dirent {
    uint32_t ino;              /* 0 = free slot */
    uint8_t  type;             /* VFS_TYPE_* */
    char     name[VIBEFS_MAX_FILENAME]; /* null-terminated */
    uint8_t  padding[4];       /* pad to 64 bytes */
} __attribute__((packed));

/* Compile-time size checks */
/* sizeof(vibefs_dirent) must equal VIBEFS_DIRENT_SIZE */

/* =========================================================================
 * In-memory filesystem state
 * ========================================================================= */

struct vibefs_state {
    struct vibefs_superblock sb;
    struct vibefs_inode      inodes[VIBEFS_MAX_INODES];
    bool                     mounted;
    bool                     dirty;       /* inodes need flushing */
};

extern struct vibefs_state vibefs;

/* =========================================================================
 * VibeFS public API
 * ========================================================================= */

/*
 * vibefs_format — Format the disk with a fresh VibeFS.
 * Creates root directory at inode VIBEFS_ROOT_INO.
 */
void vibefs_format(void);

/*
 * vibefs_mount — Read the superblock and inode table from disk.
 * Returns 0 on success, -1 if no valid VibeFS found.
 */
int vibefs_mount(void);

/*
 * vibefs_unmount — Flush all dirty state to disk.
 */
void vibefs_unmount(void);

/*
 * vibefs_get_vfs_root — Get the VFS root vnode for the mounted VibeFS.
 * Returns NULL if not mounted.
 */
struct vnode *vibefs_get_vfs_root(void);

/*
 * vibefs_vfs_ops — The VFS operations vtable for VibeFS.
 * Pass this to vfs_mount().
 */
extern const struct fs_ops vibefs_vfs_ops;

/*
 * vibefs_ls — Pretty-print contents of a path (for shell use).
 */
void vibefs_ls(const char *path);

/*
 * vibefs_info — Print superblock info.
 */
void vibefs_info(void);
