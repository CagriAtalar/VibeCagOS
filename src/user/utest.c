/*
 * utest - Ring-3 self-test program. `utest <n>` (kernel shell) starts it with
 * n delivered as the argument. Every test prints a line starting "T<n>:" so
 * tests/run.sh can grep deterministic results from the serial console.
 */
#include "ulib.h"

static int slen(const char *s) { return (int)strlen(s); }
static void puts_(const char *s) { sys_write(1, s, (u32)slen(s)); }
static void putint(int v) {
    char b[16]; int i = 15; b[i] = 0;
    int neg = v < 0; unsigned u = neg ? -(unsigned)v : (unsigned)v;
    do { b[--i] = '0' + u % 10; u /= 10; } while (u);
    if (neg) b[--i] = '-';
    puts_(b + i);
}
static void busy(volatile unsigned n) { while (n--) ; }

static const char rodata_probe[16] = "ro";   /* lives in read-only pages */
static volatile int data_probe = 1234;        /* read-write page */
static int bss_probe;                         /* BSS: loader must zero this */

int user_main(int argc, char **argv) {
    int test = argc > 1 ? atoi(argv[1]) : -1;
    switch (test) {
    case 0: {   /* Ring 3 entry: CS/SS RPL must be 3, interrupts enabled */
        unsigned cs, ss, fl;
        __asm__ __volatile__("mov %%cs,%0; mov %%ss,%1; pushf; pop %2" : "=r"(cs), "=r"(ss), "=r"(fl));
        puts_("T0: hello from ring3\n");
        puts_("T0: cs&3="); putint(cs & 3); puts_(" ss&3="); putint(ss & 3);
        puts_(" IF="); putint((fl >> 9) & 1); puts_("\n");
        return 0;
    }
    case 1: {   /* syscalls: getpid, putchar, uptime, invalid number */
        puts_("T1: getpid="); putint(sys_getpid() > 0); puts_("\n");
        puts_("T1: putchar="); sys_putchar('X'); puts_("\n");
        puts_("T1: uptime_ok="); putint(sys_uptime() >= 0); puts_("\n");
        puts_("T1: invalid_syscall="); putint(syscall3(9999, 0, 0, 0)); puts_("\n");
        puts_("T1: write_badfd="); putint(sys_write(7, "x", 1)); puts_("\n");
        return 0;
    }
    case 2: {   /* user data/rodata are readable, data is writable, BSS is zero */
        data_probe++;
        puts_("T2: data="); putint(data_probe); puts_(" ro0="); putint(rodata_probe[0]);
        puts_(" bss0="); putint(bss_probe); puts_("\n");
        return bss_probe != 0;
    }
    case 3: {   /* Ring 3 reads kernel memory -> page fault, only we die */
        puts_("T3: before kernel read\n");
        volatile unsigned *k = (volatile unsigned *)0x00100000;
        unsigned v = *k;
        puts_("T3: SURVIVED "); putint((int)v); puts_("\n");   /* must not appear */
        return 0;
    }
    case 4: {   /* preemption: A (no yield) */
        for (int i = 0; i < 40; i++) { sys_putchar('A'); busy(6000000); }
        return 10;
    }
    case 5: {   /* preemption: B (no yield) */
        for (int i = 0; i < 40; i++) { sys_putchar('B'); busy(6000000); }
        return 11;
    }
    case 6: {   /* sleep ~300 ms, report elapsed */
        int t0 = sys_uptime();
        sys_sleep(300);
        int dt = sys_uptime() - t0;
        puts_("T6: slept_ok="); putint(dt >= 290 && dt < 600); puts_(" dt>=290\n");
        return 0;
    }
    case 7: {   /* usercopy validation: every bad pointer must return -E_FAULT */
        struct vibe_info info;
        puts_("T7: write_null="); putint(sys_write(1, (void *)0, 5)); puts_("\n");
        puts_("T7: write_kernel="); putint(sys_write(1, (void *)0x00100000, 16)); puts_("\n");
        puts_("T7: write_unmapped="); putint(sys_write(1, (void *)0x30000000, 4)); puts_("\n");
        puts_("T7: write_wrap="); putint(sys_write(1, (void *)0xFFFFFFF0, 64)); puts_("\n");
        /* buffer straddles the top of the stack: 2nd page unmapped */
        puts_("T7: cross_page_stack="); putint(sys_getinfo((void *)(0x20000000 - 4))); puts_("\n");
        /* destination in a read-only (text) page */
        puts_("T7: readonly_dst="); putint(sys_getinfo((void *)rodata_probe)); puts_("\n");
        puts_("T7: getinfo_ok="); putint(sys_getinfo(&info)); puts_(" pid_ok="); putint(info.pid == (u32)sys_getpid()); puts_("\n");
        /* valid buffer straddling two mapped stack pages must work */
        static char big[1]; (void)big;
        puts_("T7: done\n");
        return 0;
    }
    case 8: {   /* privileged instruction -> #GP -> killed */
        puts_("T8: before cli\n");
        __asm__ __volatile__("cli");
        puts_("T8: SURVIVED\n");
        return 0;
    }
    case 9: {   /* port I/O from Ring 3 -> #GP -> killed */
        puts_("T9: before outb\n");
        __asm__ __volatile__("outb %0, %1" : : "a"((unsigned char)0xFF), "Nd"((unsigned short)0x21));
        puts_("T9: SURVIVED\n");
        return 0;
    }
    case 10: {  /* fake a hardware interrupt vector: must be #GP, not run the ISR */
        puts_("T10: before int 0x20\n");
        __asm__ __volatile__("int $0x20");
        puts_("T10: SURVIVED\n");
        return 0;
    }
    case 11: {  /* write to read-only text page */
        puts_("T11: before write to text\n");
        *(volatile char *)(0x10000000 + 8) = 1;
        puts_("T11: SURVIVED\n");
        return 0;
    }
    case 13: {  /* stdio fds: stdout/stderr writable, stdin not, fstat says chardev */
        struct vibe_stat st;
        puts_("T13: stdout="); putint(sys_write(1, "ok", 2)); puts_("\n");
        puts_("T13: stderr="); putint(sys_write(2, "ok", 2)); puts_("\n");
        puts_("T13: write_stdin="); putint(sys_write(0, "x", 1)); puts_("\n");
        puts_("T13: read_stdout="); { char c; putint(sys_read(1, &c, 1)); } puts_("\n");
        puts_("T13: fstat0="); putint(sys_fstat(0, &st)); puts_(" type="); putint((int)st.type); puts_("\n");
        puts_("T13: close_bad="); putint(sys_close(99)); puts_("\n");
        return 0;
    }
    case 14: {  /* stdin: blocking read from the console (keyboard/serial) */
        char buf[16];
        puts_("T14: waiting\n");
        int n = sys_read(0, buf, sizeof(buf));
        puts_("T14: read="); putint(n); puts_(" data=");
        if (n > 0) sys_write(1, buf, (u32)(n > 0 && buf[n-1] == '\n' ? n - 1 : n));
        puts_("\n");
        return 0;
    }
    case 15: {  /* file I/O through fds */
        char buf[32]; struct vibe_stat st; int fd, n;
        fd = sys_open("/t15.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        puts_("T15: open_create="); putint(fd); puts_("\n");
        puts_("T15: write="); putint(sys_write(fd, "hello fs", 8)); puts_("\n");
        puts_("T15: read_on_wronly="); putint(sys_read(fd, buf, 4)); puts_("\n");
        puts_("T15: close="); putint(sys_close(fd)); puts_("\n");
        puts_("T15: close_again="); putint(sys_close(fd)); puts_("\n");

        fd = sys_open("/t15.txt", O_RDONLY, 0);
        n = sys_read(fd, buf, sizeof(buf));
        puts_("T15: read="); putint(n); puts_(" data="); sys_write(1, buf, (u32)(n > 0 ? n : 0)); puts_("\n");
        puts_("T15: write_on_rdonly="); putint(sys_write(fd, "x", 1)); puts_("\n");
        puts_("T15: lseek="); putint(sys_lseek(fd, 6, SEEK_SET));
        n = sys_read(fd, buf, sizeof(buf));
        puts_(" tail="); sys_write(1, buf, (u32)(n > 0 ? n : 0)); puts_("\n");
        puts_("T15: fstat="); putint(sys_fstat(fd, &st)); puts_(" size="); putint((int)st.size);
        puts_(" type="); putint((int)st.type); puts_("\n");
        sys_close(fd);

        puts_("T15: stat="); putint(sys_stat("/t15.txt", &st)); puts_(" size="); putint((int)st.size); puts_("\n");
        puts_("T15: open_missing="); putint(sys_open("/nope", O_RDONLY, 0)); puts_("\n");
        puts_("T15: stat_missing="); putint(sys_stat("/nope", &st)); puts_("\n");
        puts_("T15: open_badptr="); putint(sys_open((const char *)0x00100000, O_RDONLY, 0)); puts_("\n");
        puts_("T15: stat_badbuf="); putint(sys_stat("/t15.txt", (struct vibe_stat *)0x00100000)); puts_("\n");

        /* append + truncate */
        fd = sys_open("/t15.txt", O_WRONLY | O_APPEND, 0);
        sys_write(fd, "!!", 2); sys_close(fd);
        sys_stat("/t15.txt", &st); puts_("T15: after_append_size="); putint((int)st.size); puts_("\n");
        fd = sys_open("/t15.txt", O_WRONLY | O_TRUNC, 0); sys_close(fd);
        sys_stat("/t15.txt", &st); puts_("T15: after_trunc_size="); putint((int)st.size); puts_("\n");

        /* directories */
        puts_("T15: mkdir="); putint(sys_mkdir("/d15", 0755)); puts_("\n");
        puts_("T15: mkdir_again="); putint(sys_mkdir("/d15", 0755)); puts_("\n");
        sys_stat("/d15", &st); puts_("T15: dir_type="); putint((int)st.type); puts_("\n");
        fd = sys_open("/", O_RDONLY, 0);
        { struct vibe_dirent d; int found = 0, cnt = 0;
          while (sys_readdir(fd, &d, 1) == 1 && cnt < 64) { cnt++;
              if (d.name[0]=='d' && d.name[1]=='1' && d.name[2]=='5' && d.name[3]==0) found = 1; }
          puts_("T15: readdir_found_d15="); putint(found); puts_("\n"); }
        puts_("T15: write_dir="); putint(sys_open("/d15", O_WRONLY, 0)); puts_("\n");
        sys_close(fd);
        puts_("T15: rmdir="); putint(sys_rmdir("/d15")); puts_("\n");
        puts_("T15: unlink="); putint(sys_unlink("/t15.txt")); puts_("\n");
        puts_("T15: stat_after_unlink="); putint(sys_stat("/t15.txt", &st)); puts_("\n");
        return 0;
    }
    case 16: {  /* fd exhaustion: 13 free slots (3..15), then EMFILE; close all */
        int fds[20], n = 0, last = 0;
        for (int i = 0; i < 20; i++) { last = sys_open("/", O_RDONLY, 0); if (last < 0) break; fds[n++] = last; }
        puts_("T16: opened="); putint(n); puts_(" then="); putint(last); puts_("\n");
        for (int i = 0; i < n; i++) sys_close(fds[i]);
        puts_("T16: reopen="); putint(sys_open("/", O_RDONLY, 0)); puts_("\n");
        return 0;     /* exits WITH an fd still open: kernel must release it */
    }
    case 17: {  /* long sleeper: target for kill/wait tests */
        for (;;) sys_sleep(1000);
    }
    case 18: {  /* spawn/wait from Ring 3: parent starts 2 children, reaps both */
        const char *a1[] = { "utest", "12", 0 };
        const char *a2[] = { "utest", "0", 0 };
        int p1 = sys_spawn("utest", a1), p2 = sys_spawn("utest", a2);
        int ok = p1 > 0 && p2 > 0 && p1 != p2;
        int st1 = -1, st2 = -1, r1 = sys_waitpid(p1, &st1), r2 = sys_waitpid(-1, &st2);
        /* Print only after both children are reaped: while they are alive
         * their own output can interleave ours on the serial line. */
        puts_("T18: spawned="); putint(ok); puts_("\n");
        puts_("T18: wait1="); putint(r1 == p1); puts_(" st1="); putint(st1);
        puts_(" wait2="); putint(r2 == p2); puts_(" st2="); putint(st2); puts_("\n");
        puts_("T18: no_more_children="); putint(sys_waitpid(-1, &st1)); puts_("\n");
        puts_("T18: spawn_missing="); putint(sys_spawn("nonexistent", 0)); puts_("\n");
        puts_("T18: spawn_badptr="); putint(sys_spawn((const char *)0x00100000, 0)); puts_("\n");
        puts_("T18: wait_badptr="); putint(sys_waitpid(-1, (int *)0x00100000)); puts_("\n");
        return 7;
    }
    case 19: {  /* per-process cwd: chdir in a child must not affect the parent */
        char b[64];
        puts_("T19: chdir="); putint(sys_chdir("/proc")); puts_("\n");
        sys_getcwd(b, sizeof(b)); puts_("T19: cwd="); puts_(b); puts_("\n");
        int fd = sys_open("version", O_RDONLY, 0);       /* relative to /proc */
        puts_("T19: relative_open_ok="); putint(fd >= 3); puts_("\n");
        puts_("T19: chdir_file="); putint(sys_chdir("/proc/version")); puts_("\n");
        puts_("T19: dotdot="); putint(sys_chdir("../proc/../")); sys_getcwd(b, sizeof(b)); puts_(" "); puts_(b); puts_("\n");
        return 0;
    }
    case 20: {  /* parent exits while its child still runs -> child is orphaned */
        const char *a[] = { "utest", "17", 0 };
        int c = sys_spawn("utest", a);
        puts_("T20: child_spawned="); putint(c > 0); puts_("\n");
        return 0;                      /* exits WITHOUT waiting */
    }
    case 21: {  /* pipes: round-trip, EOF, short write on a full buffer */
        int fds[2]; char buf[32];
        puts_("T21: pipe="); putint(sys_pipe(fds)); puts_("\n");
        puts_("T21: fds="); putint(fds[0]); puts_(","); putint(fds[1]); puts_("\n");
        puts_("T21: write="); putint(sys_write(fds[1], "pipe hello", 10)); puts_("\n");
        int n = sys_read(fds[0], buf, sizeof(buf));
        puts_("T21: read="); putint(n); puts_(" data=");
        sys_write(1, buf, (u32)(n > 0 ? n : 0)); puts_("\n");

        /* closing the write end must give the reader EOF (0), not hang */
        sys_close(fds[1]);
        puts_("T21: eof_after_close="); putint(sys_read(fds[0], buf, sizeof(buf)) == 0); puts_("\n");

        /* Fill the pipe exactly to capacity: this is the largest write a
         * single process may make without a concurrent reader, because a
         * pipe blocks once it is full. */
        int g[2]; sys_pipe(g);
        static char big[VIBE_PIPE_SIZE];
        for (int i = 0; i < VIBE_PIPE_SIZE; i++) big[i] = (char)('a' + (i & 15));
        int w = sys_write(g[1], big, VIBE_PIPE_SIZE);
        puts_("T21: full_write="); putint(w == VIBE_PIPE_SIZE); puts_(" w="); putint(w); puts_("\n");
        sys_close(g[1]);
        /* everything written before the close must still be readable */
        static char out[VIBE_PIPE_SIZE];
        int total = 0, r2;
        while (total < VIBE_PIPE_SIZE && (r2 = sys_read(g[0], out + total, VIBE_PIPE_SIZE - total)) > 0)
            total += r2;
        puts_("T21: drain_ok="); putint(total == VIBE_PIPE_SIZE); puts_(" got="); putint(total); puts_("\n");
        puts_("T21: eof="); putint(sys_read(g[0], out, 1) == 0); puts_("\n");
        sys_close(g[0]);

        /* bad pointer must be rejected with EFAULT, not crash the kernel */
        puts_("T21: badptr="); putint(sys_pipe((int *)0x00100000)); puts_("\n");
        return 0;
    }
    case 22: {  /* exec: hand argv to the new image, leak an fd on the way out */
        puts_("T22: argv0="); puts_(argc > 0 ? argv[0] : "(none)"); puts_("\n");
        int fd = sys_open("/t22.txt", O_CREAT | O_WRONLY, 0644);
        puts_("T22: open_fd="); putint(fd); puts_("\n");
        /* exec replaces this process; only the new program's output follows. */
        const char *a[] = { "/bin/utest", "23", "extra", 0 };
        puts_("T22: exec="); putint(sys_exec("/bin/utest", a)); puts_("\n");
        puts_("T22: SURVIVED\n");            /* must not appear */
        return 0;
    }
    case 23: {  /* running as the image exec(2) started: argv and fds */
        puts_("T23: argc="); putint(argc);
        puts_(" argv0="); puts_(argc > 0 ? argv[0] : "(none)");
        puts_(" argv1="); puts_(argc > 1 ? argv[1] : "(none)");
        puts_(" argv2="); puts_(argc > 2 ? argv[2] : "(none)");
        puts_("\n");
        /* POSIX exec keeps 0,1,2 and closes everything else. */
        puts_("T23: leaked_fd="); putint(sys_close(3)); puts_("\n");
        puts_("T23: stdout="); putint(sys_fstat(1, 0) == -14); puts_("\n");
        puts_("T23: hello from the exec'd image\n");
        return 55;
    }
    case 24: {  /* exec rejections: each must fail cleanly, leaving us alive */
        puts_("T24: missing="); putint(sys_exec("/nope", 0)); puts_("\n");
        puts_("T24: dir="); putint(sys_exec("/proc", 0)); puts_("\n");
        /* a text file is not a VBIN executable */
        int fd = sys_open("/etc/version", O_RDONLY, 0);
        puts_("T24: open_text="); putint(fd >= 3); puts_("\n");
        if (fd >= 3) sys_close(fd);
        puts_("T24: notelf="); putint(sys_exec("/etc/version", 0)); puts_("\n");
        puts_("T24: badptr="); putint(sys_exec((const char *)0x00100000, 0)); puts_("\n");
        puts_("T24: alive\n");
        return 0;
    }
    case 27: {  /* malformed VBIN images: exec must fail cleanly, never harm us */
        /* VBIN header words: magic version entry text data mem flags */
        static const unsigned good[7] =
            { 0x4E494256u, 1, 0x10000000u, 0x1000, 0, 0x1000, 0 };
        static unsigned bad[7];
        static char payload[0x1000];
        for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = (char)('A' + (i & 15));
        int fails = 0;
        /* each row: {word to corrupt, corrupt value, payload bytes, tag} */
        static const struct { int idx; unsigned val; unsigned pay; const char *tag; } rows[] = {
            { 0, 0xDEADBEEFu, 64, "magic" },     /* bad magic */
            { -1, 0, 10, "trunc" },              /* truncated header */
            { 1, 2, 64, "version" },             /* bad version */
            { 6, 1, 64, "flags" },               /* bad flags */
            { 5, 0, 64, "mem0" },                /* mem_size == 0 */
            { 5, 0x2000000u, 0x1000, "memhuge" },/* mem_size past the 1 MiB cap */
            { 3, 0, 64, "text0" },               /* empty text */
            { 2, 0x00100000u, 64, "entrykern" }, /* entry below USER_BASE */
            { 2, 0x10001000u, 64, "entrydata" }, /* entry in data, not text */
            { 3, 0x2000, 64, "textover" },       /* text past end of file */
            { 4, 0x2000, 64, "dataover" },       /* text+data past end of file */
            { 5, 0x100, 64, "memsmall" },        /* text+data larger than mem */
        };
        for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
            for (int w = 0; w < 7; w++) bad[w] = good[w];
            if (rows[i].idx >= 0) bad[rows[i].idx] = rows[i].val;
            char path[16] = "/t27-0.vbn";
            path[5] = (char)('a' + i);
            int fd = sys_open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd < 0) { puts_("T27: open fail\n"); return 1; }
            sys_write(fd, bad, rows[i].idx == -1 ? 10 : (u32)sizeof(bad));
            if (rows[i].pay > (rows[i].idx == -1 ? 10 : sizeof(bad)))
                sys_write(fd, payload, rows[i].pay - (rows[i].idx == -1 ? 10 : sizeof(bad)));
            sys_close(fd);
            int r = sys_exec(path, 0);
            puts_("T27: "); puts_(rows[i].tag); puts_("=");
            putint(r); puts_("\n");
            if (r != -8) fails++;
            sys_unlink(path);
        }
        puts_("T27: bad_rejected="); putint(fails == 0); puts_("\n");
        /* a good image still loads after all that rejection */
        puts_("T27: alive\n");
        return fails != 0;
    }
    case 25: {  /* pipeline writer: everything on fd 1 goes into the pipe */
        puts_("T25: sent\n");
        const char *payload = "alpha\nbeta\n";
        sys_write(1, payload, (u32)strlen(payload));
        return 0;
    }
    case 26: {  /* pipeline reader: drain fd 0 until EOF, then report */
        char buf[128];
        int total = 0;
        for (;;) {
            int n = sys_read(0, buf + total, sizeof(buf) - (u32)total);
            if (n <= 0) break;
            total += n;
            if (total >= (int)sizeof(buf)) break;
        }
        puts_("T26: got="); putint(total); puts_("\n");
        sys_write(1, buf, (u32)total);
        return 0;
    }
    case 12: {  /* exit status */
        return 42;
    }
    default:
        puts_("T?: unknown test\n");
        return -1;
    }
}
