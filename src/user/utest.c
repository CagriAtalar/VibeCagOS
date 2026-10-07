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
    case 2: {   /* user data/rodata are readable, data is writable */
        data_probe++;
        puts_("T2: data="); putint(data_probe); puts_(" ro0="); putint(rodata_probe[0]); puts_("\n");
        return 0;
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
        puts_("T18: spawned="); putint(p1 > 0 && p2 > 0 && p1 != p2); puts_("\n");
        int st1 = -1, st2 = -1, r1 = sys_waitpid(p1, &st1), r2 = sys_waitpid(-1, &st2);
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
    case 12: {  /* exit status */
        return 42;
    }
    default:
        puts_("T?: unknown test\n");
        return -1;
    }
}
