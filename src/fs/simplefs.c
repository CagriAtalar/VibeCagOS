#include "simplefs.h"
#include "common.h"

// Forward declarations
void read_write_disk(void *buf, unsigned sector, int is_write);
void putchar(char ch);
void printf(const char *fmt, ...);

struct simplefs_state fs;

// Find a free inode
static struct simplefs_inode *find_free_inode(void) {
    for (int i = 0; i < SIMPLEFS_MAX_FILES; i++) {
        if (!fs.inodes[i].in_use) {
            return &fs.inodes[i];
        }
    }
    return NULL;
}

// Find inode by filename
static struct simplefs_inode *find_inode(const char *filename) {
    for (int i = 0; i < SIMPLEFS_MAX_FILES; i++) {
        if (fs.inodes[i].in_use && strcmp(fs.inodes[i].filename, filename) == 0) {
            return &fs.inodes[i];
        }
    }
    return NULL;
}

// Allocate a data block
static int alloc_block(void) {
    // Start from block after inode table
    int start_block = 1 + (SIMPLEFS_MAX_FILES * sizeof(struct simplefs_inode) + SIMPLEFS_BLOCK_SIZE - 1) / SIMPLEFS_BLOCK_SIZE;
    
    // Simple linear search for free block (in real FS, use bitmap)
    // For now, we track in-use blocks by checking inodes
    bool used[SIMPLEFS_DATA_BLOCKS] = {false};
    
    // Mark blocks used by existing files
    for (int i = 0; i < SIMPLEFS_MAX_FILES; i++) {
        if (fs.inodes[i].in_use) {
            for (int j = 0; j < 4; j++) {
                if (fs.inodes[i].blocks[j] != 0) {
                    int block_idx = fs.inodes[i].blocks[j] - start_block;
                    if (block_idx >= 0 && block_idx < SIMPLEFS_DATA_BLOCKS) {
                        used[block_idx] = true;
                    }
                }
            }
        }
    }
    
    // Find first free block
    for (int i = 0; i < SIMPLEFS_DATA_BLOCKS; i++) {
        if (!used[i]) {
            return start_block + i;
        }
    }
    
    return -1;  // No free blocks
}

// Format the disk with simplefs
void simplefs_format(void) {
    printf("Formatting disk with SimpleFS...\n");
    
    // Initialize superblock
    memset(&fs.sb, 0, sizeof(fs.sb));
    fs.sb.magic = SIMPLEFS_MAGIC;
    fs.sb.total_blocks = 4096;  // 2MB with 512-byte blocks
    fs.sb.inode_blocks = (SIMPLEFS_MAX_FILES * sizeof(struct simplefs_inode) + SIMPLEFS_BLOCK_SIZE - 1) / SIMPLEFS_BLOCK_SIZE;
    fs.sb.data_blocks = SIMPLEFS_DATA_BLOCKS;
    fs.sb.free_inodes = SIMPLEFS_MAX_FILES;
    fs.sb.free_blocks = SIMPLEFS_DATA_BLOCKS;
    
    // Write superblock to sector 0
    read_write_disk(&fs.sb, 0, 1);
    
    // Initialize inodes
    memset(fs.inodes, 0, sizeof(fs.inodes));
    
    // Write inode table starting at sector 1
    for (int i = 0; i < SIMPLEFS_MAX_FILES; i += (SIMPLEFS_BLOCK_SIZE / sizeof(struct simplefs_inode))) {
        int inodes_per_block = SIMPLEFS_BLOCK_SIZE / sizeof(struct simplefs_inode);
        int sector = 1 + i / inodes_per_block;
        read_write_disk(&fs.inodes[i], sector, 1);
    }
    
    fs.mounted = true;
    printf("Filesystem formatted successfully!\n");
    printf("  Total blocks: %d\n", fs.sb.total_blocks);
    printf("  Inode blocks: %d\n", fs.sb.inode_blocks);
    printf("  Data blocks: %d\n", fs.sb.data_blocks);
    printf("  Max files: %d\n", SIMPLEFS_MAX_FILES);
}

// Mount the filesystem
void simplefs_mount(void) {
    printf("Mounting SimpleFS...\n");
    
    // Read superblock from sector 0
    read_write_disk(&fs.sb, 0, 0);
    
    if (fs.sb.magic != SIMPLEFS_MAGIC) {
        printf("Invalid filesystem! Please format first.\n");
        fs.mounted = false;
        return;
    }
    
    // Read inode table
    for (int i = 0; i < SIMPLEFS_MAX_FILES; i += (SIMPLEFS_BLOCK_SIZE / sizeof(struct simplefs_inode))) {
        int inodes_per_block = SIMPLEFS_BLOCK_SIZE / sizeof(struct simplefs_inode);
        int sector = 1 + i / inodes_per_block;
        read_write_disk(&fs.inodes[i], sector, 0);
    }
    
    fs.mounted = true;
    printf("Filesystem mounted successfully!\n");
}

// List all files
void simplefs_ls(void) {
    if (!fs.mounted) {
        printf("Filesystem not mounted!\n");
        return;
    }
    
    printf("Files:\n");
    int count = 0;
    for (int i = 0; i < SIMPLEFS_MAX_FILES; i++) {
        if (fs.inodes[i].in_use) {
            printf("  [%d] %s (%d bytes)\n", i, fs.inodes[i].filename, fs.inodes[i].size);
            count++;
        }
    }
    if (count == 0) {
        printf("  (no files)\n");
    }
    printf("Total: %d files\n", count);
}

// Create a new file
int simplefs_create(const char *filename) {
    if (!fs.mounted) {
        printf("Filesystem not mounted!\n");
        return -1;
    }
    
    // Check if file already exists
    if (find_inode(filename)) {
        return -1;  // Already exists
    }
    
    // Find free inode
    struct simplefs_inode *inode = find_free_inode();
    if (!inode) {
        return -2;  // No free inodes
    }
    
    // Initialize inode
    memset(inode, 0, sizeof(*inode));
    strcpy(inode->filename, filename);
    inode->in_use = 1;
    inode->size = 0;
    
    // Write inode table back to disk
    for (int i = 0; i < SIMPLEFS_MAX_FILES; i += (SIMPLEFS_BLOCK_SIZE / sizeof(struct simplefs_inode))) {
        int inodes_per_block = SIMPLEFS_BLOCK_SIZE / sizeof(struct simplefs_inode);
        int sector = 1 + i / inodes_per_block;
        read_write_disk(&fs.inodes[i], sector, 1);
    }
    
    fs.sb.free_inodes--;
    read_write_disk(&fs.sb, 0, 1);
    
    return 0;
}

// Delete a file
int simplefs_delete(const char *filename) {
    if (!fs.mounted) {
        printf("Filesystem not mounted!\n");
        return -1;
    }
    
    struct simplefs_inode *inode = find_inode(filename);
    if (!inode) {
        return -1;  // Not found
    }
    
    // Mark inode as free
    inode->in_use = 0;
    
    // Write inode table back to disk
    for (int i = 0; i < SIMPLEFS_MAX_FILES; i += (SIMPLEFS_BLOCK_SIZE / sizeof(struct simplefs_inode))) {
        int inodes_per_block = SIMPLEFS_BLOCK_SIZE / sizeof(struct simplefs_inode);
        int sector = 1 + i / inodes_per_block;
        read_write_disk(&fs.inodes[i], sector, 1);
    }
    
    fs.sb.free_inodes++;
    read_write_disk(&fs.sb, 0, 1);
    
    return 0;
}

// Read file contents
int simplefs_read(const char *filename, char *buf, size_t max_len) {
    if (!fs.mounted) {
        printf("Filesystem not mounted!\n");
        return -1;
    }
    
    struct simplefs_inode *inode = find_inode(filename);
    if (!inode) {
        return -1;  // Not found
    }
    
    size_t to_read = inode->size < max_len ? inode->size : max_len;
    size_t offset = 0;
    
    // Read from data blocks
    for (int i = 0; i < 4 && offset < to_read; i++) {
        if (inode->blocks[i] == 0) break;
        
        char block_buf[SIMPLEFS_BLOCK_SIZE];
        read_write_disk(block_buf, inode->blocks[i], 0);
        
        size_t to_copy = to_read - offset;
        if (to_copy > SIMPLEFS_BLOCK_SIZE) {
            to_copy = SIMPLEFS_BLOCK_SIZE;
        }
        
        memcpy(buf + offset, block_buf, to_copy);
        offset += to_copy;
    }
    
    return offset;
}

// Write file contents
int simplefs_write(const char *filename, const char *buf, size_t len) {
    if (!fs.mounted) {
        printf("Filesystem not mounted!\n");
        return -1;
    }
    
    struct simplefs_inode *inode = find_inode(filename);
    if (!inode) {
        return -1;  // Not found
    }
    
    // Limit to 4 blocks
    if (len > SIMPLEFS_BLOCK_SIZE * 4) {
        len = SIMPLEFS_BLOCK_SIZE * 4;
    }
    
    size_t offset = 0;
    int block_idx = 0;
    
    while (offset < len && block_idx < 4) {
        // Allocate block if needed
        if (inode->blocks[block_idx] == 0) {
            int new_block = alloc_block();
            if (new_block < 0) {
                break;  // No more free blocks
            }
            inode->blocks[block_idx] = new_block;
        }
        
        // Write to block
        char block_buf[SIMPLEFS_BLOCK_SIZE];
        memset(block_buf, 0, SIMPLEFS_BLOCK_SIZE);
        
        size_t to_copy = len - offset;
        if (to_copy > SIMPLEFS_BLOCK_SIZE) {
            to_copy = SIMPLEFS_BLOCK_SIZE;
        }
        
        memcpy(block_buf, buf + offset, to_copy);
        read_write_disk(block_buf, inode->blocks[block_idx], 1);
        
        offset += to_copy;
        block_idx++;
    }
    
    inode->size = offset;
    
    // Write inode table back to disk
    for (int i = 0; i < SIMPLEFS_MAX_FILES; i += (SIMPLEFS_BLOCK_SIZE / sizeof(struct simplefs_inode))) {
        int inodes_per_block = SIMPLEFS_BLOCK_SIZE / sizeof(struct simplefs_inode);
        int sector = 1 + i / inodes_per_block;
        read_write_disk(&fs.inodes[i], sector, 1);
    }
    
    return offset;
}

// Cat file contents
void simplefs_cat(const char *filename) {
    char buf[SIMPLEFS_MAX_FILE_SIZE];
    int bytes = simplefs_read(filename, buf, sizeof(buf));
    
    if (bytes < 0) {
        printf("File not found: %s\n", filename);
        return;
    }
    
    for (int i = 0; i < bytes; i++) {
        putchar(buf[i]);
    }
    putchar('\n');
}

