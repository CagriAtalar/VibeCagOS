/*
 * VibeCagOS — VibeFS Implementation
 *
 * Hierarchical inode-based filesystem with VFS integration.
 *
 * Data flow:
 *
 *   VFS call (e.g. vfs_mkdir("/home/user"))
 *       |
 *       v
 *   vfs_lookup_parent -> resolve "/home" to vnode
 *       |
 *       v
 *   vibefs_vfs_ops.mkdir(parent_vnode, "user", mode)
 *       |
 *       v
 *   vibefs_inode_alloc() -> gets free inode
 *   vibefs_dir_add_entry(parent, "user", new_ino, VFS_TYPE_DIR)
 *   flush_inode_table()
 *       |
 *       v
 *   return new vnode to VFS
 *
 * Directory data layout:
 *
 *   Each directory inode's blocks[] contain sectors of vibefs_dirent
 *   entries (8 entries per 512-byte sector).  Entry with ino==0 is free.
 *
 *   Entry 0 of the first block: "."  -> self
 *   Entry 1 of the first block: ".." -> parent
 *   Remaining entries: actual directory contents
 *
 * Inode table I/O:
 *   The entire inode table is loaded into RAM on mount.
 *   Any modification writes back only the modified sector.
 *   sync() writes all sectors.
 */

#include "vibefs.h"
#include "vfs.h"
#include "common.h"
#include "vga.h"
#include "kernel/kmalloc.h"
#include "kernel/kernel.h"

/* Global filesystem state */
struct vibefs_state vibefs;

/* Forward declarations */
extern void read_write_disk(void *buf, unsigned sector, int is_write);
static struct vnode *vibefs_make_vnode(uint32_t ino);

/* =========================================================================
 * Compile-time assertions
 * ========================================================================= */

/* Ensure dirent is exactly VIBEFS_DIRENT_SIZE bytes */
typedef char _dirent_size_check[
    (sizeof(struct vibefs_dirent) == VIBEFS_DIRENT_SIZE) ? 1 : -1
];

/* One inode must be exactly one sector: the inode table is read and written
 * a sector at a time by index, so a straddling inode would corrupt its
 * neighbours. */
typedef char _inode_sector_check[
    (sizeof(struct vibefs_inode) == VIBEFS_BLOCK_SIZE) ? 1 : -1
];
typedef char _inode_fits_check[
    (15 + 4 * VIBEFS_INODE_BLOCKS <= VIBEFS_BLOCK_SIZE) ? 1 : -1
];

/* Ensure superblock fits in one sector */
typedef char _sb_size_check[
    (sizeof(struct vibefs_superblock) == VIBEFS_BLOCK_SIZE) ? 1 : -1
];

/* =========================================================================
 * Low-level disk I/O
 * ========================================================================= */

static void disk_read(uint32_t sector, void *buf) {
    read_write_disk(buf, (unsigned)sector, 0);
}

static void disk_write(uint32_t sector, const void *buf) {
    read_write_disk((void *)buf, (unsigned)sector, 1);
}

/* =========================================================================
 * Inode table I/O
 * ========================================================================= */

#define INODES_PER_SECTOR (VIBEFS_BLOCK_SIZE / sizeof(struct vibefs_inode))

static void flush_inode_table(void) {
    for (uint32_t s = 0; s < VIBEFS_INODE_SECTORS; s++) {
        uint32_t first = s * (uint32_t)INODES_PER_SECTOR;
        disk_write(1u + s, &vibefs.inodes[first]);
    }
    vibefs.dirty = false;
}

static void read_inode_table(void) {
    for (uint32_t s = 0; s < VIBEFS_INODE_SECTORS; s++) {
        uint32_t first = s * (uint32_t)INODES_PER_SECTOR;
        disk_read(1u + s, &vibefs.inodes[first]);
    }
}

/* Flush only the sector containing a specific inode */
static void flush_inode(uint32_t ino) {
    if (ino == 0 || ino >= VIBEFS_MAX_INODES) return;
    uint32_t sector_idx = (ino - 1) / (uint32_t)INODES_PER_SECTOR;
    uint32_t first_ino  = sector_idx * (uint32_t)INODES_PER_SECTOR;
    disk_write(1u + sector_idx, &vibefs.inodes[first_ino]);
}

/* =========================================================================
 * Inode allocation / freeing
 * ========================================================================= */

/* Returns pointer to a free inode slot, or NULL if full.
   Assigns inode number (1-based). */
static struct vibefs_inode *vibefs_inode_alloc(void) {
    /* Start from 1 (0 is reserved = "null inode") */
    for (uint32_t i = 1; i < VIBEFS_MAX_INODES; i++) {
        if (vibefs.inodes[i].ino == 0) {
            vibefs.inodes[i].ino   = i;
            vibefs.inodes[i].nlink = 1;
            vibefs.sb.free_inodes--;
            return &vibefs.inodes[i];
        }
    }
    return NULL;  /* Inode table full */
}

static void vibefs_inode_free(uint32_t ino) {
    if (ino == 0 || ino >= VIBEFS_MAX_INODES) return;
    memset(&vibefs.inodes[ino], 0, sizeof(struct vibefs_inode));
    vibefs.sb.free_inodes++;
}

/* =========================================================================
 * Block allocation / freeing
 *
 * The set of used data sectors lives in vibefs.block_bitmap, rebuilt once at
 * mount from the inode table and kept in sync by the two functions below.
 * ========================================================================= */

static inline bool blk_used(uint32_t idx) {
    return (vibefs.block_bitmap[idx >> 3] >> (idx & 7)) & 1u;
}
static inline void blk_mark(uint32_t idx) {
    vibefs.block_bitmap[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}
static inline void blk_unmark(uint32_t idx) {
    vibefs.block_bitmap[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
}
static inline bool blk_in_range(uint32_t blk) {
    return blk >= VIBEFS_DATA_START && (blk - VIBEFS_DATA_START) < VIBEFS_DATA_SECTORS;
}

/* Read the VIBEFS_PTRS_PER_BLOCK pointers held in one indirect block. */
static void read_ptrs(uint32_t sector, uint32_t *out) {
    disk_read(sector, out);
}

/* Mark one inode's direct blocks and every block reachable through its
 * indirect blocks. */
static void mark_inode_blocks(const struct vibefs_inode *inode) {
    const uint32_t per = VIBEFS_PTRS_PER_BLOCK;
    static uint32_t l1[VIBEFS_PTRS_PER_BLOCK];
    static uint32_t l2[VIBEFS_PTRS_PER_BLOCK];

    for (uint32_t j = 0; j < VIBEFS_NDIRECT; j++)
        if (blk_in_range(inode->blocks[j]))
            blk_mark(inode->blocks[j] - VIBEFS_DATA_START);

    if (blk_in_range(inode->blocks[VIBEFS_SIND_INDEX])) {
        uint32_t ind = inode->blocks[VIBEFS_SIND_INDEX];
        blk_mark(ind - VIBEFS_DATA_START);
        read_ptrs(ind, l1);
        for (uint32_t k = 0; k < per; k++)
            if (blk_in_range(l1[k])) blk_mark(l1[k] - VIBEFS_DATA_START);
    }

    if (blk_in_range(inode->blocks[VIBEFS_DIND_INDEX])) {
        uint32_t dind = inode->blocks[VIBEFS_DIND_INDEX];
        blk_mark(dind - VIBEFS_DATA_START);
        read_ptrs(dind, l1);
        for (uint32_t d = 0; d < per; d++) {
            if (!blk_in_range(l1[d])) continue;
            blk_mark(l1[d] - VIBEFS_DATA_START);
            read_ptrs(l1[d], l2);           /* separate buffer: l1 is the loop */
            for (uint32_t e = 0; e < per; e++)
                if (blk_in_range(l2[e])) blk_mark(l2[e] - VIBEFS_DATA_START);
        }
    }
}

/* Rebuild the used-block bitmap and the free-block count from the in-memory
 * inode table. Also self-heals sb.free_blocks after an unclean shutdown. */
static void vibefs_rebuild_bitmap(void) {
    memset(vibefs.block_bitmap, 0, sizeof(vibefs.block_bitmap));
    for (uint32_t i = 1; i < VIBEFS_MAX_INODES; i++)
        if (vibefs.inodes[i].ino != 0)
            mark_inode_blocks(&vibefs.inodes[i]);

    uint32_t used = 0;
    for (uint32_t i = 0; i < VIBEFS_DATA_SECTORS; i++)
        if (blk_used(i)) used++;
    vibefs.sb.free_blocks = VIBEFS_DATA_SECTORS - used;
    vibefs.alloc_hint = 0;
}

/* Reserve a free data block; returns its sector number, or 0 if the disk is
 * full. */
static uint32_t vibefs_alloc_block(void) {
    if (vibefs.sb.free_blocks == 0) return 0;   /* cheap early out */
    for (uint32_t n = 0; n < VIBEFS_DATA_SECTORS; n++) {
        uint32_t idx = (vibefs.alloc_hint + n) % VIBEFS_DATA_SECTORS;
        if (!blk_used(idx)) {
            blk_mark(idx);
            vibefs.sb.free_blocks--;
            vibefs.alloc_hint = (idx + 1 < VIBEFS_DATA_SECTORS) ? idx + 1 : 0;
            return VIBEFS_DATA_START + idx;
        }
    }
    return 0;  /* Disk full */
}

static void vibefs_free_block(uint32_t blk) {
    if (!blk_in_range(blk)) return;
    uint32_t idx = blk - VIBEFS_DATA_START;
    if (blk_used(idx)) {
        blk_unmark(idx);
        vibefs.sb.free_blocks++;
    }
}

/* Free all data blocks belonging to an inode, including indirect metadata. */
static void vibefs_inode_free_blocks(struct vibefs_inode *inode) {
    const uint32_t per = VIBEFS_PTRS_PER_BLOCK;
    static uint32_t l1[VIBEFS_PTRS_PER_BLOCK];
    static uint32_t l2[VIBEFS_PTRS_PER_BLOCK];

    for (uint32_t j = 0; j < VIBEFS_NDIRECT; j++) {
        vibefs_free_block(inode->blocks[j]);
        inode->blocks[j] = 0;
    }
    if (inode->blocks[VIBEFS_SIND_INDEX]) {
        uint32_t ind = inode->blocks[VIBEFS_SIND_INDEX];
        read_ptrs(ind, l1);
        for (uint32_t k = 0; k < per; k++) vibefs_free_block(l1[k]);
        vibefs_free_block(ind);
        inode->blocks[VIBEFS_SIND_INDEX] = 0;
    }
    if (inode->blocks[VIBEFS_DIND_INDEX]) {
        uint32_t dind = inode->blocks[VIBEFS_DIND_INDEX];
        read_ptrs(dind, l1);
        for (uint32_t d = 0; d < per; d++) {
            if (l1[d] == 0) continue;
            read_ptrs(l1[d], l2);
            for (uint32_t e = 0; e < per; e++) vibefs_free_block(l2[e]);
            vibefs_free_block(l1[d]);
        }
        vibefs_free_block(dind);
        inode->blocks[VIBEFS_DIND_INDEX] = 0;
    }
}

/*
 * Map a file's logical block number to a disk sector, allocating the block
 * (and any indirect block that addresses it) when `alloc` is set. Returns 0
 * when the block does not exist and allocation is off, or on ENOSPC. If
 * `is_new` is non-NULL it is set to 1 when the returned data block was just
 * allocated, so the writer knows it need not read stale contents.
 */
static uint32_t vibefs_bmap(struct vibefs_inode *inode, uint32_t lbn,
                            int alloc, int *is_new) {
    const uint32_t per = VIBEFS_PTRS_PER_BLOCK;
    static const uint32_t zero_blk[VIBEFS_PTRS_PER_BLOCK];
    static uint32_t l1[VIBEFS_PTRS_PER_BLOCK];
    static uint32_t l2[VIBEFS_PTRS_PER_BLOCK];
    if (is_new) *is_new = 0;

    /* direct */
    if (lbn < VIBEFS_NDIRECT) {
        uint32_t b = inode->blocks[lbn];
        if (b == 0 && alloc) {
            b = vibefs_alloc_block();
            if (b == 0) return 0;
            inode->blocks[lbn] = b;
            if (is_new) *is_new = 1;
        }
        return b;
    }

    uint32_t i = lbn - VIBEFS_NDIRECT;

    /* single indirect */
    if (i < per) {
        uint32_t ind = inode->blocks[VIBEFS_SIND_INDEX];
        if (ind == 0) {
            if (!alloc) return 0;
            ind = vibefs_alloc_block();
            if (ind == 0) return 0;
            disk_write(ind, zero_blk);
            inode->blocks[VIBEFS_SIND_INDEX] = ind;
        }
        read_ptrs(ind, l1);
        uint32_t b = l1[i];
        if (b == 0 && alloc) {
            b = vibefs_alloc_block();
            if (b == 0) return 0;
            l1[i] = b;
            disk_write(ind, l1);
            if (is_new) *is_new = 1;
        }
        return b;
    }

    i -= per;

    /* double indirect */
    if (i < per * per) {
        uint32_t d = i / per, e = i % per;
        uint32_t dind = inode->blocks[VIBEFS_DIND_INDEX];
        if (dind == 0) {
            if (!alloc) return 0;
            dind = vibefs_alloc_block();
            if (dind == 0) return 0;
            disk_write(dind, zero_blk);
            inode->blocks[VIBEFS_DIND_INDEX] = dind;
        }
        read_ptrs(dind, l1);
        uint32_t child = l1[d];
        if (child == 0) {
            if (!alloc) return 0;
            child = vibefs_alloc_block();
            if (child == 0) return 0;
            disk_write(child, zero_blk);
            l1[d] = child;
            disk_write(dind, l1);
        }
        read_ptrs(child, l2);
        uint32_t b = l2[e];
        if (b == 0 && alloc) {
            b = vibefs_alloc_block();
            if (b == 0) return 0;
            l2[e] = b;
            disk_write(child, l2);
            if (is_new) *is_new = 1;
        }
        return b;
    }

    return 0;   /* beyond VIBEFS_MAX_FILE_SIZE */
}

/* =========================================================================
 * Directory operations
 * ========================================================================= */

/*
 * vibefs_dir_find — scan a directory inode for a named entry.
 * Returns the found dirent ino, or 0 if not found.
 */
static uint32_t vibefs_dir_find(struct vibefs_inode *dir, const char *name,
                                 uint8_t *type_out) {
    static char sector_buf[VIBEFS_BLOCK_SIZE];

    for (int blk = 0; blk < (int)VIBEFS_NDIRECT; blk++) {
        if (dir->blocks[blk] == 0) break;
        disk_read(dir->blocks[blk], sector_buf);

        struct vibefs_dirent *de = (struct vibefs_dirent *)sector_buf;
        for (int i = 0; i < (int)VIBEFS_DIRENTS_PER_BLK; i++) {
            if (de[i].ino != 0 && strcmp(de[i].name, name) == 0) {
                if (type_out) *type_out = de[i].type;
                return de[i].ino;
            }
        }
    }
    return 0;
}

/*
 * vibefs_dir_add_entry — add a new dirent to a directory inode.
 * Returns 0 on success.
 */
static int vibefs_dir_add_entry(struct vibefs_inode *dir, const char *name,
                                 uint32_t ino, uint8_t type) {
    static char sector_buf[VIBEFS_BLOCK_SIZE];

    /* Search for a free slot in existing blocks */
    for (int blk = 0; blk < (int)VIBEFS_NDIRECT; blk++) {
        if (dir->blocks[blk] == 0) {
            /* Need a new block */
            uint32_t new_sector = vibefs_alloc_block();
            if (new_sector == 0) return VFS_ENOSPC;
            dir->blocks[blk] = new_sector;
            /* Zero the new sector */
            memset(sector_buf, 0, sizeof(sector_buf));
            disk_write(new_sector, sector_buf);
        }

        disk_read(dir->blocks[blk], sector_buf);
        struct vibefs_dirent *de = (struct vibefs_dirent *)sector_buf;

        for (int i = 0; i < (int)VIBEFS_DIRENTS_PER_BLK; i++) {
            if (de[i].ino == 0) {
                /* Free slot found */
                de[i].ino  = ino;
                de[i].type = type;
                strncpy(de[i].name, name, VIBEFS_MAX_FILENAME);
                de[i].name[VIBEFS_MAX_FILENAME - 1] = '\0';
                disk_write(dir->blocks[blk], sector_buf);
                dir->size++;
                return 0;
            }
        }
    }
    return VFS_ENOSPC;
}

/*
 * vibefs_dir_remove_entry — remove a named entry from a directory inode.
 * Returns 0 on success, VFS_ENOENT if not found.
 */
static int vibefs_dir_remove_entry(struct vibefs_inode *dir, const char *name) {
    static char sector_buf[VIBEFS_BLOCK_SIZE];

    for (int blk = 0; blk < (int)VIBEFS_NDIRECT; blk++) {
        if (dir->blocks[blk] == 0) break;
        disk_read(dir->blocks[blk], sector_buf);

        struct vibefs_dirent *de = (struct vibefs_dirent *)sector_buf;
        for (int i = 0; i < (int)VIBEFS_DIRENTS_PER_BLK; i++) {
            if (de[i].ino != 0 && strcmp(de[i].name, name) == 0) {
                de[i].ino  = 0;
                de[i].name[0] = '\0';
                disk_write(dir->blocks[blk], sector_buf);
                if (dir->size > 0) dir->size--;
                return 0;
            }
        }
    }
    return VFS_ENOENT;
}

/*
 * vibefs_dir_is_empty — check if a directory has no entries except "." and ".."
 */
static bool vibefs_dir_is_empty(struct vibefs_inode *dir) {
    static char sector_buf[VIBEFS_BLOCK_SIZE];

    for (int blk = 0; blk < (int)VIBEFS_NDIRECT; blk++) {
        if (dir->blocks[blk] == 0) break;
        disk_read(dir->blocks[blk], sector_buf);

        struct vibefs_dirent *de = (struct vibefs_dirent *)sector_buf;
        for (int i = 0; i < (int)VIBEFS_DIRENTS_PER_BLK; i++) {
            if (de[i].ino == 0) continue;
            if (strcmp(de[i].name, ".") == 0) continue;
            if (strcmp(de[i].name, "..") == 0) continue;
            return false;  /* Non-empty */
        }
    }
    return true;
}

/* =========================================================================
 * Create root directory
 * ========================================================================= */

static void vibefs_create_root(void) {
    struct vibefs_inode *root = &vibefs.inodes[VIBEFS_ROOT_INO];
    memset(root, 0, sizeof(*root));
    root->ino   = VIBEFS_ROOT_INO;
    root->type  = VFS_TYPE_DIR;
    root->mode  = VFS_PERM_DEFAULT_DIR;
    root->nlink = 2;  /* "." and from parent */
    root->size  = 0;

    /* Add "." and ".." entries */
    vibefs_dir_add_entry(root, ".", VIBEFS_ROOT_INO, VFS_TYPE_DIR);
    vibefs_dir_add_entry(root, "..", VIBEFS_ROOT_INO, VFS_TYPE_DIR);
}

/* =========================================================================
 * vibefs_format / vibefs_mount / vibefs_unmount
 * ========================================================================= */

void vibefs_format(void) {
    printf("VibeFS: Formatting...\n");

    /* Build superblock */
    memset(&vibefs.sb, 0, sizeof(vibefs.sb));
    vibefs.sb.magic        = VIBEFS_MAGIC;
    vibefs.sb.version      = VIBEFS_VERSION;
    vibefs.sb.total_sectors= VIBEFS_DATA_START + VIBEFS_DATA_SECTORS;
    vibefs.sb.inode_start  = 1;
    vibefs.sb.inode_sectors= VIBEFS_INODE_SECTORS;
    vibefs.sb.data_start   = VIBEFS_DATA_START;
    vibefs.sb.data_sectors = VIBEFS_DATA_SECTORS;
    vibefs.sb.free_inodes  = VIBEFS_MAX_INODES - 1; /* minus null inode */
    vibefs.sb.free_blocks  = VIBEFS_DATA_SECTORS;
    vibefs.sb.root_ino     = VIBEFS_ROOT_INO;

    disk_write(0, &vibefs.sb);

    /* Zero inode table and the in-memory block bitmap */
    memset(vibefs.inodes, 0, sizeof(vibefs.inodes));
    memset(vibefs.block_bitmap, 0, sizeof(vibefs.block_bitmap));
    vibefs.alloc_hint = 0;

    /* Create root directory */
    vibefs_create_root();
    vibefs.sb.free_inodes--;  /* Root consumed one */

    flush_inode_table();
    disk_write(0, &vibefs.sb);

    vibefs.mounted = true;
    vibefs.dirty   = false;

    printf("VibeFS: Format complete.\n");
    printf("  Max inodes   : %u\n", VIBEFS_MAX_INODES);
    printf("  Data sectors : %u\n", VIBEFS_DATA_SECTORS);
    printf("  Max file size: %u bytes\n", (unsigned)VIBEFS_MAX_FILE_SIZE);
}

int vibefs_mount(void) {
    printf("VibeFS: Mounting...\n");

    disk_read(0, &vibefs.sb);

    if (vibefs.sb.magic != VIBEFS_MAGIC) {
        printf("VibeFS: Invalid magic (0x%x). Formatting...\n",
               vibefs.sb.magic);
        vibefs_format();
        return 0;
    }

    if (vibefs.sb.version != VIBEFS_VERSION) {
        /* The on-disk inode layout changed between versions; a v1 image is not
         * safe to read with v2 block-map code, so start fresh. */
        printf("VibeFS: Version %u != %u, reformatting\n",
               vibefs.sb.version, VIBEFS_VERSION);
        vibefs_format();
        return 0;
    }

    read_inode_table();
    vibefs_rebuild_bitmap();
    vibefs.mounted = true;
    vibefs.dirty   = false;

    printf("VibeFS: Mounted OK (free_inodes=%u, free_blocks=%u)\n",
           vibefs.sb.free_inodes, vibefs.sb.free_blocks);
    return 0;
}

void vibefs_unmount(void) {
    if (!vibefs.mounted) return;
    if (vibefs.dirty) flush_inode_table();
    disk_write(0, &vibefs.sb);
    vibefs.mounted = false;
}

/* =========================================================================
 * VFS operations vtable implementation
 * ========================================================================= */

static struct vnode *vibefs_vfs_lookup(struct vnode *dir, const char *name) {
    if (!vibefs.mounted) return NULL;

    struct vibefs_inode *dir_inode = &vibefs.inodes[dir->ino];
    if (dir_inode->type != VFS_TYPE_DIR) return NULL;

    uint8_t type;
    uint32_t child_ino = vibefs_dir_find(dir_inode, name, &type);
    if (child_ino == 0) return NULL;

    return vibefs_make_vnode(child_ino);
}

static int vibefs_vfs_create(struct vnode *dir, const char *name,
                              uint16_t mode, struct vnode **out) {
    if (!vibefs.mounted) return VFS_EIO;
    if (strlen(name) >= VIBEFS_MAX_FILENAME) return VFS_ENAMETOOLONG;

    struct vibefs_inode *dir_inode = &vibefs.inodes[dir->ino];

    /* Check name doesn't already exist */
    if (vibefs_dir_find(dir_inode, name, NULL) != 0)
        return VFS_EEXIST;

    struct vibefs_inode *new_inode = vibefs_inode_alloc();
    if (!new_inode) return VFS_ENOSPC;

    new_inode->type = VFS_TYPE_REG;
    new_inode->mode = mode ? mode : VFS_PERM_DEFAULT_FILE;
    new_inode->size = 0;

    int r = vibefs_dir_add_entry(dir_inode, name, new_inode->ino, VFS_TYPE_REG);
    if (r != 0) {
        vibefs_inode_free(new_inode->ino);
        return r;
    }

    flush_inode(new_inode->ino);
    flush_inode(dir->ino);
    disk_write(0, &vibefs.sb);

    if (out) *out = vibefs_make_vnode(new_inode->ino);
    return VFS_OK;
}

static int vibefs_vfs_mkdir(struct vnode *dir, const char *name,
                             uint16_t mode, struct vnode **out) {
    if (!vibefs.mounted) return VFS_EIO;
    if (strlen(name) >= VIBEFS_MAX_FILENAME) return VFS_ENAMETOOLONG;

    struct vibefs_inode *dir_inode = &vibefs.inodes[dir->ino];

    if (vibefs_dir_find(dir_inode, name, NULL) != 0)
        return VFS_EEXIST;

    struct vibefs_inode *new_inode = vibefs_inode_alloc();
    if (!new_inode) return VFS_ENOSPC;

    new_inode->type  = VFS_TYPE_DIR;
    new_inode->mode  = mode ? mode : VFS_PERM_DEFAULT_DIR;
    new_inode->nlink = 2;
    new_inode->size  = 0;

    /* Add "." and ".." */
    vibefs_dir_add_entry(new_inode, ".", new_inode->ino, VFS_TYPE_DIR);
    vibefs_dir_add_entry(new_inode, "..", dir->ino, VFS_TYPE_DIR);

    /* Add entry in parent */
    int r = vibefs_dir_add_entry(dir_inode, name, new_inode->ino, VFS_TYPE_DIR);
    if (r != 0) {
        vibefs_inode_free_blocks(new_inode);
        vibefs_inode_free(new_inode->ino);
        return r;
    }

    dir_inode->nlink++;  /* Parent gains ".." from child */

    flush_inode(new_inode->ino);
    flush_inode(dir->ino);
    disk_write(0, &vibefs.sb);

    if (out) *out = vibefs_make_vnode(new_inode->ino);
    return VFS_OK;
}

static int vibefs_vfs_unlink(struct vnode *dir, const char *name) {
    if (!vibefs.mounted) return VFS_EIO;

    struct vibefs_inode *dir_inode = &vibefs.inodes[dir->ino];

    uint8_t type;
    uint32_t ino = vibefs_dir_find(dir_inode, name, &type);
    if (ino == 0) return VFS_ENOENT;

    if (type == VFS_TYPE_DIR) return VFS_EISDIR;

    /* Remove directory entry */
    int r = vibefs_dir_remove_entry(dir_inode, name);
    if (r != 0) return r;

    /* Decrement link count; free if zero */
    struct vibefs_inode *inode = &vibefs.inodes[ino];
    if (inode->nlink > 0) inode->nlink--;
    if (inode->nlink == 0) {
        vibefs_inode_free_blocks(inode);
        vibefs_inode_free(ino);
    }

    flush_inode(dir->ino);
    if (ino < VIBEFS_MAX_INODES) flush_inode(ino);
    disk_write(0, &vibefs.sb);
    return VFS_OK;
}

static int vibefs_vfs_rmdir(struct vnode *dir, const char *name) {
    if (!vibefs.mounted) return VFS_EIO;

    struct vibefs_inode *dir_inode = &vibefs.inodes[dir->ino];

    uint8_t type;
    uint32_t ino = vibefs_dir_find(dir_inode, name, &type);
    if (ino == 0) return VFS_ENOENT;
    if (type != VFS_TYPE_DIR) return VFS_ENOTDIR;

    struct vibefs_inode *child_inode = &vibefs.inodes[ino];
    if (!vibefs_dir_is_empty(child_inode)) return VFS_ENOTEMPTY;

    /* Remove from parent */
    int r = vibefs_dir_remove_entry(dir_inode, name);
    if (r != 0) return r;

    if (dir_inode->nlink > 0) dir_inode->nlink--;

    /* Free child's blocks and inode */
    vibefs_inode_free_blocks(child_inode);
    vibefs_inode_free(ino);

    flush_inode(dir->ino);
    flush_inode(ino);
    disk_write(0, &vibefs.sb);
    return VFS_OK;
}

static int vibefs_vfs_read(struct vnode *vn, void *buf, size_t len,
                            uint32_t off) {
    if (!vibefs.mounted) return VFS_EIO;

    struct vibefs_inode *inode = &vibefs.inodes[vn->ino];
    if (inode->type == VFS_TYPE_DIR) return VFS_EISDIR;

    if (off >= inode->size) return 0;

    size_t to_read = inode->size - off;
    if (to_read > len) to_read = len;

    size_t  done = 0;
    uint32_t cur_off = off;
    static char blk_buf[VIBEFS_BLOCK_SIZE];

    while (done < to_read) {
        uint32_t blk_idx = cur_off / VIBEFS_BLOCK_SIZE;
        uint32_t blk_off = cur_off % VIBEFS_BLOCK_SIZE;

        uint32_t blk = vibefs_bmap(inode, blk_idx, 0, NULL);
        if (blk == 0) break;

        disk_read(blk, blk_buf);

        size_t chunk = VIBEFS_BLOCK_SIZE - blk_off;
        if (chunk > to_read - done) chunk = to_read - done;

        memcpy((uint8_t *)buf + done, blk_buf + blk_off, chunk);
        done    += chunk;
        cur_off += (uint32_t)chunk;
    }

    return (int)done;
}

static int vibefs_vfs_write(struct vnode *vn, const void *buf, size_t len,
                             uint32_t off) {
    if (!vibefs.mounted) return VFS_EIO;

    struct vibefs_inode *inode = &vibefs.inodes[vn->ino];
    if (inode->type == VFS_TYPE_DIR) return VFS_EISDIR;

    /* Check capacity */
    uint32_t max_size = VIBEFS_MAX_FILE_SIZE;
    if (off >= max_size) return VFS_ENOSPC;
    if (off + (uint32_t)len > max_size) len = max_size - off;

    size_t  done = 0;
    uint32_t cur_off = off;
    static char blk_buf[VIBEFS_BLOCK_SIZE];

    while (done < len) {
        uint32_t blk_idx = cur_off / VIBEFS_BLOCK_SIZE;
        uint32_t blk_off = cur_off % VIBEFS_BLOCK_SIZE;

        size_t chunk = VIBEFS_BLOCK_SIZE - blk_off;
        if (chunk > len - done) chunk = len - done;

        int is_new = 0;
        uint32_t blk = vibefs_bmap(inode, blk_idx, 1, &is_new);
        if (blk == 0) break;

        /* A whole-block overwrite needs neither a read nor a zero-fill. A
         * freshly allocated partial block must start from zeros so its tail
         * does not expose another file's old data. */
        int full = (blk_off == 0 && chunk == VIBEFS_BLOCK_SIZE);
        if (!full) {
            if (is_new) memset(blk_buf, 0, sizeof(blk_buf));
            else        disk_read(blk, blk_buf);
        }

        memcpy(blk_buf + blk_off, (const uint8_t *)buf + done, chunk);
        disk_write(blk, blk_buf);

        done    += chunk;
        cur_off += (uint32_t)chunk;
    }

    /* Update size */
    if (off + (uint32_t)done > inode->size) {
        inode->size = off + (uint32_t)done;
        vn->size    = inode->size;
    }

    flush_inode(vn->ino);
    disk_write(0, &vibefs.sb);
    return (int)done;
}

static int vibefs_vfs_readdir(struct vnode *dir, struct dirent *entries,
                               int max, uint32_t *pos) {
    if (!vibefs.mounted) return VFS_EIO;

    struct vibefs_inode *inode = &vibefs.inodes[dir->ino];
    if (inode->type != VFS_TYPE_DIR) return VFS_ENOTDIR;

    int count = 0;
    uint32_t entry_idx = *pos;  /* Global entry index across all blocks */
    static char blk_buf[VIBEFS_BLOCK_SIZE];

    for (int blk = 0; blk < (int)VIBEFS_NDIRECT && count < max; blk++) {
        if (inode->blocks[blk] == 0) break;
        disk_read(inode->blocks[blk], blk_buf);

        struct vibefs_dirent *de = (struct vibefs_dirent *)blk_buf;
        for (int i = 0; i < (int)VIBEFS_DIRENTS_PER_BLK && count < max; i++) {
            uint32_t cur_idx = (uint32_t)blk * VIBEFS_DIRENTS_PER_BLK + (uint32_t)i;
            if (cur_idx < entry_idx) continue;  /* Skip already-returned entries */

            if (de[i].ino == 0) {
                entry_idx = cur_idx + 1;
                continue;
            }

            entries[count].ino  = de[i].ino;
            entries[count].type = de[i].type;
            strncpy(entries[count].name, de[i].name, VFS_NAME_MAX);
            entries[count].name[VFS_NAME_MAX] = '\0';
            count++;
            entry_idx = cur_idx + 1;
        }
    }

    *pos = entry_idx;
    return count;
}

static int vibefs_vfs_stat(struct vnode *vn, struct vstat *st) {
    if (!vibefs.mounted) return VFS_EIO;

    struct vibefs_inode *inode = &vibefs.inodes[vn->ino];
    st->ino   = inode->ino;
    st->type  = inode->type;
    st->mode  = inode->mode;
    st->size  = inode->size;
    st->nlink = inode->nlink;
    return VFS_OK;
}

/*
 * truncate_blocks — free every block at or beyond logical block `keep`,
 * walking the direct, single-indirect and double-indirect levels and dropping
 * indirect metadata that becomes empty.
 */
static void truncate_blocks(struct vibefs_inode *inode, uint32_t keep) {
    const uint32_t per = VIBEFS_PTRS_PER_BLOCK;
    static uint32_t l1[VIBEFS_PTRS_PER_BLOCK];
    static uint32_t l2[VIBEFS_PTRS_PER_BLOCK];

    /* direct */
    uint32_t from = keep < VIBEFS_NDIRECT ? keep : VIBEFS_NDIRECT;
    for (uint32_t j = from; j < VIBEFS_NDIRECT; j++) {
        vibefs_free_block(inode->blocks[j]);
        inode->blocks[j] = 0;
    }

    /* single indirect */
    if (inode->blocks[VIBEFS_SIND_INDEX]) {
        uint32_t ind = inode->blocks[VIBEFS_SIND_INDEX];
        read_ptrs(ind, l1);
        /* Index within the single-indirect block of the first block to free.
         * It covers logical blocks [NDIRECT, NDIRECT+per); first==0 means all
         * of it is beyond `keep`, first>=per means none of it is. */
        uint32_t first = keep > VIBEFS_NDIRECT ? keep - VIBEFS_NDIRECT : 0;
        if (first == 0) {
            for (uint32_t k = 0; k < per; k++) vibefs_free_block(l1[k]);
            vibefs_free_block(ind);
            inode->blocks[VIBEFS_SIND_INDEX] = 0;
        } else if (first < per) {
            bool changed = false;
            for (uint32_t k = first; k < per; k++)
                if (l1[k]) { vibefs_free_block(l1[k]); l1[k] = 0; changed = true; }
            if (changed) disk_write(ind, l1);
        }
        /* first >= per: everything in the single-indirect block is kept */
    }

    /* double indirect */
    if (inode->blocks[VIBEFS_DIND_INDEX]) {
        uint32_t dind = inode->blocks[VIBEFS_DIND_INDEX];
        read_ptrs(dind, l1);
        uint32_t base = VIBEFS_NDIRECT + per;   /* first logical block of dind */
        if (keep <= base) {
            for (uint32_t d = 0; d < per; d++) {
                if (l1[d] == 0) continue;
                read_ptrs(l1[d], l2);
                for (uint32_t e = 0; e < per; e++) vibefs_free_block(l2[e]);
                vibefs_free_block(l1[d]);
            }
            vibefs_free_block(dind);
            inode->blocks[VIBEFS_DIND_INDEX] = 0;
        } else {
            uint32_t off = keep - base;          /* first double index to free */
            uint32_t d0 = off / per, e0 = off % per;
            bool changed = false;
            for (uint32_t d = d0; d < per; d++) {
                if (l1[d] == 0) continue;
                uint32_t child = l1[d];
                read_ptrs(child, l2);
                uint32_t first = (d == d0) ? e0 : 0;
                for (uint32_t e = first; e < per; e++)
                    if (l2[e]) { vibefs_free_block(l2[e]); l2[e] = 0; }
                if (first == 0) {                /* the whole child is gone */
                    vibefs_free_block(child);
                    l1[d] = 0;
                } else {
                    disk_write(child, l2);
                }
                changed = true;
            }
            if (changed) disk_write(dind, l1);
        }
    }
}

static int vibefs_vfs_truncate(struct vnode *vn, uint32_t size) {
    if (!vibefs.mounted) return VFS_EIO;

    struct vibefs_inode *inode = &vibefs.inodes[vn->ino];
    if (inode->type == VFS_TYPE_DIR) return VFS_EISDIR;

    if (size > VIBEFS_MAX_FILE_SIZE) return VFS_EINVAL;

    uint32_t keep = size == 0 ? 0 : (size - 1) / VIBEFS_BLOCK_SIZE + 1;
    truncate_blocks(inode, keep);

    inode->size = size;
    vn->size    = size;
    flush_inode(vn->ino);
    disk_write(0, &vibefs.sb);
    return VFS_OK;
}

static int vibefs_vfs_rename(struct vnode *old_dir, const char *old_name,
                              struct vnode *new_dir, const char *new_name) {
    if (!vibefs.mounted) return VFS_EIO;

    struct vibefs_inode *old_dir_inode = &vibefs.inodes[old_dir->ino];
    struct vibefs_inode *new_dir_inode = &vibefs.inodes[new_dir->ino];

    uint8_t type;
    uint32_t ino = vibefs_dir_find(old_dir_inode, old_name, &type);
    if (ino == 0) return VFS_ENOENT;

    /* If destination exists, remove it first (simple: only for regular files) */
    uint8_t dst_type;
    uint32_t dst_ino = vibefs_dir_find(new_dir_inode, new_name, &dst_type);
    if (dst_ino != 0) {
        if (dst_type == VFS_TYPE_DIR) return VFS_EEXIST;
        vibefs_dir_remove_entry(new_dir_inode, new_name);
        /* Decrement link count */
        if (dst_ino < VIBEFS_MAX_INODES) {
            struct vibefs_inode *dst_inode = &vibefs.inodes[dst_ino];
            if (dst_inode->nlink > 0) dst_inode->nlink--;
            if (dst_inode->nlink == 0) {
                vibefs_inode_free_blocks(dst_inode);
                vibefs_inode_free(dst_ino);
            }
        }
    }

    /* Add new entry in destination directory */
    int r = vibefs_dir_add_entry(new_dir_inode, new_name, ino, type);
    if (r != 0) return r;

    /* Remove old entry */
    vibefs_dir_remove_entry(old_dir_inode, old_name);

    flush_inode(old_dir->ino);
    flush_inode(new_dir->ino);
    return VFS_OK;
}

static void vibefs_vfs_sync(struct mount *mnt) {
    (void)mnt;
    flush_inode_table();
    disk_write(0, &vibefs.sb);
}

/* =========================================================================
 * VFS operations vtable
 * ========================================================================= */

const struct fs_ops vibefs_vfs_ops = {
    .lookup   = vibefs_vfs_lookup,
    .create   = vibefs_vfs_create,
    .mkdir    = vibefs_vfs_mkdir,
    .unlink   = vibefs_vfs_unlink,
    .rmdir    = vibefs_vfs_rmdir,
    .read     = vibefs_vfs_read,
    .write    = vibefs_vfs_write,
    .readdir  = vibefs_vfs_readdir,
    .stat     = vibefs_vfs_stat,
    .truncate = vibefs_vfs_truncate,
    .rename   = vibefs_vfs_rename,
    .sync     = vibefs_vfs_sync,
};

/* =========================================================================
 * Vnode factory
 * ========================================================================= */

static struct vnode *vibefs_make_vnode(uint32_t ino) {
    if (ino == 0 || ino >= VIBEFS_MAX_INODES) return NULL;
    if (!vibefs.mounted) return NULL;

    struct vibefs_inode *inode = &vibefs.inodes[ino];
    if (inode->ino == 0) return NULL;  /* Deleted inode */

    struct vnode *vn = vfs_vnode_alloc();
    if (!vn) return NULL;

    vn->ino  = ino;
    vn->type = inode->type;
    vn->mode = inode->mode;
    vn->size = inode->size;
    vn->ops  = &vibefs_vfs_ops;
    return vn;
}

struct vnode *vibefs_get_vfs_root(void) {
    if (!vibefs.mounted) return NULL;
    return vibefs_make_vnode(VIBEFS_ROOT_INO);
}

/* =========================================================================
 * Shell utility functions
 * ========================================================================= */

void vibefs_ls(const char *path) {
    struct file *f = vfs_open(path ? path : "/", FILE_READ | FILE_DIR, 0);
    if (!f) {
        printf("ls: cannot open '%s'\n", path ? path : "/");
        return;
    }

    struct dirent entries[VFS_READDIR_BATCH];
    int n;
    bool any = false;

    while ((n = vfs_readdir(f, entries, VFS_READDIR_BATCH)) > 0) {
        for (int i = 0; i < n; i++) {
            const char *typestr = "?";
            switch (entries[i].type) {
                case VFS_TYPE_DIR:    typestr = "d"; break;
                case VFS_TYPE_REG:    typestr = "-"; break;
                case VFS_TYPE_CHRDEV: typestr = "c"; break;
                default: break;
            }

            /* Get size via stat */
            uint32_t sz = 0;
            if (entries[i].type == VFS_TYPE_REG) {
                /* Build full path for stat */
                char full[VFS_PATH_MAX];
                strncpy(full, path ? path : "/", VFS_PATH_MAX - 2);
                full[VFS_PATH_MAX - 2] = '\0';
                if (full[strlen(full) - 1] != '/')
                    strcat(full, "/");
                strcat(full, entries[i].name);

                struct vstat st;
                if (vfs_stat(full, &st) == 0)
                    sz = st.size;
            }

            if (entries[i].type == VFS_TYPE_DIR) {
                vga_set_color(0x0B);  /* Bright cyan for dirs */
                printf("%s  %-32s\n", typestr, entries[i].name);
                vga_set_color(0x07);
            } else {
                printf("%s  %-32s  %u bytes\n", typestr, entries[i].name, sz);
            }
            any = true;
        }
    }

    if (!any)
        printf("  (empty)\n");

    vfs_close(f);
}

void vibefs_info(void) {
    if (!vibefs.mounted) {
        printf("VibeFS: not mounted\n");
        return;
    }

    printf("VibeFS Filesystem Info:\n");
    printf("  Magic        : 0x%08x\n", vibefs.sb.magic);
    printf("  Version      : %u\n",      vibefs.sb.version);
    printf("  Total sectors: %u\n",      vibefs.sb.total_sectors);
    printf("  Free inodes  : %u / %u\n", vibefs.sb.free_inodes, VIBEFS_MAX_INODES);
    printf("  Free blocks  : %u / %u\n", vibefs.sb.free_blocks, VIBEFS_DATA_SECTORS);
    printf("  Root inode   : %u\n",      vibefs.sb.root_ino);
}
