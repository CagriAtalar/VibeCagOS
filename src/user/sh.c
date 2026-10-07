/*
 * sh - VibeCagOS user-space shell (Ring 3).
 *
 * Runs as an ordinary process: every action is a system call (see
 * src/abi/syscall.h). It includes no kernel header and cannot reach any
 * kernel function. System information commands read procfs files through
 * open()/read(); anything that is not a builtin is spawn()ed from the
 * built-in program table and wait()ed for (or left running with a trailing &).
 *
 * Redirection (> and >>) works for builtins; spawned programs always get the
 * console as fd 0/1/2 (fd inheritance comes with pipes).
 */
#include "ulib.h"

#define MAXARGS 16
#define MAXSTAGES 8
#define LINE_MAX 256

static int ofd = 1;     /* where builtins write (1, or a redirected file) */

#define out(...) uprintf(ofd, __VA_ARGS__)

static const char *errstr(int e) {
    switch (-e) {
    case E_PERM:   return "Operation not permitted";
    case E_NOENT:  return "No such file or directory";
    case E_SRCH:   return "No such process";
    case E_IO:     return "I/O error";
    case E_NOEXEC: return "Exec format error";
    case E_BADF:   return "Bad file descriptor";
    case E_CHILD:  return "No child processes";
    case E_NOMEM:  return "Out of memory";
    case E_ACCES:  return "Permission denied";
    case E_FAULT:  return "Bad address";
    case E_EXIST:  return "File exists";
    case E_NOTDIR: return "Not a directory";
    case E_ISDIR:  return "Is a directory";
    case E_INVAL:  return "Invalid argument";
    case E_MFILE:  return "Too many open files";
    case E_NOSPC:  return "No space left on device";
    case E_NAMETOOLONG: return "File name too long";
    case E_NOTEMPTY: return "Directory not empty";
    case E_NOSYS:  return "Function not implemented";
    default:       return "Error";
    }
}
static int fail(const char *cmd, const char *what, int rc) {
    uprintf(2, "%s: %s%s%s\n", cmd, what ? what : "", what ? ": " : "", errstr(rc));
    return 1;
}

/* ---- helpers ------------------------------------------------------------ */
static int copy_fd(int in, int outfd) {
    char b[256]; int n;
    while ((n = sys_read(in, b, sizeof(b))) > 0) {
        int off = 0;
        while (off < n) { int w = sys_write(outfd, b + off, (u32)(n - off)); if (w <= 0) return w ? w : -E_IO; off += w; }
    }
    return n;
}

static int cat_path(const char *cmd, const char *path) {
    int fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return fail(cmd, path, fd);
    int r = copy_fd(fd, ofd);
    sys_close(fd);
    if (r < 0) return fail(cmd, path, r);
    return 0;
}

/* ---- builtins ----------------------------------------------------------- */
static int cmd_ls(int argc, char **argv) {
    int longfmt = 0; const char *path = ".";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) longfmt = 1; else path = argv[i];
    }
    int fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return fail("ls", path, fd);
    struct vibe_dirent d;
    int rc = 0, r;
    while ((r = sys_readdir(fd, &d, 1)) == 1) {
        if (!strcmp(d.name, ".") || !strcmp(d.name, "..")) continue;
        char t = d.type == VIBE_TYPE_DIR ? 'd' : d.type == VIBE_TYPE_CHR ? 'c' : '-';
        if (!longfmt) { out("%c  %s\n", t, d.name); continue; }
        char full[300]; struct vibe_stat st;
        strcpy(full, path);
        if (full[strlen(full) - 1] != '/') strcpy(full + strlen(full), "/");
        strncpy(full + strlen(full), d.name, 200);
        if (sys_stat(full, &st) < 0) { st.size = 0; st.mode = 0; }
        out("%c %03o %6u  %s\n", t, st.mode, st.size, d.name);
    }
    if (r < 0) rc = fail("ls", path, r);
    sys_close(fd);
    return rc;
}

static int cmd_cat(int argc, char **argv) {
    if (argc < 2) { return copy_fd(0, ofd) < 0; }
    int rc = 0;
    for (int i = 1; i < argc; i++) rc |= cat_path("cat", argv[i]);
    return rc;
}

static int cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) out("%s%s", argv[i], i + 1 < argc ? " " : "");
    out("\n");
    return 0;
}

static int cmd_touch(int argc, char **argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_CREAT | O_WRONLY, 0644);
        if (fd < 0) rc |= fail("touch", argv[i], fd); else sys_close(fd);
    }
    return rc;
}

static int cmd_write(int argc, char **argv) {
    if (argc < 3) { uputs(2, "usage: write <file> <text...>\n"); return 1; }
    int fd = sys_open(argv[1], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return fail("write", argv[1], fd);
    for (int i = 2; i < argc; i++) {
        sys_write(fd, argv[i], strlen(argv[i]));
        sys_write(fd, i + 1 < argc ? " " : "\n", 1);
    }
    sys_close(fd);
    return 0;
}

static int cmd_cp(int argc, char **argv) {
    if (argc != 3) { uputs(2, "usage: cp <src> <dst>\n"); return 1; }
    int in = sys_open(argv[1], O_RDONLY, 0);
    if (in < 0) return fail("cp", argv[1], in);
    int o = sys_open(argv[2], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (o < 0) { sys_close(in); return fail("cp", argv[2], o); }
    int r = copy_fd(in, o);
    sys_close(in); sys_close(o);
    return r < 0 ? fail("cp", argv[1], r) : 0;
}

static int cmd_mv(int argc, char **argv) {
    if (argc != 3) { uputs(2, "usage: mv <old> <new>\n"); return 1; }
    int r = sys_rename(argv[1], argv[2]);
    return r < 0 ? fail("mv", argv[1], r) : 0;
}

static int cmd_rm(int argc, char **argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) { int r = sys_unlink(argv[i]); if (r < 0) rc |= fail("rm", argv[i], r); }
    return argc < 2 ? (uputs(2, "usage: rm <file...>\n"), 1) : rc;
}
static int cmd_rmdir(int argc, char **argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) { int r = sys_rmdir(argv[i]); if (r < 0) rc |= fail("rmdir", argv[i], r); }
    return argc < 2 ? (uputs(2, "usage: rmdir <dir...>\n"), 1) : rc;
}
static int cmd_mkdir(int argc, char **argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) { int r = sys_mkdir(argv[i], 0755); if (r < 0) rc |= fail("mkdir", argv[i], r); }
    return argc < 2 ? (uputs(2, "usage: mkdir <dir...>\n"), 1) : rc;
}
static int cmd_cd(int argc, char **argv) {
    int r = sys_chdir(argc > 1 ? argv[1] : "/");
    return r < 0 ? fail("cd", argc > 1 ? argv[1] : "/", r) : 0;
}
static int cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char b[256]; int r = sys_getcwd(b, sizeof(b));
    if (r < 0) return fail("pwd", 0, r);
    out("%s\n", b);
    return 0;
}

static int cmd_stat(int argc, char **argv) {
    if (argc < 2) { uputs(2, "usage: stat <path>\n"); return 1; }
    struct vibe_stat st;
    int r = sys_stat(argv[1], &st);
    if (r < 0) return fail("stat", argv[1], r);
    out("  File: %s\n  Type: %s  Size: %u  Mode: %03o  Links: %u  Inode: %u\n", argv[1],
        st.type == VIBE_TYPE_DIR ? "directory" : st.type == VIBE_TYPE_CHR ? "chardev" : "regular file",
        st.size, st.mode, st.nlink, st.ino);
    return 0;
}

static int cmd_head(int argc, char **argv) {
    int lines = 10, i = 1;
    if (argc > 2 && !strcmp(argv[1], "-n")) { lines = atoi(argv[2]); i = 3; }
    if (i >= argc) { uputs(2, "usage: head [-n N] <file>\n"); return 1; }
    int fd = sys_open(argv[i], O_RDONLY, 0);
    if (fd < 0) return fail("head", argv[i], fd);
    char b[128]; int n, seen = 0;
    while (seen < lines && (n = sys_read(fd, b, sizeof(b))) > 0) {
        int k = 0;
        for (; k < n && seen < lines; k++) if (b[k] == '\n') seen++;
        sys_write(ofd, b, (u32)k);
    }
    sys_close(fd);
    return 0;
}

static int cmd_hexdump(int argc, char **argv) {
    if (argc < 2) { uputs(2, "usage: hexdump <file>\n"); return 1; }
    int fd = sys_open(argv[1], O_RDONLY, 0);
    if (fd < 0) return fail("hexdump", argv[1], fd);
    unsigned char b[16]; int n; u32 off = 0;
    while ((n = sys_read(fd, b, sizeof(b))) > 0) {
        out("%08x  ", off);
        for (int i = 0; i < 16; i++) { if (i < n) out("%02x ", b[i]); else out("   "); }
        out(" |");
        for (int i = 0; i < n; i++) out("%c", b[i] >= 32 && b[i] < 127 ? b[i] : '.');
        out("|\n");
        off += (u32)n;
    }
    sys_close(fd);
    return 0;
}

/* procfs-backed information commands */
static int cmd_proc(const char *name, const char *path) { return cat_path(name, path); }

static int cmd_kill(int argc, char **argv) {
    if (argc < 2) { uputs(2, "usage: kill <pid>\n"); return 1; }
    int r = sys_kill(atoi(argv[1]));
    if (r < 0) return fail("kill", argv[1], r);
    out("kill: terminated pid %d\n", atoi(argv[1]));
    return 0;
}

static int cmd_sleep(int argc, char **argv) {
    sys_sleep(argc > 1 ? (u32)atoi(argv[1]) : 1000);
    return 0;
}

static int cmd_wait(int argc, char **argv) {
    (void)argc; (void)argv;
    int st, pid;
    while ((pid = sys_waitpid(-1, &st)) > 0) out("[pid %d] exit code %d\n", pid, st);
    return 0;
}

static int cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    out("VibeCagOS user shell (Ring 3). Builtins:\n"
        "  ls [-l] [dir]  cd [dir]  pwd  mkdir  rmdir  touch  rm  mv  cp  cat  head [-n N]\n"
        "  echo [text]  write <file> <text>  stat  hexdump\n"
        "  ps  kill <pid>  wait  sleep <ms>  uptime  date  uname  free  mem\n"
        "  cpuinfo  devices  pci  dmesg  mounts  net  clear  help  exit\n"
        "  exec <program> [args...]  replace this shell with another ELF program\n"
        "Syntax:\n"
        "  cmd > file   cmd >> file   redirection (builtins and programs)\n"
        "  cmd1 | cmd2  pipelines between programs, e.g. utest 21 | cat\n"
        "  cmd &        run in the background; 'wait' reaps them\n"
        "Programs (spawned in their own address space, ELF32 images):\n"
        "  utest <n>   Ring-3 self tests\n"
        "  /bin/utest <n>  the same program, named by path\n");
    return 0;
}

/* `exec prog args...`: replace this shell with `prog`. On failure we are still
 * the shell, so the error is reported and the prompt comes back. */
static int cmd_exec(int argc, char **argv) {
    if (argc < 2) { uputs(2, "usage: exec <program> [args...]\n"); return 1; }
    argv[argc] = 0;
    int r = sys_exec(argv[1], (const char *const *)(argv + 1));
    return fail("exec", argv[1], r);
}

/* ---- command dispatch ----------------------------------------------------- */
static int run_builtin(int argc, char **argv, int *found) {
    const char *c = argv[0];
    *found = 1;
    if (!strcmp(c, "ls"))      return cmd_ls(argc, argv);
    if (!strcmp(c, "cat"))     return cmd_cat(argc, argv);
    if (!strcmp(c, "echo"))    return cmd_echo(argc, argv);
    if (!strcmp(c, "touch"))   return cmd_touch(argc, argv);
    if (!strcmp(c, "write"))   return cmd_write(argc, argv);
    if (!strcmp(c, "cp"))      return cmd_cp(argc, argv);
    if (!strcmp(c, "mv"))      return cmd_mv(argc, argv);
    if (!strcmp(c, "rm"))      return cmd_rm(argc, argv);
    if (!strcmp(c, "rmdir"))   return cmd_rmdir(argc, argv);
    if (!strcmp(c, "mkdir"))   return cmd_mkdir(argc, argv);
    if (!strcmp(c, "cd"))      return cmd_cd(argc, argv);
    if (!strcmp(c, "pwd"))     return cmd_pwd(argc, argv);
    if (!strcmp(c, "stat"))    return cmd_stat(argc, argv);
    if (!strcmp(c, "head"))    return cmd_head(argc, argv);
    if (!strcmp(c, "hexdump")) return cmd_hexdump(argc, argv);
    if (!strcmp(c, "kill"))    return cmd_kill(argc, argv);
    if (!strcmp(c, "sleep"))   return cmd_sleep(argc, argv);
    if (!strcmp(c, "wait"))    return cmd_wait(argc, argv);
    if (!strcmp(c, "exec"))    return cmd_exec(argc, argv);
    if (!strcmp(c, "help"))    return cmd_help(argc, argv);
    if (!strcmp(c, "clear"))   return sys_clear();
    if (!strcmp(c, "ps"))      return cmd_proc(c, "/proc/tasks");
    if (!strcmp(c, "uptime"))  return cmd_proc(c, "/proc/uptime");
    if (!strcmp(c, "uname"))   return cmd_proc(c, "/proc/version");
    if (!strcmp(c, "free") || !strcmp(c, "mem")) return cmd_proc(c, "/proc/meminfo");
    if (!strcmp(c, "cpuinfo")) return cmd_proc(c, "/proc/cpuinfo");
    if (!strcmp(c, "dmesg"))   return cmd_proc(c, "/proc/dmesg");
    if (!strcmp(c, "mounts"))  return cmd_proc(c, "/proc/mounts");
    if (!strcmp(c, "net"))     return cmd_proc(c, "/proc/net");
    if (!strcmp(c, "devices")) return cmd_proc(c, "/proc/devices");
    if (!strcmp(c, "pci"))     return cmd_proc(c, "/proc/pci");
    if (!strcmp(c, "date"))    return cmd_proc(c, "/proc/date");
    *found = 0;
    return 0;
}

/*
 * Run one command. `in`/`out`/`err` are the descriptors the program should see
 * as fds 0/1/2; -1 closes that fd. Builtins are handled in-process (their
 * output fd is redirected instead), programs are spawned with those
 * descriptors via SYS_SPAWNFDS.
 */
static int run_one(char **argv, int argc, int in, int out, int err, int background) {
    int found = 0;
    if (out != 1) { ofd = out; run_builtin(argc, argv, &found); ofd = 1; }
    else          run_builtin(argc, argv, &found);
    if (found) return 0;

    argv[argc] = 0;
    int pid = sys_spawnfds(argv[0], (const char *const *)argv, in, out, err);
    if (pid < 0) {
        if (pid == -E_NOENT) uprintf(2, "sh: %s: command not found\n", argv[0]);
        else fail("sh", argv[0], pid);
        return 127;
    }
    if (background) { uprintf(1, "[pid %d] started\n", pid); return 0; }
    int st = 0;
    sys_waitpid(pid, &st);
    uprintf(1, "[pid %d] exit code %d\n", pid, st);
    return st;
}

/* Split a line into words; "double quotes" group. Returns argc. */
static int tokenize(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (argc >= max - 1) break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) *p++ = 0;
    }
    argv[argc] = 0;
    return argc;
}

/*
 * Run a pipeline: `cmd1 | cmd2 [| cmd3 ...]`.
 *
 * Every stage but the last gets a fresh pipe: its fd 1 is the write end and the
 * next stage's fd 0 is the read end. The shell drops its own copies as soon as
 * a stage has been spawned - if it kept a write end open the reader would never
 * see EOF, because EOF means "no write end is open anywhere".
 *
 * Stages are programs, not builtins: a builtin would run inside the shell and
 * read the pipe itself, which needs a different design.
 */
static int run_pipeline(char **argv, int argc) {
    int pids[MAXSTAGES];
    int npids = 0;
    int rc = 0;

    int stages = 1;
    for (int i = 0; i < argc; i++) if (!strcmp(argv[i], "|")) stages++;
    if (stages > MAXSTAGES) { uputs(2, "sh: too many pipeline stages\n"); return 1; }

    int pending = -1;          /* the shell's read end for the stage we spawn */
    int start = 0;
    for (int s = 0; s < stages; s++) {
        int end = start;
        while (end < argc && strcmp(argv[end], "|")) end++;

        int last = (s == stages - 1);
        int in = pending, out = 1, next = -1;
        if (!last) {
            int fds[2];
            if (sys_pipe(fds) < 0) { uputs(2, "sh: pipe failed\n"); return 1; }
            out = fds[1];
            next = fds[0];
        }

        /* the kernel reads argv until NULL, so terminate the stage in place */
        char *saved = argv[end];
        argv[end] = 0;
        int pid = sys_spawnfds(argv[start], (const char *const *)(argv + start),
                               in, out, 2);
        argv[end] = saved;

        if (in >= 0) sys_close(in);
        if (!last) sys_close(out);
        pending = next;

        if (pid < 0) {
            uprintf(2, "sh: %s: cannot run\n", argv[start]);
            rc = 127;
        } else {
            pids[npids++] = pid;
        }
        start = end + 1;
    }

    for (int i = 0; i < npids; i++) {
        int st = 0;
        sys_waitpid(pids[i], &st);
    }
    return rc;
}

static void execute(char *line) {
    char *argv[MAXARGS + 1];
    int argc = tokenize(line, argv, MAXARGS);
    if (argc == 0) return;

    int background = 0;
    if (!strcmp(argv[argc - 1], "&")) { background = 1; argc--; }

    if (!strcmp(argv[0], "exit")) {
        uputs(1, "System halting. Goodbye!\n");
        sys_exit(0);
    }

    /* pipelines: a '|' anywhere turns the line into a pipeline */
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "|")) { run_pipeline(argv, argc); return; }
    }

    /* redirection: cmd > file  |  cmd >> file   (last two words) */
    const char *redir = 0; int append = 0;
    if (argc >= 3 && (!strcmp(argv[argc - 2], ">") || !strcmp(argv[argc - 2], ">>"))) {
        append = argv[argc - 2][1] == '>';
        redir = argv[argc - 1];
        argc -= 2;
    }
    if (argc == 0) return;

    if (redir) {
        int fd = sys_open(redir, O_CREAT | O_WRONLY | (append ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) { fail("sh", redir, fd); return; }
        run_one(argv, argc, 0, fd, 2, background);
        sys_close(fd);
        return;
    }
    run_one(argv, argc, 0, 1, 2, background);
}

int user_main(int argc, char **argv) {
    (void)argc; (void)argv;
    char line[LINE_MAX], cwd[256];
    uputs(1, "\nVibeCagOS 0.5.0 user shell (Ring 3) ready. Type 'help' for commands.\n\n");
    for (;;) {
        if (sys_getcwd(cwd, sizeof(cwd)) < 0) strcpy(cwd, "?");
        uprintf(1, "vcos:%s$ ", cwd);
        if (ugetline(line, sizeof(line)) < 0) return 1;
        execute(line);
    }
}
