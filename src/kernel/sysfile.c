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
#include "pipe.h"
#include "klog.h"
#include "../fs/vfs.h"
#include "../abi/syscall.h"

extern int console_read_blocking(char *buf, size_t max);

#define IO_CHUNK 256

/* ---- fd table ---------------------------------------------------------- */

void fd_init_std(struct process *p) {
    memset(p->fds, 0, sizeof(p->fds));
    p->fds[0] = (struct fdent){ FD_CONSOLE, 1, 0, NULL, NULL };   /* stdin  */
    p->fds[1] = (struct fdent){ FD_CONSOLE, 0, 1, NULL, NULL };   /* stdout */
    p->fds[2] = (struct fdent){ FD_CONSOLE, 0, 1, NULL, NULL };   /* stderr */
}

void fd_close_all(struct process *p) {
    for (int i = 0; i < OPEN_MAX; i++) {
        if (p->fds[i].type == FD_VFS && p->fds[i].file)
            vfs_close(p->fds[i].file);
        else if (p->fds[i].type == FD_PIPE && p->fds[i].pipe) {
            /* Release the end this descriptor actually held. */
            if (p->fds[i].can_read) pipe_close_read(p->fds[i].pipe);
            else                    pipe_close_write(p->fds[i].pipe);
        }
        p->fds[i].type = FD_NONE;
        p->fds[i].file = NULL;
        p->fds[i].pipe = NULL;
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

/* Two lowest free descriptors, for SYS_PIPE. Returns 0, or -EMFILE if the
 * table has fewer than two free slots left. */
static int fd_alloc2(int *lo, int *hi) {
    int first = -1;
    for (int i = 3; i < OPEN_MAX; i++) {
        if (current_proc->fds[i].type != FD_NONE) continue;
        if (first < 0) { first = i; continue; }
        *lo = first; *hi = i;
        return 0;
    }
    return -E_MFILE;
}

/* Allocate a pipe and bind it to two fresh descriptors of the current process.
 * Returns the descriptor pair via rd and wr. On failure nothing is bound. */
static int pipe_bind(int *rd, int *wr) {
    int r = fd_alloc2(rd, wr);
    if (r < 0) return r;
    struct pipe *p = pipe_alloc();
    if (!p) return -E_NOMEM;

    uint32_t fl = irq_save();
    current_proc->fds[*rd] = (struct fdent){ FD_PIPE, 1, 0, NULL, p };
    current_proc->fds[*wr] = (struct fdent){ FD_PIPE, 0, 1, NULL, p };
    irq_restore(fl);
    return 0;
}

int sys_pipe(int *ufds) {
    if (!user_range_valid(ufds, 2 * sizeof(int), true)) return -E_FAULT;

    int rd, wr;
    int r = pipe_bind(&rd, &wr);
    if (r < 0) return r;

    int kfds[2] = { rd, wr };
    r = copy_to_user(ufds, kfds, sizeof(kfds));
    if (r < 0) {          /* validated above, so unreachable; stay defensive */
        sys_close((uint32_t)rd);
        sys_close((uint32_t)wr);
        return r;
    }
    return 0;
}

/* VFS_E* (-1..-11) -> -E_* */
int vfs_err(int r) {
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

/* Make `in` absolute against `cwd` and normalise "." / ".." / "//". */
int path_resolve(const char *cwd, const char *in, char *out, size_t outsz) {
    char tmp[VFS_PATH_MAX];
    size_t n = 0;
    if (in[0] != '/') {
        size_t cl = strlen(cwd);
        if (cl + 1 + strlen(in) + 1 > sizeof(tmp)) return -E_NAMETOOLONG;
        memcpy(tmp, cwd, cl); n = cl;
        if (n == 0 || tmp[n - 1] != '/') tmp[n++] = '/';
    }
    size_t il = strlen(in);
    if (n + il + 1 > sizeof(tmp)) return -E_NAMETOOLONG;
    memcpy(tmp + n, in, il + 1);

    size_t o = 0;
    if (outsz < 2) return -E_NAMETOOLONG;
    out[o++] = '/';
    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t sl = (size_t)(p - seg);
        if (sl == 1 && seg[0] == '.') continue;
        if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
            if (o > 1) { o--; while (o > 1 && out[o - 1] != '/') o--; }
            continue;
        }
        if (o + sl + 1 >= outsz) return -E_NAMETOOLONG;
        if (o > 1) out[o++] = '/';
        memcpy(out + o, seg, sl); o += sl;
    }
    if (o > 1 && out[o - 1] == '/') o--;
    out[o] = 0;
    return 0;
}

static int get_path(const char *upath, char *kbuf) {
    char raw[VFS_PATH_MAX];
    int n = strncpy_from_user(raw, upath, sizeof(raw));
    if (n < 0) return n == -E_INVAL ? -E_NAMETOOLONG : n;
    if (n == 0) return -E_NOENT;
    return path_resolve(current_proc->cwd[0] ? current_proc->cwd : "/", raw, kbuf, VFS_PATH_MAX);
}

/* ---- read / write ------------------------------------------------------ */

int sys_write(uint32_t fd, const void *ubuf, uint32_t len) {
    struct fdent *e = fd_get(fd);
    if (!e || !e->can_write) return -E_BADF;
    if (len == 0) return 0;

    char tmp[IO_CHUNK];
    uint32_t done = 0;
    while (done < len) {
        uint32_t n = len - done;
        if (n > IO_CHUNK) n = IO_CHUNK;
        int r = copy_from_user(tmp, (const char *)ubuf + done, n);
        if (r < 0) return done ? (int)done : r;
        if (e->type == FD_CONSOLE) {
            for (uint32_t i = 0; i < n; i++) putchar(tmp[i]);
        } else if (e->type == FD_PIPE) {
            /* pipe_write may block; it copies through the kernel buffer. */
            int w = pipe_write(e->pipe, tmp, n);
            if (w < 0) return done ? (int)done : w;
            done += (uint32_t)w;
            if ((uint32_t)w < n) break;           /* short write: pipe was full */
            continue;
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
    } else if (e->type == FD_PIPE) {
        n = pipe_read(e->pipe, tmp, len);          /* may block; 0 = EOF */
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
                                            (uint8_t)!!(vf & FILE_WRITE), f, NULL };
    return fd;
}

int sys_close(uint32_t fd) {
    struct fdent *e = fd_get(fd);
    if (!e) return -E_BADF;
    if (e->type == FD_VFS) vfs_close(e->file);
    else if (e->type == FD_PIPE && e->pipe) {
        if (e->can_read) pipe_close_read(e->pipe);
        else              pipe_close_write(e->pipe);
    }
    e->type = FD_NONE; e->file = NULL; e->pipe = NULL;
    e->can_read = e->can_write = 0;
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
    } else if (e->type == FD_PIPE) {
        /* A pipe is not a file: report a FIFO-style character device. */
        memset(&st, 0, sizeof(st));
        st.type = VFS_TYPE_CHRDEV;
    } else {
        int r = vfs_fstat(e->file, &st);
        if (r < 0) return vfs_err(r);
    }
    return put_stat(ustat, &st);
}

/* ---- fd inheritance ----------------------------------------------------- */

/*
 * Replace a freshly created child's fds 0/1/2 with descriptors taken from the
 * CURRENT process.
 *
 *   map[i] = the parent's fd to use as the child's fd i
 *   map[i] < 0                  -> the child's fd i is closed
 *   map[i] == i                 -> the child keeps the default console
 *
 * This is how a shell hands a pipeline or a redirection to a child without
 * needing fork() + dup2(): the parent already holds the pipe ends or the open
 * file, so it just says "your stdout is my fd 7".
 *
 * Everything else in the child's table is closed, so a child can never inherit
 * a half-open file the parent did not mean to share. Pipe ends are reference
 * counted per end, so both processes may hold the same pipe and only the last
 * close frees it.
 *
 * Call with IF=0 (spawn does).
 */
void fd_inherit_std(struct process *child, const int map[3]) {
    struct fdent src[3];
    for (int i = 0; i < 3; i++) {
        int f = map[i];
        src[i] = (f >= 0 && f < OPEN_MAX && current_proc->fds[f].type != FD_NONE)
                     ? current_proc->fds[f]
                     : (struct fdent){ FD_NONE, 0, 0, NULL, NULL };
        if (src[i].type == FD_PIPE && src[i].pipe) {
            if (src[i].can_read) pipe_dup_read(src[i].pipe);
            else                 pipe_dup_write(src[i].pipe);
        }
    }
    fd_close_all(child);
    for (int i = 0; i < 3; i++) child->fds[i] = src[i];
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

/* Change a file's size, freeing the blocks that fall off the end. */
int sys_truncate(const char *upath, uint32_t size) {
    char path[VFS_PATH_MAX];
    int r = get_path(upath, path);
    if (r < 0) return r;
    return vfs_err(vfs_truncate(path, size));
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

/* ---- cwd / rename ------------------------------------------------------ */

int sys_chdir(const char *upath) {
    char path[VFS_PATH_MAX];
    int r = get_path(upath, path);
    if (r < 0) return r;
    struct vstat st;
    r = vfs_stat(path, &st);
    if (r < 0) return vfs_err(r);
    if (st.type != VFS_TYPE_DIR) return -E_NOTDIR;
    if (strlen(path) >= PROC_CWD_MAX) return -E_NAMETOOLONG;
    strcpy(current_proc->cwd, path);
    return 0;
}

int sys_getcwd(char *ubuf, uint32_t size) {
    const char *cwd = current_proc->cwd[0] ? current_proc->cwd : "/";
    uint32_t n = (uint32_t)strlen(cwd) + 1;
    if (size < n) return -E_INVAL;
    int r = copy_to_user(ubuf, cwd, n);
    return r < 0 ? r : (int)(n - 1);
}

int sys_rename(const char *uold, const char *unew) {
    char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
    int r = get_path(uold, a);
    if (r < 0) return r;
    r = get_path(unew, b);
    if (r < 0) return r;
    return vfs_err(vfs_rename(a, b));
}

/* ---- process control --------------------------------------------------- */
/* SYS_SPAWN and SYS_EXEC live in exec.c: both resolve a program image and load
 * it, and keeping them together makes the one-loader claim checkable. */

int sys_waitpid(int pid, int *ustatus) {
    if (ustatus && !user_range_valid(ustatus, sizeof(int), true)) return -E_FAULT;
    int status = 0;
    int r = process_waitpid(pid, &status);          /* may block */
    if (r > 0 && ustatus) copy_to_user(ustatus, &status, sizeof(status));
    return r;
}

int sys_kill(int pid) {
    if (pid == current_proc->pid) return -E_INVAL;
    int r = process_kill(pid);
    if (r == 0) return 0;
    if (r == -1) return -E_PERM;
    if (r == -2) return 0;                          /* already a zombie */
    return -E_SRCH;
}
