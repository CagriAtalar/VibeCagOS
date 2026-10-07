/*
 * VibeCagOS - program image loading (SYS_SPAWN / SYS_EXEC).
 *
 * Two paths, one loader:
 *
 *   spawn "name"      -> built-in embedded ELF (progs.c)   [Phase 1 staging]
 *   spawn "/bin/x"    -> read the file through the VFS, then the ELF loader
 *   exec  "/bin/x"    -> replace THIS process's address space, like POSIX
 *
 * exec() is the interesting one. It runs on the caller's kernel stack, inside
 * a syscall, so it cannot simply "return into the new program": the new user
 * context has to be built on a kernel stack the current C frames are not
 * using. It therefore:
 *
 *   1. reads and validates the ELF into a scratch buffer (nothing is mapped
 *      yet, so a bad image just returns an error and leaves us untouched)
 *   2. builds a brand-new address space and loads the ELF into it
 *   3. reads the file's fds into the new space and closes everything else
 *   4. swaps current_proc->page_table and CR3 over
 *   5. throws away the current kernel stack frame and returns into a
 *      hand-built iret frame on the same stack, one level below where it is
 *
 * Step 5 is why this is safe: the code between is never returned into. The
 * old C frames simply become garbage that nothing will ever read again, which
 * is the same trick used at process creation.
 */
#include "kernel.h"
#include "vmm.h"
#include "pmm.h"
#include "elf.h"
#include "klog.h"
#include "sysfile.h"
#include "usercopy.h"
#include "../fs/vfs.h"
#include "../abi/syscall.h"

/* Largest program image we will read into a kernel buffer. */
#define EXEC_MAX_IMAGE (512 * 1024)

/* Defined below: spawn with an explicit fd map for the child. */
int sys_spawnfds(const char *uname, const char *const *uargv, const int map[3]);

/* Scratch buffer for the image being loaded. Static on purpose: one exec at a
 * time is enough (a second exec in the same process can only happen after the
 * first one has left this function and entered Ring 3). */
static uint8_t exec_image[EXEC_MAX_IMAGE];

/* Read an absolute VFS path into the scratch buffer. Returns the image and
 * its size, or a negative errno. The caller keeps running on failure. */
static int load_vfs_file(const char *path, const uint8_t **out, uint32_t *out_size) {
    struct vstat st;
    int r = vfs_stat(path, &st);
    if (r < 0) return vfs_err(r);
    if (st.type == VFS_TYPE_DIR) return -E_ISDIR;
    if (st.size == 0 || st.size > sizeof(exec_image)) return -E_NOMEM;

    struct file *f = vfs_open(path, FILE_READ, 0);
    if (!f) return -E_NOENT;
    int n = vfs_read(f, exec_image, st.size);
    vfs_close(f);
    if (n < 0) return vfs_err(n);
    if ((uint32_t)n != st.size) return -E_IO;

    *out = exec_image;
    *out_size = (uint32_t)n;
    return 0;
}

/* Resolve a program name. Paths containing '/' go through the VFS against the
 * caller's cwd. Bare names try /bin/<name> on the disk first and fall back to
 * the embedded table, so the filesystem is authoritative but a deleted /bin
 * entry still boots from the built-in copy. Returns the image and its size,
 * or a negative errno. */
static int load_image(const char *name, const uint8_t **out, uint32_t *out_size) {
    if (strchr(name, '/')) {
        char path[VFS_PATH_MAX];
        int r = path_resolve(current_proc->cwd[0] ? current_proc->cwd : "/",
                             name, path, sizeof(path));
        if (r < 0) return r;
        return load_vfs_file(path, out, out_size);
    }

    /* Bare name: disk first, embedded fallback. stat first so a corrupt /bin
     * file reports its own error instead of silently running stale bytes. */
    char binpath[64];
    int n = 0;
    const char *prefix = "/bin/";
    for (const char *s = prefix; *s && n < (int)sizeof(binpath) - 1; s++) binpath[n++] = *s;
    for (const char *s = name; *s && n < (int)sizeof(binpath) - 1; s++) binpath[n++] = *s;
    binpath[n] = '\0';
    struct vstat st;
    if (vfs_stat(binpath, &st) == 0 && st.size > 0)
        return load_vfs_file(binpath, out, out_size);

    const struct user_prog *prog = user_prog_find(name);
    if (!prog) return -E_NOENT;
    *out      = prog->start;
    *out_size = (uint32_t)(prog->end - prog->start);
    return 0;
}

/*
 * kernel_spawn_path - create a user process from the kernel side (no
 * user pointers involved). Used by kinit to start /sbin/init from the
 * filesystem: the child's ppid is the caller and its cwd is "/".
 * Returns the child, or NULL on failure.
 */
struct process *kernel_spawn_path(const char *path, const char *name,
                                  int argc, const char *const *argv) {
    const uint8_t *image;
    uint32_t size;
    if (load_vfs_file(path, &image, &size) < 0) return NULL;
    struct process *c = process_create_user(name, image, size, argc, argv);
    if (!c) return NULL;
    uint32_t fl = irq_save();
    c->ppid = current_proc ? current_proc->pid : 0;
    strcpy(c->cwd, "/");
    irq_restore(fl);
    return c;
}

/* Copy the caller's argv[] (NULL-terminated array of user strings) into the
 * kernel, exactly as sys_spawn does, so both share one contract. */
int exec_copy_argv(const char *const *uargv, char args[][SPAWN_ARG_LEN],
                   const char **argp, int *out_argc) {
    int argc = 0;
    if (uargv) {
        for (;; argc++) {
            if (argc >= SPAWN_ARGS_MAX) return -E_INVAL;
            const char *up;
            int r = copy_from_user(&up, uargv + argc, sizeof(up));
            if (r < 0) return r;
            if (!up) break;
            r = strncpy_from_user(args[argc], up, SPAWN_ARG_LEN);
            if (r < 0) return r == -E_INVAL ? -E_NAMETOOLONG : r;
            argp[argc] = args[argc];
        }
    }
    *out_argc = argc;
    return 0;
}

int sys_spawn(const char *uname, const char *const *uargv) {
    const int console[3] = { 0, 1, 2 };
    return sys_spawnfds(uname, uargv, console);
}

int sys_spawnfds(const char *uname, const char *const *uargv,
                 const int map[3]) {
    char name[32];
    int r = strncpy_from_user(name, uname, sizeof(name));
    if (r < 0) return r == -E_INVAL ? -E_NAMETOOLONG : r;

    static char args[SPAWN_ARGS_MAX][SPAWN_ARG_LEN];
    const char *argp[SPAWN_ARGS_MAX];
    int argc = 0;
    r = exec_copy_argv(uargv, args, argp, &argc);
    if (r < 0) return r;
    if (argc == 0) { argp[0] = name; argc = 1; }

    const uint8_t *image;
    uint32_t size;
    r = load_image(name, &image, &size);
    if (r < 0) return r;

    /* ps shows the basename: "/bin/sh" runs as "sh". */
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    struct process *c = process_create_user(base, image, size, argc, argp);
    if (!c) return -E_NOMEM;
    uint32_t fl = irq_save();
    c->ppid = current_proc->pid;
    strcpy(c->cwd, current_proc->cwd[0] ? current_proc->cwd : "/");
    fd_inherit_std(c, map);
    irq_restore(fl);
    return c->pid;
}

/*
 * SYS_SPAWNFDS(name, argv, in, out, err) -> pid
 *
 * Same as SYS_SPAWN, but the child's fds 0/1/2 are taken from the caller's
 * descriptor numbers `in`/`out`/`err`; a negative value leaves that fd closed
 * in the child. This is how the shell redirects output and wires up `|`
 * pipelines without a fork()+dup2() pair. See fd_inherit_std().
 */

/* ---- exec --------------------------------------------------------------- */

/* Build the initial user stack in `pd` and hand back the initial ESP.
 * Identical in shape to process_create_user's version (System-V: argc, argv[],
 * NULL, then the strings), factored out so both entry points agree. */
static uint32_t exec_build_stack(uint32_t *pd, int argc, const char *const *argv,
                                 uint32_t *entry_out) {
    uint32_t va = USER_STACK_TOP - 16;
    uint32_t ptrs[SPAWN_ARGS_MAX + 1];

    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        va -= (uint32_t)len;
        if (!ustack_write(pd, va, argv[i], len)) return 0;
        ptrs[i] = va;
    }
    ptrs[argc] = 0;
    va &= ~15u;
    va -= 4u * (uint32_t)(argc + 1);
    if (!ustack_write(pd, va, ptrs, 4u * (uint32_t)(argc + 1))) return 0;
    va -= 4;
    uint32_t n = (uint32_t)argc;
    if (!ustack_write(pd, va, &n, 4)) return 0;
    *entry_out = va;
    return 1;
}

/* Map the user stack pages into a fresh address space. */
static bool exec_map_stack(uint32_t *pd) {
    for (int i = 1; i <= USER_STACK_PAGES; i++) {
        paddr_t frame = pmm_alloc_frame();
        if (!frame) return false;
        vmm_map_page(pd, USER_STACK_TOP - (uint32_t)i * PAGE_SIZE, frame,
                     VMM_FLAG_USER | VMM_FLAG_WRITABLE);
    }
    return true;
}

int sys_exec(const char *upath, const char *const *uargv) {
    char path_in[VFS_PATH_MAX];
    int r = strncpy_from_user(path_in, upath, sizeof(path_in));
    if (r < 0) return r == -E_INVAL ? -E_NAMETOOLONG : r;

    static char args[SPAWN_ARGS_MAX][SPAWN_ARG_LEN];
    const char *argp[SPAWN_ARGS_MAX];
    int argc = 0;
    r = exec_copy_argv(uargv, args, argp, &argc);
    if (r < 0) return r;

    /* Read the image first: everything above this point is side-effect free,
     * so a bad path or a bad ELF leaves the current process running. */
    char path[VFS_PATH_MAX];
    r = path_resolve(current_proc->cwd[0] ? current_proc->cwd : "/",
                     path_in, path, sizeof(path));
    if (r < 0) return r;

    struct vstat st;
    r = vfs_stat(path, &st);
    if (r < 0) return vfs_err(r);
    if (st.type == VFS_TYPE_DIR) return -E_ISDIR;
    if (st.size == 0) return -E_NOEXEC;
    if (st.size > sizeof(exec_image)) return -E_NOMEM;

    struct file *f = vfs_open(path, FILE_READ, 0);
    if (!f) return -E_NOENT;
    int n = vfs_read(f, exec_image, st.size);
    vfs_close(f);
    if (n < 0) return vfs_err(n);
    if ((uint32_t)n != st.size) return -E_IO;

    /* New address space. */
    uint32_t *pd = vmm_create_address_space();
    if (!pd) return -E_NOMEM;

    uint32_t entry = 0;
    if (elf_load(pd, exec_image, (size_t)n, &entry) < 0) {
        KWARN("EXEC", "pid %d: %s rejected: %s", current_proc->pid, path, elf_error());
        vmm_destroy_address_space(pd);
        return -E_NOEXEC;
    }
    if (!exec_map_stack(pd)) { vmm_destroy_address_space(pd); return -E_NOMEM; }

    /* argv[0] defaults to the path we were asked to run. */
    const char *use_argv[SPAWN_ARGS_MAX + 1];
    if (argc == 0) { argp[0] = path; argc = 1; }
    for (int i = 0; i < argc; i++) use_argv[i] = argp[i];

    uint32_t esp = 0;
    if (!exec_build_stack(pd, argc, use_argv, &esp)) {
        vmm_destroy_address_space(pd);
        return -E_NOMEM;
    }

    /* Keep fds 0,1,2 across the exec, drop everything else, exactly like POSIX. */
    struct fdent keep[3];
    for (int i = 0; i < 3; i++) keep[i] = current_proc->fds[i];
    fd_close_all(current_proc);
    for (int i = 0; i < 3; i++) current_proc->fds[i] = keep[i];

    /* Swap address spaces. The kernel half is shared, so the old directory's
     * kernel page tables stay valid and we can destroy it after loading CR3. */
    uint32_t *old_pd = current_proc->page_table;
    current_proc->page_table = pd;
    current_proc->user_entry = entry;
    current_proc->user_esp   = esp;
    current_proc->exit_code  = 0;
    load_cr3((uint32_t)pd);
    if (old_pd) vmm_destroy_address_space(old_pd);

    /* Hand argc to crt0 in EAX and return into Ring 3 through a fresh iret
     * frame at the top of our kernel stack. The C frames below it (including
     * this function) are abandoned: exec_return_to_user() switches onto the
     * frame and never comes back. */
    struct trap_frame *tf = (struct trap_frame *)(kernel_stack_top(current_proc)
                                                 - sizeof(struct trap_frame));
    memset(tf, 0, sizeof(*tf));
    tf->gs = tf->fs = tf->es = tf->ds = SEL_USER_DATA;
    tf->eax      = (uint32_t)argc;
    tf->eip      = entry;
    tf->cs       = SEL_USER_CODE;
    tf->eflags   = 0x202;
    tf->user_esp = esp;
    tf->user_ss  = SEL_USER_DATA;
    exec_return_to_user(tf);
}