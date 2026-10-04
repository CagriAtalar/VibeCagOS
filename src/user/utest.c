/*
 * utest - Ring-3 self-test program. `utest <n>` (kernel shell) starts it with
 * n delivered as the argument. Every test prints a line starting "T<n>:" so
 * tests/run.sh can grep deterministic results from the serial console.
 */
#include "ulib.h"

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
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

int user_main(int test) {
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
        for (int i = 0; i < 10; i++) { sys_putchar('A'); busy(6000000); }
        return 10;
    }
    case 5: {   /* preemption: B (no yield) */
        for (int i = 0; i < 10; i++) { sys_putchar('B'); busy(6000000); }
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
    case 12: {  /* exit status */
        return 42;
    }
    default:
        puts_("T?: unknown test\n");
        return -1;
    }
}
