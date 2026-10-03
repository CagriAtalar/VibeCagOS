#pragma once
#include "common.h"

/*
 * VibeCagOS — Virtual Filesystem (VFS) Layer
 *
 * Provides a uniform interface over different filesystem implementations.
 * Current backends: SimpleFS (on IDE), procfs (pseudo), devfs (pseudo).
 *
 * Architecture:
 *
 *   Syscalls (open, read, write, stat, readdir, mkdir, ...)
 *      |
 *      v
 *   VFS layer  (path resolution, vnode lookup, fd table)
 *      |
 *      +---> simplefs (IDE-backed inode filesystem)
 *      +---> procfs   (synthetic /proc entries)
 *      +---> devfs    (synthetic /dev entries — future)
 *
 * Ownership model:
 *   - struct vnode is ref-counted.  Callers must call vfs_vnode_put()
 *     when done.
 *   - struct file wraps a vnode + position; ref-counted separately.
 *   - File descriptors are per-process indices into the file table.
 *
 * IMPORTANT: This is a single-CPU design.  No locking is implemented
 * yet.  SMP requires adding a spinlock to vnode and mount table operations.
 */

/* =========================================================================
 * File type constants
 * ========================================================================= */

#define VFS_TYPE_NONE    0  /* Unused / invalid */
#define VFS_TYPE_REG     1  /* Regular file */
#define VFS_TYPE_DIR     2  /* Directory */
#define VFS_TYPE_SYMLINK 3  /* Symbolic link (future) */
#define VFS_TYPE_CHRDEV  4  /* Character device */
#define VFS_TYPE_BLKDEV  5  /* Block device (future) */

/* =========================================================================
 * Permission bits (POSIX-like, simplified)
 * ========================================================================= */

#define VFS_PERM_OWNER_R  0400
#define VFS_PERM_OWNER_W  0200
#define VFS_PERM_OWNER_X  0100
#define VFS_PERM_GROUP_R  0040
#define VFS_PERM_GROUP_W  0020
#define VFS_PERM_GROUP_X  0010
#define VFS_PERM_OTHER_R  0004
#define VFS_PERM_OTHER_W  0002
#define VFS_PERM_OTHER_X  0001

#define VFS_PERM_DEFAULT_FILE 0644
#define VFS_PERM_DEFAULT_DIR  0755

/* =========================================================================
 * Maximum filename and path lengths
 * ========================================================================= */

#define VFS_NAME_MAX  255
#define VFS_PATH_MAX  1024

/* Maximum number of directory entries returned per readdir call */
#define VFS_READDIR_BATCH 32

/* =========================================================================
 * Forward declarations
 * ========================================================================= */

struct vnode;
struct file;
struct mount;
struct fs_ops;
struct dirent;

/* =========================================================================
 * Directory entry (returned by readdir)
 * ========================================================================= */

struct dirent {
    uint32_t ino;                  /* Inode number */
    uint8_t  type;                 /* VFS_TYPE_* */
    char     name[VFS_NAME_MAX + 1];
};

/* =========================================================================
 * File statistics (stat-like)
 * ========================================================================= */

struct vstat {
    uint32_t ino;    /* Inode number */
    uint8_t  type;   /* VFS_TYPE_* */
    uint16_t mode;   /* Permission bits */
    uint32_t size;   /* File size in bytes */
    uint32_t nlink;  /* Hard link count */
};

/* =========================================================================
 * Filesystem operations (vtable per filesystem type)
 * ========================================================================= */

struct fs_ops {
    /*
     * lookup — find a child vnode by name inside a directory vnode.
     * Returns the child vnode on success (caller must call vfs_vnode_put()),
     * or NULL if not found.
     */
    struct vnode *(*lookup)(struct vnode *dir, const char *name);

    /*
     * create — create a new regular file inside a directory.
     * Returns 0 on success, negative error code on failure.
     */
    int (*create)(struct vnode *dir, const char *name, uint16_t mode,
                  struct vnode **out);

    /*
     * mkdir — create a new directory inside a parent directory.
     * Returns 0 on success, negative error code on failure.
     */
    int (*mkdir)(struct vnode *dir, const char *name, uint16_t mode,
                 struct vnode **out);

    /*
     * unlink — remove a file (decrement link count; free if 0).
     * Returns 0 on success.
     */
    int (*unlink)(struct vnode *dir, const char *name);

    /*
     * rmdir — remove an empty directory.
     * Returns 0 on success, -ENOTEMPTY if directory is not empty.
     */
    int (*rmdir)(struct vnode *dir, const char *name);

    /*
     * read — read up to `len` bytes from file at offset `off`.
     * Returns bytes read, or negative error.
     */
    int (*read)(struct vnode *vn, void *buf, size_t len, uint32_t off);

    /*
     * write — write `len` bytes to file at offset `off`.
     * Returns bytes written, or negative error.
     */
    int (*write)(struct vnode *vn, const void *buf, size_t len, uint32_t off);

    /*
     * readdir — fill `entries` with up to `max` directory entries starting
     * at cookie `*pos`.  Updates *pos for the next call.
     * Returns number of entries filled.
     */
    int (*readdir)(struct vnode *dir, struct dirent *entries, int max,
                   uint32_t *pos);

    /*
     * stat — fill *st with metadata for this vnode.
     */
    int (*stat)(struct vnode *vn, struct vstat *st);

    /*
     * truncate — change file size to `size`.
     */
    int (*truncate)(struct vnode *vn, uint32_t size);

    /*
     * rename — rename a directory entry (may be in same dir or different).
     */
    int (*rename)(struct vnode *old_dir, const char *old_name,
                  struct vnode *new_dir, const char *new_name);

    /*
     * sync — flush all dirty metadata and data to disk.
     */
    void (*sync)(struct mount *mnt);
};

/* =========================================================================
 * Vnode — in-memory representation of a file/directory
 * ========================================================================= */

struct vnode {
    uint32_t         ino;      /* Inode number (filesystem-specific) */
    uint8_t          type;     /* VFS_TYPE_* */
    uint16_t         mode;     /* Permission bits */
    uint32_t         size;     /* File size */
    uint32_t         refcnt;   /* Reference count */
    struct mount    *mount;    /* Which mount point owns this vnode */
    const struct fs_ops *ops;  /* Filesystem operations vtable */
    void            *priv;     /* Filesystem-private data pointer */
};

/* =========================================================================
 * Open file object
 * ========================================================================= */

#define FILE_READ   (1 << 0)
#define FILE_WRITE  (1 << 1)
#define FILE_APPEND (1 << 2)
#define FILE_DIR    (1 << 3)   /* Opened as directory */

struct file {
    struct vnode *vnode;   /* Underlying vnode */
    uint32_t      pos;     /* Current byte position (for read/write) */
    uint32_t      flags;   /* FILE_READ | FILE_WRITE | ... */
    uint32_t      refcnt;  /* Reference count */
    uint32_t      dir_pos; /* readdir position cookie */
};

/* =========================================================================
 * Mount table
 * ========================================================================= */

#define VFS_MAX_MOUNTS  8

struct mount {
    char              path[VFS_PATH_MAX];   /* Mount point path */
    struct vnode     *root;                 /* Root vnode of this filesystem */
    const struct fs_ops *ops;               /* Filesystem operations */
    void             *priv;                 /* Filesystem-private state */
    bool              active;
};

/* =========================================================================
 * File descriptor table
 * ========================================================================= */

#define VFS_FD_MAX  32   /* Per-process open file limit */

/* =========================================================================
 * VFS public API
 * ========================================================================= */

/*
 * vfs_init — Initialize the VFS layer.  Must be called before any other
 * VFS function.
 */
void vfs_init(void);

/*
 * vfs_mount — Mount a filesystem at `path`.
 * `ops` is the filesystem operations vtable.
 * `priv` is filesystem-private state.
 * `root` is the root vnode of the mounted filesystem.
 * Returns 0 on success.
 */
int vfs_mount(const char *path, const struct fs_ops *ops, void *priv,
              struct vnode *root);

/*
 * vfs_unmount — Unmount the filesystem at `path`.
 */
int vfs_unmount(const char *path);

/*
 * vfs_lookup — Resolve an absolute or cwd-relative path to a vnode.
 * Caller must call vfs_vnode_put() when done.
 * Returns NULL if path does not exist.
 */
struct vnode *vfs_lookup(const char *path);

/*
 * vfs_lookup_parent — Resolve the PARENT directory of `path`.
 * Fills `name_out` with the final component name.
 * Caller must call vfs_vnode_put() on the returned vnode.
 */
struct vnode *vfs_lookup_parent(const char *path, char *name_out, size_t name_out_len);

/*
 * vfs_vnode_alloc — Allocate a new blank vnode with refcnt=1.
 */
struct vnode *vfs_vnode_alloc(void);

/*
 * vfs_vnode_get — Increment reference count.
 */
void vfs_vnode_get(struct vnode *vn);

/*
 * vfs_vnode_put — Decrement reference count; free when it reaches 0.
 */
void vfs_vnode_put(struct vnode *vn);

/*
 * vfs_open — Open a file; returns a struct file* or NULL on error.
 * flags: FILE_READ, FILE_WRITE, FILE_APPEND
 * create_mode: if non-zero and file doesn't exist, create it with these perms.
 */
struct file *vfs_open(const char *path, uint32_t flags, uint16_t create_mode);

/*
 * vfs_close — Close an open file object.
 */
void vfs_close(struct file *f);

/*
 * vfs_read — Read from a file.  Returns bytes read or negative error.
 */
int vfs_read(struct file *f, void *buf, size_t len);

/*
 * vfs_write — Write to a file.  Returns bytes written or negative error.
 */
int vfs_write(struct file *f, const void *buf, size_t len);

/*
 * vfs_seek — Seek within a file.
 * whence: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END
 */
int vfs_seek(struct file *f, int32_t offset, int whence);

/*
 * vfs_stat — Get file metadata.  Returns 0 on success.
 */
int vfs_stat(const char *path, struct vstat *st);

/*
 * vfs_fstat — Get file metadata from open file.  Returns 0 on success.
 */
int vfs_fstat(struct file *f, struct vstat *st);

/*
 * vfs_mkdir — Create a directory.  Returns 0 on success.
 */
int vfs_mkdir(const char *path, uint16_t mode);

/*
 * vfs_unlink — Remove a file.  Returns 0 on success.
 */
int vfs_unlink(const char *path);

/*
 * vfs_rmdir — Remove an empty directory.  Returns 0 on success.
 */
int vfs_rmdir(const char *path);

/*
 * vfs_rename — Rename a file or directory.  Returns 0 on success.
 */
int vfs_rename(const char *old_path, const char *new_path);

/*
 * vfs_readdir — Read directory entries from an open directory file.
 * Returns number of entries written, 0 at end-of-directory.
 */
int vfs_readdir(struct file *f, struct dirent *entries, int max);

/*
 * vfs_truncate — Truncate a file to `size` bytes.
 */
int vfs_truncate(const char *path, uint32_t size);

/*
 * vfs_sync — Flush all dirty data/metadata to backing storage.
 */
void vfs_sync(void);

/*
 * vfs_get_cwd — Copy the current working directory path into `buf`.
 */
void vfs_get_cwd(char *buf, size_t len);

/*
 * vfs_set_cwd — Change the current working directory.
 * Returns 0 on success, -1 if path doesn't exist or isn't a directory.
 */
int vfs_set_cwd(const char *path);

/*
 * vfs_dump_mounts — Print mount table for debugging.
 */
void vfs_dump_mounts(void);

/* =========================================================================
 * VFS Error codes
 * ========================================================================= */

#define VFS_OK         0
#define VFS_ENOENT    -1   /* File not found */
#define VFS_EEXIST    -2   /* Already exists */
#define VFS_ENOTDIR   -3   /* Not a directory */
#define VFS_EISDIR    -4   /* Is a directory */
#define VFS_ENOMEM    -5   /* Out of memory */
#define VFS_EIO       -6   /* I/O error */
#define VFS_ENOSPC    -7   /* No space left */
#define VFS_ENOTEMPTY -8   /* Directory not empty */
#define VFS_EINVAL    -9   /* Invalid argument */
#define VFS_EACCES    -10  /* Permission denied */
#define VFS_ENAMETOOLONG -11 /* Filename too long */
