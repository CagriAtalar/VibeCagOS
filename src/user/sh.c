/*
 * sh - VibeCagOS user-space shell (Ring 3).
 *
 * Runs as an ordinary process: every action is a system call (see
 * src/abi/syscall.h). It includes no kernel header and cannot reach any
 * kernel function.
 *
 * Almost every command is a separate Ring-3 program in /bin: ls, cat, echo,
 * pwd, head, hexdump, stat, mkdir, rmdir, rm, mv, cp, touch, write, clear,
 * sleep, kill, ps, uname, uptime. The shell only keeps the operations that
 * MUST run in the shell's own process, because they change shell state:
 *
 *   cd     changes this process's cwd
 *   exit   terminates this process
 *   exec   replaces this process image
 *   wait   reaps this shell's background children
 *   help   pure shell text
 *
 * plus a few read-only procfs convenience aliases (free, date, ...) that are
 * equivalent to `cat /proc/<x>`.
 *
 * Redirection (`>`, `>>`) and pipelines (`|`) work for programs, because
 * SYS_SPAWNFDS can hand a child the caller's descriptors; a pipeline stage is
 * therefore always a program, e.g. `ls /bin | head`.
 */
#include "ulib.h"

#define MAXARGS 16
#define MAXSTAGES 8
#define LINE_MAX 256

static int ofd = 1;     /* where builtins write (1, or a redirected file) */

#define out(...) uprintf(ofd, __VA_ARGS__)
#define fail(...) ufail(__VA_ARGS__)

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

/* ---- builtins: only what must run in the shell's own process ------------- */

static int cmd_cd(int argc, char **argv) {
    int r = sys_chdir(argc > 1 ? argv[1] : "/");
    return r < 0 ? fail("cd", argc > 1 ? argv[1] : "/", r) : 0;
}

static int cmd_wait(int argc, char **argv) {
    (void)argc; (void)argv;
    int st, pid;
    while ((pid = sys_waitpid(-1, &st)) > 0) out("[pid %d] exit code %d\n", pid, st);
    return 0;
}

static int cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    out("VibeCagOS user shell (Ring 3).\n"
        "Builtins (must run in the shell): cd, exit, exec, wait, help\n"
        "  cd [dir]              change this shell's working directory\n"
        "  exec <prog> [args...] replace this shell with another program\n"
        "  wait                  reap background children\n"
        "Procfs views (equivalent to `cat /proc/<x>`):\n"
        "  free|mem  cpuinfo  dmesg  mounts  net  devices  pci  date\n"
        "Programs in /bin (separate address spaces):\n"
        "  ls cat echo pwd head hexdump stat touch write\n"
        "  mkdir rmdir rm mv cp clear sleep kill ps uname uptime utest\n"
        "Syntax:\n"
        "  cmd > file   cmd >> file   redirection (programs and builtins)\n"
        "  cmd1 | cmd2  pipeline between programs, e.g. ls /bin | head\n"
        "  cmd &        run in the background; 'wait' reaps them\n"
        "  cmd < file   feed a program's stdin from a file\n");
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
    if (!strcmp(c, "cd"))      return cmd_cd(argc, argv);
    if (!strcmp(c, "wait"))    return cmd_wait(argc, argv);
    if (!strcmp(c, "exec"))    return cmd_exec(argc, argv);
    if (!strcmp(c, "help"))    return cmd_help(argc, argv);
    /* procfs convenience aliases: a one-word way to `cat /proc/<x>` */
    if (!strcmp(c, "free") || !strcmp(c, "mem")) return cat_path(c, "/proc/meminfo");
    if (!strcmp(c, "cpuinfo")) return cat_path(c, "/proc/cpuinfo");
    if (!strcmp(c, "dmesg"))   return cat_path(c, "/proc/dmesg");
    if (!strcmp(c, "mounts"))  return cat_path(c, "/proc/mounts");
    if (!strcmp(c, "net"))     return cat_path(c, "/proc/net");
    if (!strcmp(c, "devices")) return cat_path(c, "/proc/devices");
    if (!strcmp(c, "pci"))     return cat_path(c, "/proc/pci");
    if (!strcmp(c, "date"))    return cat_path(c, "/proc/date");
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

    /* redirection: pull `< file`, `> file` and `>> file` out of the words */
    const char *infile = 0, *outfile = 0;
    int append = 0, w = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "<") && i + 1 < argc) {
            infile = argv[i + 1]; i++;
        } else if ((!strcmp(argv[i], ">") || !strcmp(argv[i], ">>")) && i + 1 < argc) {
            append = argv[i][1] == '>';
            outfile = argv[i + 1]; i++;
        } else {
            argv[w++] = argv[i];
        }
    }
    argc = w;
    argv[argc] = 0;
    if (argc == 0) return;

    int in = 0, outfd = 1;
    if (infile) {
        in = sys_open(infile, O_RDONLY, 0);
        if (in < 0) { fail("sh", infile, in); return; }
    }
    if (outfile) {
        outfd = sys_open(outfile, O_CREAT | O_WRONLY | (append ? O_APPEND : O_TRUNC), 0644);
        if (outfd < 0) { fail("sh", outfile, outfd); if (in > 0) sys_close(in); return; }
    }

    run_one(argv, argc, in, outfd, 2, background);

    if (in > 0) sys_close(in);
    if (outfd != 1) sys_close(outfd);
}

int user_main(int argc, char **argv) {
    (void)argc; (void)argv;
    char line[LINE_MAX], cwd[256];
    uputs(1, "\nVibeCagOS 0.6.0 user shell (Ring 3) ready. Type 'help' for commands.\n\n");
    for (;;) {
        if (sys_getcwd(cwd, sizeof(cwd)) < 0) strcpy(cwd, "?");
        uprintf(1, "vcos:%s$ ", cwd);
        if (ugetline(line, sizeof(line)) < 0) return 1;
        execute(line);
    }
}
