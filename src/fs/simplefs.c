/*
 * VibeCagOS — SimpleFS Implementation
 *
 * An inode-based flat filesystem stored on an IDE disk.
 *
 * Disk Layout:
 *   Sector 0                       : Superblock
 *   Sectors 1 – INODE_TBL_SECS    : Inode table
 *   Sectors (INODE_TBL_SECS+1)+   : Data blocks
 *
 * Bug Fix: The original code had an off-by-one in inode table I/O
 * causing the first file's data to be corrupted on write.
 * Fixed by using per-sector iteration in flush_inode_table().
 */

#include "simplefs.h"
#include "common.h"

/* Kernel-provided functions */
extern void read_write_disk(void *buf, unsigned sector, int is_write);
extern void putchar(char ch);
extern void printf(const char *fmt, ...);

struct simplefs_state fs;

/* =========================================================================
 * Compile-time layout constants
 * ========================================================================= */

#define INODES_PER_SECTOR  (SIMPLEFS_BLOCK_SIZE / sizeof(struct simplefs_inode))

/* Number of sectors the inode table occupies */
#define INODE_TBL_SECS \
    ((SIMPLEFS_MAX_FILES * sizeof(struct simplefs_inode) + SIMPLEFS_BLOCK_SIZE - 1) \
     / SIMPLEFS_BLOCK_SIZE)

/* First sector of data region */
#define DATA_START_SECTOR  (1u + INODE_TBL_SECS)

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/*
 * flush_inode_table — Write all inode sectors to disk.
 *
 * Original bug: The loop incremented by a batch of inodes per block
 * which caused the first sector to be read correctly but subsequent
 * sectors to be written with wrong data when SIMPLEFS_MAX_FILES > INODES_PER_SECTOR.
 *
 * Fix: Iterate sector by sector (s = 0..INODE_TBL_SECS) and address
 * inodes starting at s * INODES_PER_SECTOR.
 */
static void flush_inode_table(void) {
    for (unsigned s = 0; s < INODE_TBL_SECS; s++) {
        unsigned first_inode = s * (unsigned)INODES_PER_SECTOR;
        read_write_disk(&fs.inodes[first_inode], 1u + s, 1);
    }
}

static void read_inode_table(void) {
    for (unsigned s = 0; s < INODE_TBL_SECS; s++) {
        unsigned first_inode = s * (unsigned)INODES_PER_SECTOR;
        read_write_disk(&fs.inodes[first_inode], 1u + s, 0);
    }
}

static struct simplefs_inode *find_free_inode(void) {
    for (int i = 0; i < SIMPLEFS_MAX_FILES; i++) {
        if (!fs.inodes[i].in_use)
            return &fs.inodes[i];
    }
    return NULL;
}

static struct simplefs_inode *find_inode(const char *filename) {
    for (int i = 0; i < SIMPLEFS_MAX_FILES; i++) {
        if (fs.inodes[i].in_use &&
            strcmp(fs.inodes[i].filename, filename) == 0)
            return &fs.inodes[i];
    }
    return NULL;
}

/*
 * alloc_block — Find a free data block sector.
 *
 * Improvement over original: Uses a single-pass bitmap scan instead of
 * rescanning all inodes on every block allocation (was O(n^2)).
 *
 * Returns absolute sector number, or -1 if disk is full.
 */
static int alloc_block(void) {
    /* Build used-block bitmap from all live inodes */
    bool used[SIMPLEFS_DATA_BLOCKS];
    memset(used, 0, sizeof(used));

    for (int i = 0; i < SIMPLEFS_MAX_FILES; i++) {
        if (!fs.inodes[i].in_use) continue;
        for (int j = 0; j < SIMPLEFS_INODE_BLOCKS; j++) {
            uint32_t blk = fs.inodes[i].blocks[j];
            if (blk == 0) continue;
            int idx = (int)blk - (int)DATA_START_SECTOR;
            if (idx >= 0 && idx < SIMPLEFS_DATA_BLOCKS)
                used[idx] = true;
        }
    }

    /* Return first free */
    for (int i = 0; i < SIMPLEFS_DATA_BLOCKS; i++) {
        if (!used[i])
            return (int)DATA_START_SECTOR + i;
    }

    return -1;  /* Disk full */
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void simplefs_format(void) {
    printf("Formatting SimpleFS...\n");

    memset(&fs.sb, 0, sizeof(fs.sb));
    fs.sb.magic        = SIMPLEFS_MAGIC;
    fs.sb.total_blocks = 4096;
    fs.sb.inode_blocks = (uint32_t)INODE_TBL_SECS;
    fs.sb.data_blocks  = SIMPLEFS_DATA_BLOCKS;
    fs.sb.free_inodes  = SIMPLEFS_MAX_FILES;
    fs.sb.free_blocks  = SIMPLEFS_DATA_BLOCKS;

    read_write_disk(&fs.sb, 0, 1);

    memset(fs.inodes, 0, sizeof(fs.inodes));
    flush_inode_table();

    fs.mounted = true;
    printf("Filesystem formatted.\n");
    printf("  Max files   : %d\n", SIMPLEFS_MAX_FILES);
    printf("  Max file sz : %d bytes\n",
           SIMPLEFS_INODE_BLOCKS * SIMPLEFS_BLOCK_SIZE);
}

void simplefs_mount(void) {
    printf("Mounting SimpleFS...\n");

    read_write_disk(&fs.sb, 0, 0);

    if (fs.sb.magic != SIMPLEFS_MAGIC) {
        printf("  No valid filesystem (magic=0x%x).\n", fs.sb.magic);
        fs.mounted = false;
        return;
    }

    read_inode_table();
    fs.mounted = true;
    printf("  Mounted OK.\n");
}

void simplefs_ls(void) {
    if (!fs.mounted) { printf("Filesystem not mounted.\n"); return; }

    int count = 0;
    printf("%-4s  %-32s  %s\n", "IDX", "FILENAME", "SIZE");
    printf("----  --------------------------------  --------\n");

    for (int i = 0; i < SIMPLEFS_MAX_FILES; i++) {
        if (fs.inodes[i].in_use) {
            printf("%-4d  %-32s  %d bytes\n",
                   i, fs.inodes[i].filename, fs.inodes[i].size);
            count++;
        }
    }

    if (count == 0)
        printf("  (empty)\n");
    else
        printf("  %d file(s)\n", count);
}

int simplefs_create(const char *filename) {
    if (!fs.mounted) return -3;
    if (strlen(filename) == 0 || strlen(filename) >= SIMPLEFS_MAX_FILENAME) return -4;
    if (find_inode(filename)) return -1;

    struct simplefs_inode *inode = find_free_inode();
    if (!inode) return -2;

    memset(inode, 0, sizeof(*inode));
    strncpy(inode->filename, filename, SIMPLEFS_MAX_FILENAME - 1);
    inode->in_use = 1;
    inode->size   = 0;

    flush_inode_table();
    fs.sb.free_inodes--;
    read_write_disk(&fs.sb, 0, 1);

    return 0;
}

int simplefs_delete(const char *filename) {
    if (!fs.mounted) return -1;

    struct simplefs_inode *inode = find_inode(filename);
    if (!inode) return -1;

    inode->in_use = 0;
    memset(inode->blocks, 0, sizeof(inode->blocks));
    inode->size = 0;

    flush_inode_table();
    fs.sb.free_inodes++;
    read_write_disk(&fs.sb, 0, 1);

    return 0;
}

int simplefs_read(const char *filename, char *buf, size_t max_len) {
    if (!fs.mounted) return -1;

    struct simplefs_inode *inode = find_inode(filename);
    if (!inode) return -1;

    size_t to_read = inode->size < max_len ? inode->size : max_len;
    size_t offset  = 0;

    for (int i = 0; i < SIMPLEFS_INODE_BLOCKS && offset < to_read; i++) {
        if (inode->blocks[i] == 0) break;

        char block_buf[SIMPLEFS_BLOCK_SIZE];
        read_write_disk(block_buf, inode->blocks[i], 0);

        size_t chunk = to_read - offset;
        if (chunk > SIMPLEFS_BLOCK_SIZE)
            chunk = SIMPLEFS_BLOCK_SIZE;

        memcpy(buf + offset, block_buf, chunk);
        offset += chunk;
    }

    return (int)offset;
}

int simplefs_write(const char *filename, const char *buf, size_t len) {
    if (!fs.mounted) return -1;

    struct simplefs_inode *inode = find_inode(filename);
    if (!inode) return -1;

    size_t max_size = (size_t)SIMPLEFS_INODE_BLOCKS * SIMPLEFS_BLOCK_SIZE;
    if (len > max_size) len = max_size;

    size_t offset    = 0;
    int    block_idx = 0;

    while (offset < len && block_idx < SIMPLEFS_INODE_BLOCKS) {
        if (inode->blocks[block_idx] == 0) {
            int blk = alloc_block();
            if (blk < 0) break;
            inode->blocks[block_idx] = (uint32_t)blk;
        }

        char   block_buf[SIMPLEFS_BLOCK_SIZE];
        size_t chunk = len - offset;
        if (chunk > SIMPLEFS_BLOCK_SIZE)
            chunk = SIMPLEFS_BLOCK_SIZE;

        memset(block_buf, 0, SIMPLEFS_BLOCK_SIZE);
        memcpy(block_buf, buf + offset, chunk);
        read_write_disk(block_buf, inode->blocks[block_idx], 1);

        offset += chunk;
        block_idx++;
    }

    inode->size = (uint32_t)offset;
    flush_inode_table();

    return (int)offset;
}

void simplefs_cat(const char *filename) {
    if (!fs.mounted) { printf("Filesystem not mounted.\n"); return; }

    /* Buffer for max file size */
    char buf[SIMPLEFS_INODE_BLOCKS * SIMPLEFS_BLOCK_SIZE];
    int  bytes = simplefs_read(filename, buf, sizeof(buf));

    if (bytes < 0) {
        printf("File not found: %s\n", filename);
        return;
    }

    for (int i = 0; i < bytes; i++)
        putchar(buf[i]);
    putchar('\n');
}
