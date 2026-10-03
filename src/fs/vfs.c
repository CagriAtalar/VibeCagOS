/*
 * VibeCagOS — VFS Layer Implementation
 *
 * Responsibilities:
 *   1. Maintain a mount table (up to VFS_MAX_MOUNTS entries).
 *   2. Resolve paths to vnodes (walking the directory tree).
 *   3. Maintain the current working directory (CWD) as a string.
 *   4. Dispatch all file operations to the appropriate filesystem backend.
 *   5. Manage file reference counts.
 *
 * Path resolution:
 *
 *   input path
 *       |
 *       v
 *   absolute? (/...) or relative?
 *       |
 *       +-- relative -> prepend cwd
 *       v
 *   tokenize path components (split on '/')
 *       |
 *       v
 *   start from root vnode of the best-matching mount
 *       |
 *       v
 *   for each component:
 *       lookup(current_dir, component) -> next_vnode
 *       |
 *       +-- '.' -> stay
 *       +-- '..' -> go to parent (tracked as a stack)
 *       +-- name -> ops->lookup(dir, name)
 *       v
 *   return final vnode
 *
 * This implementation does NOT support:
 *   - Symlinks (follow)
 *   - SMP locking (single CPU assumed)
 *   - Bind mounts or overlays
 *
 * Known limitations:
 *   - CWD is a global (not per-process) — once per-process structures
 *     are robust, move into struct process.
 *   - Max 8 mount points.
 *   - Path resolution is O(depth) — acceptable for a hobby OS.
 */

#include "vfs.h"
#include "common.h"
#include "kernel/kmalloc.h"
#include "kernel/kernel.h"

/* =========================================================================
 * Global VFS state
 * ========================================================================= */

static struct mount   mounts[VFS_MAX_MOUNTS];
static int            mount_count = 0;

/* Current working directory — global for now, move to per-process later */
static char cwd[VFS_PATH_MAX] = "/";

/* =========================================================================
 * Internal: vnode allocation and reference counting
 * ========================================================================= */

struct vnode *vfs_vnode_alloc(void) {
    struct vnode *vn = (struct vnode *)kmalloc(sizeof(struct vnode));
    if (!vn) return NULL;
    memset(vn, 0, sizeof(*vn));
    vn->refcnt = 1;
    return vn;
}

void vfs_vnode_get(struct vnode *vn) {
    if (vn) vn->refcnt++;
}

void vfs_vnode_put(struct vnode *vn) {
    if (!vn) return;
    if (vn->refcnt == 0) {
        PANIC("vfs_vnode_put: refcnt already 0 for ino %u", vn->ino);
    }
    vn->refcnt--;
    if (vn->refcnt == 0) {
        kfree(vn);
    }
}

/* =========================================================================
 * Internal: path utilities
 * ========================================================================= */

/*
 * path_normalize — collapse '.', '..', double slashes into `out`.
 * `out` must be at least VFS_PATH_MAX bytes.
 * `base` is the starting directory for relative paths.
 */
static void path_normalize(const char *in, const char *base, char *out) {
    char work[VFS_PATH_MAX];
    int  wlen = 0;

    /* If relative, prepend base */
    if (in[0] != '/') {
        int blen = (int)strlen(base);
        if (blen >= VFS_PATH_MAX - 1) blen = VFS_PATH_MAX - 2;
        memcpy(work, base, (size_t)blen);
        work[blen] = '/';
        wlen = blen + 1;
        int ilen = (int)strlen(in);
        if (wlen + ilen < VFS_PATH_MAX - 1) {
            memcpy(work + wlen, in, (size_t)ilen);
            wlen += ilen;
        }
    } else {
        int ilen = (int)strlen(in);
        if (ilen >= VFS_PATH_MAX) ilen = VFS_PATH_MAX - 1;
        memcpy(work, in, (size_t)ilen);
        wlen = ilen;
    }
    work[wlen] = '\0';

    /* Now collapse components into out using a stack approach */
    /* We'll write directly into out as /comp1/comp2/... */
    static char components[VFS_PATH_MAX / 2 + 1][VFS_NAME_MAX + 1];
    int  depth = 0;

    char *p = work;
    while (*p) {
        /* Skip slashes */
        while (*p == '/') p++;
        if (*p == '\0') break;

        /* Extract component */
        char *start = p;
        while (*p && *p != '/') p++;
        int clen = (int)(p - start);

        if (clen == 1 && start[0] == '.') {
            /* Current dir — skip */
            continue;
        }
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            /* Parent dir */
            if (depth > 0) depth--;
            continue;
        }

        /* Normal component */
        if (depth < (int)(sizeof(components) / sizeof(components[0])) - 1) {
            if (clen > VFS_NAME_MAX) clen = VFS_NAME_MAX;
            memcpy(components[depth], start, (size_t)clen);
            components[depth][clen] = '\0';
            depth++;
        }
    }

    /* Reconstruct */
    if (depth == 0) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }

    int olen = 0;
    for (int i = 0; i < depth; i++) {
        out[olen++] = '/';
        int clen = (int)strlen(components[i]);
        if (olen + clen >= VFS_PATH_MAX - 1) break;
        memcpy(out + olen, components[i], (size_t)clen);
        olen += clen;
    }
    out[olen] = '\0';
}

/*
 * Find the mount that best matches this path (longest prefix match).
 */
static struct mount *find_mount(const char *abs_path) {
    struct mount *best = NULL;
    int best_len = -1;

    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].active) continue;
        int mlen = (int)strlen(mounts[i].path);
        if (mlen > best_len && strncmp(abs_path, mounts[i].path, (size_t)mlen) == 0) {
            /* path must match mount point exactly or be a child */
            if (abs_path[mlen] == '\0' || abs_path[mlen] == '/' || mlen == 1) {
                best = &mounts[i];
                best_len = mlen;
            }
        }
    }
    return best;
}

/* =========================================================================
 * vfs_init
 * ========================================================================= */

void vfs_init(void) {
    memset(mounts, 0, sizeof(mounts));
    mount_count = 0;
    memcpy(cwd, "/", 2);
}

/* =========================================================================
 * vfs_mount / vfs_unmount
 * ========================================================================= */

int vfs_mount(const char *path, const struct fs_ops *ops, void *priv,
              struct vnode *root) {
    if (mount_count >= VFS_MAX_MOUNTS) return VFS_EINVAL;
    if (!path || !ops || !root) return VFS_EINVAL;

    /* Check for duplicate */
    for (int i = 0; i < mount_count; i++) {
        if (mounts[i].active && strcmp(mounts[i].path, path) == 0)
            return VFS_EEXIST;
    }

    struct mount *m = &mounts[mount_count++];
    strncpy(m->path, path, VFS_PATH_MAX - 1);
    m->path[VFS_PATH_MAX - 1] = '\0';
    m->ops    = ops;
    m->priv   = priv;
    m->root   = root;
    m->active = true;

    vfs_vnode_get(root);  /* mount holds a ref */
    return VFS_OK;
}

int vfs_unmount(const char *path) {
    for (int i = 0; i < mount_count; i++) {
        if (mounts[i].active && strcmp(mounts[i].path, path) == 0) {
            vfs_vnode_put(mounts[i].root);
            mounts[i].active = false;
            return VFS_OK;
        }
    }
    return VFS_ENOENT;
}

/* =========================================================================
 * vfs_lookup — core path resolution
 * ========================================================================= */

struct vnode *vfs_lookup(const char *path) {
    char abs[VFS_PATH_MAX];
    path_normalize(path, cwd, abs);

    struct mount *m = find_mount(abs);
    if (!m) return NULL;

    /* Strip mount prefix from abs to get relative path within fs */
    const char *rel = abs + strlen(m->path);
    if (*rel == '/') rel++;

    /* Start from the mount root */
    struct vnode *cur = m->root;
    vfs_vnode_get(cur);

    if (*rel == '\0') {
        /* Asking for the root of this mount */
        return cur;
    }

    /* Walk path components */
    char component[VFS_NAME_MAX + 1];
    const char *p = rel;

    while (*p) {
        /* Skip slashes */
        while (*p == '/') p++;
        if (*p == '\0') break;

        /* Extract component */
        const char *start = p;
        while (*p && *p != '/') p++;
        int clen = (int)(p - start);

        if (clen > VFS_NAME_MAX) {
            vfs_vnode_put(cur);
            return NULL;
        }

        memcpy(component, start, (size_t)clen);
        component[clen] = '\0';

        /* Must be in a directory */
        if (cur->type != VFS_TYPE_DIR || !cur->ops || !cur->ops->lookup) {
            vfs_vnode_put(cur);
            return NULL;
        }

        struct vnode *next = cur->ops->lookup(cur, component);
        vfs_vnode_put(cur);
        if (!next) return NULL;
        cur = next;
    }

    return cur;
}

struct vnode *vfs_lookup_parent(const char *path, char *name_out, size_t name_out_len) {
    char abs[VFS_PATH_MAX];
    path_normalize(path, cwd, abs);

    /* Find the last component */
    char *last_slash = NULL;
    for (char *p = abs; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    if (!last_slash) {
        if (name_out && name_out_len > 0) {
            strncpy(name_out, path, name_out_len - 1);
            name_out[name_out_len - 1] = '\0';
        }
        return NULL;
    }

    /* Safely copy the component name before modifying abs */
    if (name_out && name_out_len > 0) {
        strncpy(name_out, last_slash + 1, name_out_len - 1);
        name_out[name_out_len - 1] = '\0';
    }

    if (last_slash == abs) {
        /* Parent is root */
        abs[1] = '\0';
    } else {
        *last_slash = '\0';
    }

    /* Now look up the parent directory */
    struct vnode *parent = vfs_lookup(abs);
    return parent;
}

/* =========================================================================
 * vfs_open / vfs_close
 * ========================================================================= */

struct file *vfs_open(const char *path, uint32_t flags, uint16_t create_mode) {
    struct vnode *vn = vfs_lookup(path);

    if (!vn && create_mode && (flags & FILE_WRITE)) {
        /* Create the file */
        char name[VFS_NAME_MAX + 1];
        struct vnode *parent = vfs_lookup_parent(path, name, sizeof(name));
        if (!parent || !parent->ops || !parent->ops->create) {
            if (parent) vfs_vnode_put(parent);
            return NULL;
        }
        int r = parent->ops->create(parent, name, create_mode, &vn);
        vfs_vnode_put(parent);
        if (r != 0 || !vn) return NULL;
    }

    if (!vn) return NULL;

    /* Directories can be opened for readdir */
    if (vn->type == VFS_TYPE_DIR)
        flags |= FILE_DIR;

    struct file *f = (struct file *)kmalloc(sizeof(struct file));
    if (!f) { vfs_vnode_put(vn); return NULL; }

    f->vnode   = vn;  /* already has ref from lookup */
    f->flags   = flags;
    f->pos     = (flags & FILE_APPEND) ? vn->size : 0;
    f->refcnt  = 1;
    f->dir_pos = 0;

    return f;
}

void vfs_close(struct file *f) {
    if (!f) return;
    f->refcnt--;
    if (f->refcnt == 0) {
        vfs_vnode_put(f->vnode);
        kfree(f);
    }
}

/* =========================================================================
 * vfs_read / vfs_write / vfs_seek
 * ========================================================================= */

int vfs_read(struct file *f, void *buf, size_t len) {
    if (!f || !f->vnode) return VFS_EINVAL;
    if (!(f->flags & FILE_READ)) return VFS_EACCES;
    if (f->vnode->type == VFS_TYPE_DIR) return VFS_EISDIR;
    if (!f->vnode->ops || !f->vnode->ops->read) return VFS_EINVAL;

    int r = f->vnode->ops->read(f->vnode, buf, len, f->pos);
    if (r > 0) f->pos += (uint32_t)r;
    return r;
}

int vfs_write(struct file *f, const void *buf, size_t len) {
    if (!f || !f->vnode) return VFS_EINVAL;
    if (!(f->flags & FILE_WRITE)) return VFS_EACCES;
    if (f->vnode->type == VFS_TYPE_DIR) return VFS_EISDIR;
    if (!f->vnode->ops || !f->vnode->ops->write) return VFS_EINVAL;

    if (f->flags & FILE_APPEND) {
        /* Re-sync position to EOF */
        f->pos = f->vnode->size;
    }

    int r = f->vnode->ops->write(f->vnode, buf, len, f->pos);
    if (r > 0) {
        f->pos += (uint32_t)r;
        /* Update vnode size if we extended the file */
        if (f->pos > f->vnode->size)
            f->vnode->size = f->pos;
    }
    return r;
}

int vfs_seek(struct file *f, int32_t offset, int whence) {
    if (!f) return VFS_EINVAL;

    int32_t new_pos;
    switch (whence) {
        case 0: new_pos = offset; break;                          /* SEEK_SET */
        case 1: new_pos = (int32_t)f->pos + offset; break;       /* SEEK_CUR */
        case 2: new_pos = (int32_t)f->vnode->size + offset; break;/* SEEK_END */
        default: return VFS_EINVAL;
    }

    if (new_pos < 0) return VFS_EINVAL;
    f->pos = (uint32_t)new_pos;
    return (int)f->pos;
}

/* =========================================================================
 * vfs_stat / vfs_fstat
 * ========================================================================= */

int vfs_stat(const char *path, struct vstat *st) {
    struct vnode *vn = vfs_lookup(path);
    if (!vn) return VFS_ENOENT;

    int r = VFS_EINVAL;
    if (vn->ops && vn->ops->stat)
        r = vn->ops->stat(vn, st);

    vfs_vnode_put(vn);
    return r;
}

int vfs_fstat(struct file *f, struct vstat *st) {
    if (!f || !f->vnode) return VFS_EINVAL;
    if (!f->vnode->ops || !f->vnode->ops->stat) return VFS_EINVAL;
    return f->vnode->ops->stat(f->vnode, st);
}

/* =========================================================================
 * vfs_mkdir / vfs_unlink / vfs_rmdir / vfs_rename
 * ========================================================================= */

int vfs_mkdir(const char *path, uint16_t mode) {
    char name[VFS_NAME_MAX + 1];
    struct vnode *parent = vfs_lookup_parent(path, name, sizeof(name));
    if (!parent) return VFS_ENOENT;

    if (parent->type != VFS_TYPE_DIR) {
        vfs_vnode_put(parent);
        return VFS_ENOTDIR;
    }

    if (!parent->ops || !parent->ops->mkdir) {
        vfs_vnode_put(parent);
        return VFS_EINVAL;
    }

    struct vnode *newdir = NULL;
    int r = parent->ops->mkdir(parent, name, mode, &newdir);
    if (newdir) vfs_vnode_put(newdir);
    vfs_vnode_put(parent);
    return r;
}

int vfs_unlink(const char *path) {
    char name[VFS_NAME_MAX + 1];
    struct vnode *parent = vfs_lookup_parent(path, name, sizeof(name));
    if (!parent) return VFS_ENOENT;

    if (!parent->ops || !parent->ops->unlink) {
        vfs_vnode_put(parent);
        return VFS_EINVAL;
    }

    int r = parent->ops->unlink(parent, name);
    vfs_vnode_put(parent);
    return r;
}

int vfs_rmdir(const char *path) {
    char name[VFS_NAME_MAX + 1];
    struct vnode *parent = vfs_lookup_parent(path, name, sizeof(name));
    if (!parent) return VFS_ENOENT;

    if (!parent->ops || !parent->ops->rmdir) {
        vfs_vnode_put(parent);
        return VFS_EINVAL;
    }

    int r = parent->ops->rmdir(parent, name);
    vfs_vnode_put(parent);
    return r;
}

int vfs_rename(const char *old_path, const char *new_path) {
    char old_name[VFS_NAME_MAX + 1];
    char new_name[VFS_NAME_MAX + 1];
    struct vnode *old_parent = vfs_lookup_parent(old_path, old_name, sizeof(old_name));
    if (!old_parent) return VFS_ENOENT;

    struct vnode *new_parent = vfs_lookup_parent(new_path, new_name, sizeof(new_name));
    if (!new_parent) {
        vfs_vnode_put(old_parent);
        return VFS_ENOENT;
    }

    int r = VFS_EINVAL;
    if (old_parent->ops && old_parent->ops->rename)
        r = old_parent->ops->rename(old_parent, old_name, new_parent, new_name);

    vfs_vnode_put(old_parent);
    vfs_vnode_put(new_parent);
    return r;
}

/* =========================================================================
 * vfs_readdir
 * ========================================================================= */

int vfs_readdir(struct file *f, struct dirent *entries, int max) {
    if (!f || !f->vnode) return VFS_EINVAL;
    if (f->vnode->type != VFS_TYPE_DIR) return VFS_ENOTDIR;
    if (!f->vnode->ops || !f->vnode->ops->readdir) return VFS_EINVAL;

    return f->vnode->ops->readdir(f->vnode, entries, max, &f->dir_pos);
}

/* =========================================================================
 * vfs_truncate
 * ========================================================================= */

int vfs_truncate(const char *path, uint32_t size) {
    struct vnode *vn = vfs_lookup(path);
    if (!vn) return VFS_ENOENT;

    int r = VFS_EINVAL;
    if (vn->ops && vn->ops->truncate)
        r = vn->ops->truncate(vn, size);

    vfs_vnode_put(vn);
    return r;
}

/* =========================================================================
 * vfs_sync
 * ========================================================================= */

void vfs_sync(void) {
    for (int i = 0; i < mount_count; i++) {
        if (mounts[i].active && mounts[i].ops && mounts[i].ops->sync)
            mounts[i].ops->sync(&mounts[i]);
    }
}

/* =========================================================================
 * CWD management
 * ========================================================================= */

void vfs_get_cwd(char *buf, size_t len) {
    strncpy(buf, cwd, len - 1);
    buf[len - 1] = '\0';
}

int vfs_set_cwd(const char *path) {
    char abs[VFS_PATH_MAX];
    path_normalize(path, cwd, abs);

    struct vnode *vn = vfs_lookup(abs);
    if (!vn) return -1;
    if (vn->type != VFS_TYPE_DIR) {
        vfs_vnode_put(vn);
        return -1;
    }

    strncpy(cwd, abs, VFS_PATH_MAX - 1);
    cwd[VFS_PATH_MAX - 1] = '\0';
    vfs_vnode_put(vn);
    return 0;
}

/* =========================================================================
 * vfs_dump_mounts
 * ========================================================================= */

void vfs_dump_mounts(void) {
    printf("VFS mount table (%d entries):\n", mount_count);
    for (int i = 0; i < mount_count; i++) {
        if (mounts[i].active) {
            printf("  [%d] %-20s  root_ino=%u\n",
                   i, mounts[i].path,
                   mounts[i].root ? mounts[i].root->ino : 0);
        }
    }
}
