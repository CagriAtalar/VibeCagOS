#pragma once
#include "common.h"

// Simple inode-based filesystem for VirtualBox disk

#define SIMPLEFS_MAGIC 0x53494D50  // "SIMP"
#define SIMPLEFS_MAX_FILES 64
#define SIMPLEFS_MAX_FILENAME 56
#define SIMPLEFS_BLOCK_SIZE 512
#define SIMPLEFS_MAX_FILE_SIZE (128 * SIMPLEFS_BLOCK_SIZE)  // 64KB max per file
#define SIMPLEFS_DATA_BLOCKS 1024

// Superblock - first sector of disk
struct simplefs_superblock {
    uint32_t magic;              // Magic number
    uint32_t total_blocks;       // Total number of blocks
    uint32_t inode_blocks;       // Number of blocks for inodes
    uint32_t data_blocks;        // Number of data blocks
    uint32_t free_inodes;        // Number of free inodes
    uint32_t free_blocks;        // Number of free data blocks
    uint8_t padding[SIMPLEFS_BLOCK_SIZE - 24];
} __attribute__((packed));

// Inode - file metadata
struct simplefs_inode {
    char filename[SIMPLEFS_MAX_FILENAME];  // File name
    uint32_t size;               // File size in bytes
    uint32_t blocks[4];          // Direct block pointers (4 blocks = 2KB)
    uint8_t in_use;              // 1 if in use, 0 if free
    uint8_t padding[3];
} __attribute__((packed));

// In-memory filesystem state
struct simplefs_state {
    struct simplefs_superblock sb;
    struct simplefs_inode inodes[SIMPLEFS_MAX_FILES];
    bool mounted;
};

// Global filesystem state (defined in simplefs.c)
extern struct simplefs_state fs;

// Filesystem operations
void simplefs_format(void);
void simplefs_mount(void);
void simplefs_ls(void);
int simplefs_create(const char *filename);
int simplefs_delete(const char *filename);
int simplefs_read(const char *filename, char *buf, size_t max_len);
int simplefs_write(const char *filename, const char *buf, size_t len);
void simplefs_cat(const char *filename);

