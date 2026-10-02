#pragma once
#include "common.h"

/*
 * VibeCagOS — SimpleFS Header
 *
 * Flat inode-based filesystem for IDE disk.
 * Disk layout:
 *   Sector 0                    : Superblock
 *   Sectors 1 – INODE_SECTORS  : Inode table
 *   Sectors (INODE_SECTORS+1)+ : Data blocks
 */

#define SIMPLEFS_MAGIC         0x53494D50u   /* "SIMP" */
#define SIMPLEFS_MAX_FILES     64
#define SIMPLEFS_MAX_FILENAME  56            /* Includes null terminator */
#define SIMPLEFS_BLOCK_SIZE    512
#define SIMPLEFS_INODE_BLOCKS  8            /* Direct block pointers per inode (4 KB max/file) */
#define SIMPLEFS_DATA_BLOCKS   1024         /* Total data blocks on disk */

/* Superblock — first sector of disk */
struct simplefs_superblock {
    uint32_t magic;
    uint32_t total_blocks;
    uint32_t inode_blocks;
    uint32_t data_blocks;
    uint32_t free_inodes;
    uint32_t free_blocks;
    uint8_t  padding[SIMPLEFS_BLOCK_SIZE - 24];
} __attribute__((packed));

/* Inode — file metadata */
struct simplefs_inode {
    char     filename[SIMPLEFS_MAX_FILENAME];
    uint32_t size;
    uint32_t blocks[SIMPLEFS_INODE_BLOCKS];  /* Direct block sector numbers */
    uint8_t  in_use;
    uint8_t  padding[3];
} __attribute__((packed));

/* In-memory filesystem state */
struct simplefs_state {
    struct simplefs_superblock sb;
    struct simplefs_inode      inodes[SIMPLEFS_MAX_FILES];
    bool                       mounted;
};

/* Global filesystem state */
extern struct simplefs_state fs;

/* Operations */
void simplefs_format(void);
void simplefs_mount(void);
void simplefs_ls(void);
int  simplefs_create(const char *filename);
int  simplefs_delete(const char *filename);
int  simplefs_read(const char *filename, char *buf, size_t max_len);
int  simplefs_write(const char *filename, const char *buf, size_t len);
void simplefs_cat(const char *filename);
