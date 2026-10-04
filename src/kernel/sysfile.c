/*
 * VibeCagOS - per-process file descriptors and the file syscalls.
 *
 *   user fd -> current_proc->fds[fd] -> { console | struct file } -> VFS -> fs
 *
 * fd 0/1/2 are bound to the kernel console (TTY) when a process is created,
 * so user code never touches VGA / serial / port 0x60 directly.
 * Paths come from user memory via strncpy_from_user(). The VFS cwd is still
 * global (known limitation, see docs).
 */
#include "kernel.h"
#include "usercopy.h"
#include "sysfile.h"
#include "klog.h"
#include "../fs/vfs.h"
#include "../abi/syscall.h"

extern int console_read_blocking(char *buf, size_t max);

#define IO_CHUNK 256

/* ---- fd table ---------------------------------------------------------- */

void fd_init_std(struct process *p) {
    memset(p->fds, 0, sizeof(p->fds));
    p->fds[0] = (struct fdent){ FD_CONSOLE, 1, 0, NULL };   /* stdin  */
    p->fds[1] = (struct fdent){ FD_CONSOLE, 0, 1, NULL };   /* stdout */
    p->fds[2] = (struct fdent){ FD_CONSOLE, 0, 1, NULL };   /* stderr */
}

void fd_close_all(struct process *p) {
    for (int i = 0; i < OPEN_MAX; i++) {
        if (p->fds[i].type == FD_VFS && p->fds[i].file)
            vfs_close(p->fds[i].file);
        p->fds[i].type = FD_NONE;
        p->fds[i].file = NULL;
    }
}

static struct fdent *fd_get(uint32_t fd) {
    if (fd >= OPEN_MAX) return NULL;
    struct fdent *e = &current_proc->fds[fd];
    return e->type == FD_NONE ? NULL : e;
}

static int fd_alloc(void) {
    for (int i = 3; i < OPEN_MAX; i++)
        if (current_proc->fds[i].type == FD_NONE) return i;
    return -E_MFILE;
}

/* VFS_E* (-1..-11) -> -E_* */
static int vfs_err(int r) {
    if (r >= 0) return r;
    switch (r) {
    case VFS_ENOENT:       return -E_NOENT;
    case VFS_EEXIST:       return -E_EXIST;
    case VFS_ENOTDIR:      return -E_NOTDIR;
    case VFS_EISDIR:       return -E_ISDIR;
    case VFS_ENOMEM:       return -E_NOMEM;
    case VFS_EIO:          return -E_IO;
    case VFS_ENOSPC:       return -E_NOSPC;
    case VFS_ENOTEMPTY:    return -E_NOTEMPTY;
    case VFS_EINVAL:       return -E_INVAL;
    case VFS_EACCES:       return -E_ACCES;
    case VFS_ENAMETOOLONG: return -E_NAMETOOLONG;
    default:               return -E_IO;
    }
}

static int get_path(const char *upath, char *kbuf) {
    int n = strncpy_from_user(kbuf, upath, VFS_PATH_MAX);
    if (n < 0) return n == -E_INVAL ? -E_NAMETOOLONG : n;
    if (n == 0) return -E_NOENT;
    return 0;
}

/* ---- read / write ------------------------------------------------------ */

int sys_write(uint32_t fd, const void *ubuf, uint32_t len) {
    struct fdent *e = fd_get(fd);
    if (!e || !e->can_write) return -E_BADF;

    char tmp[IO_CHUNK];
    uint32_t done = 0;
    while (done < len) {
        uint32_t n = len - done;
        if (n > IO_CHUNK) n = IO_CHUNK;
        int r = copy_from_user(tmp, (const char *)ubuf + done, n);
        if (r < 0) return done ? (int)done : r;
        if (e->type == FD_CONSOLE) {
            for (uint32_t i = 0; i < n; i++) putchar(tmp[i]);
        } else {
            int w = vfs_write(e->file, tmp, n);
            if (w < 0) return done ? (int)done : vfs_err(w);
            done += (uint32_t)w;
            if ((uint32_t)w < n) break;           /* short write */
            continue;
        }
        done += n;
    }
    return (int)done;
}

int sys_read(uint32_t fd, void *ubuf, uint32_t len) {
    struct fdent *e = fd_get(fd);
    if (!e || !e->can_read) return -E_BADF;
    if (len == 0) return 0;
    if (len > IO_CHUNK) len = IO_CHUNK;
    /* Fail early so a bad pointer doesn't swallow keystrokes / file data. */
    if (!user_range_valid(ubuf, len, true)) return -E_FAULT;

    char tmp[IO_CHUNK];
    int n;
    if (e->type == FD_CONSOLE) {
        n = console_read_blocking(tmp, len);       /* may block */
    } else {
        n = vfs_read(e->file, tmp, len);
        if (n < 0) return vfs_err(n);
    }
    int r = copy_to_user(ubuf, tmp, (size_t)n);
    return r < 0 ? r : n;
}

/* ---- open / close / lseek ---------------------------------------------- */

int sys_open(const char *upath, uint32_t flags, uint32_t mode) {
    char path[VFS_PATH_MAX];
    int r = get_path(upath, path);
    if (r < 0) return r;

    int fd = fd_alloc();
    if (fd < 0) return fd;

    uint32_t acc = flags & 3, vf = 0;
    if (acc == O_RDONLY)      vf = FILE_READ;
    else if (acc == O_WRONLY) vf = FILE_WRITE;
    else if (acc == O_RDWR)   vf = FILE_READ | FILE_WRITE;
    else return -E_INVAL;
    if (flags & O_APPEND) vf |= FILE_APPEND;

    uint16_t cm = (flags & O_CREAT) ? (uint16_t)(mode ? (mode & 0777) : VFS_PERM_DEFAULT_FILE) : 0;
    if ((flags & O_CREAT) && !(vf & FILE_WRITE)) return -E_INVAL;

    if ((flags & O_TRUNC) && (vf & FILE_WRITE)) {
        struct vstat st;
        if (vfs_stat(path, &st) == 0) {
            if (st.type == VFS_TYPE_DIR) return -E_ISDIR;
            int t = vfs_truncate(path, 0);
            if (t < 0) return vfs_err(t);
        }
    }

    struct file *f = vfs_open(path, vf, cm);
    if (!f) return (flags & O_CREAT) ? -E_IO : -E_NOENT;

    /* Only directories may be opened read-only for readdir; no writing. */
    if ((f->flags & FILE_DIR) && (vf & FILE_WRITE)) { vfs_close(f); return -E_ISDIR; }

    current_proc->fds[fd] = (struct fdent){ FD_VFS, (uint8_t)!!(vf & FILE_READ),
                                            (uint8_t)!!(vf & FILE_WRITE), f };
    return fd;
}

int sys_close(uint32_t fd) {
    struct fdent *e = fd_get(fd);
    if (!e) return -E_BADF;
    if (e->type == FD_VFS) vfs_close(e->file);
    e->type = FD_NONE; e->file = NULL; e->can_read = e->can_write = 0;
    return 0;
}

int sys_lseek(uint32_t fd, int off, uint32_t whence) {
    struct fdent *e = fd_get(fd);
    if (!e || e->type != FD_VFS) return -E_BADF;
    return vfs_err(vfs_seek(e->file, off, (int)whence));
}

/* ---- stat -------------------------------------------------------------- */

static int put_stat(void *ustat, const struct vstat *st) {
    struct vibe_stat o = { st->ino, st->type, st->mode, st->size, st->nlink };
    return copy_to_user(ustat, &o, sizeof(o));
}

int sys_stat(const char *upath, void *ustat) {
    char path[VFS_PATH_MAX];
    int r = get_path(upath, path);
    if (r < 0) return r;
    struct vstat st;
    r = vfs_stat(path, &st);
    if (r < 0) return vfs_err(r);
    return put_stat(ustat, &st);
}

int sys_fstat(uint32_t fd, void *ustat) {
    struct fdent *e = fd_get(fd);
    if (!e) return -E_BADF;
    struct vstat st;
    if (e->type == FD_CONSOLE) {
        memset(&st, 0, sizeof(st));
        st.type = VFS_TYPE_CHRDEV;
    } else {
        int r = vfs_fstat(e->file, &st);
        if (r < 0) return vfs_err(r);
    }
    return put_stat(ustat, &st);
}

/* ---- namespace operations ---------------------------------------------- */

int sys_mkdir(const char *upath, uint32_t mode) {
    char path[VFS_PATH_MAX];
    int r = get_path(upath, path);
    if (r < 0) return r;
    return vfs_err(vfs_mkdir(path, mode ? (uint16_t)(mode & 0777) : VFS_PERM_DEFAULT_DIR));
}

int sys_unlink(const char *upath) {
    char path[VFS_PATH_MAX];
    int r = get_path(upath, path);
    if (r < 0) return r;
    return vfs_err(vfs_unlink(path));
}

int sys_rmdir(const char *upath) {
    char path[VFS_PATH_MAX];
    int r = get_path(upath, path);
    if (r < 0) return r;
    return vfs_err(vfs_rmdir(path));
}

/* Returns 1 and fills *udirent, or 0 at end of directory. */
int sys_readdir(uint32_t fd, void *udirent, uint32_t max) {
    struct fdent *e = fd_get(fd);
    if (!e || e->type != FD_VFS) return -E_BADF;
    if (!(e->file->flags & FILE_DIR)) return -E_NOTDIR;
    if (max < 1) return -E_INVAL;
    if (!user_range_valid(udirent, sizeof(struct vibe_dirent), true)) return -E_FAULT;

    struct dirent d;
    int n = vfs_readdir(e->file, &d, 1);
    if (n < 0) return vfs_err(n);
    if (n == 0) return 0;

    struct vibe_dirent o;
    memset(&o, 0, sizeof(o));
    o.ino = d.ino; o.type = d.type;
    strncpy(o.name, d.name, VIBE_NAME_MAX);
    int r = copy_to_user(udirent, &o, sizeof(o));
    return r < 0 ? r : 1;
}
