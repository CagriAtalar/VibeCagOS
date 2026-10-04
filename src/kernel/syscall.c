/*
 * VibeCagOS - system call dispatcher (int 0x80).
 *
 * Flow:  Ring3 `int $0x80` -> CPU loads SS0:ESP0 from the TSS -> isr128 ->
 * isr_common (saves frame) -> handle_interrupt() -> syscall_dispatch(frame)
 * -> result stored into frame->eax -> trapret -> iret -> Ring3.
 *
 * The syscall may switch processes (sleep/read/yield): the frame lives on
 * this process's own kernel stack, so it is still intact when we resume.
 *
 * ABI and per-call contracts: src/abi/syscall.h
 */
#include "kernel.h"
#include "usercopy.h"
#include "klog.h"
#include "../abi/syscall.h"

extern int console_read_blocking(char *buf, size_t max);

#define IO_CHUNK 128

static int sys_write(uint32_t fd, const void *ubuf, uint32_t len) {
    if (fd != 1 && fd != 2) return -E_BADF;
    char tmp[IO_CHUNK];
    uint32_t done = 0;
    while (done < len) {
        uint32_t n = len - done;
        if (n > IO_CHUNK) n = IO_CHUNK;
        int r = copy_from_user(tmp, (const char *)ubuf + done, n);
        if (r < 0) return done ? (int)done : r;
        for (uint32_t i = 0; i < n; i++) putchar(tmp[i]);
        done += n;
    }
    return (int)done;
}

static int sys_read(uint32_t fd, void *ubuf, uint32_t len) {
    if (fd != 0) return -E_BADF;
    if (len == 0) return 0;
    if (len > IO_CHUNK) len = IO_CHUNK;
    /* Fail early so a bad pointer doesn't swallow keystrokes. */
    if (!user_range_valid(ubuf, len, true)) return -E_FAULT;
    char tmp[IO_CHUNK];
    int n = console_read_blocking(tmp, len);
    int r = copy_to_user(ubuf, tmp, (size_t)n);
    return r < 0 ? r : n;
}

static int sys_getinfo(void *ubuf) {
    struct vibe_info info;
    info.pid       = (unsigned)current_proc->pid;
    info.ppid      = (unsigned)(current_proc->ppid > 0 ? current_proc->ppid : 0);
    info.ticks     = get_ticks();
    info.uptime_ms = get_uptime_ms();
    return copy_to_user(ubuf, &info, sizeof(info));
}

void syscall_dispatch(struct trap_frame *f) {
    uint32_t a1 = f->ebx, a2 = f->ecx, a3 = f->edx;
    int ret;

    switch (f->eax) {
    case SYS_EXIT:
        process_exit((int)a1);                      /* no return */
    case SYS_WRITE:   ret = sys_write(a1, (const void *)a2, a3); break;
    case SYS_READ:    ret = sys_read(a1, (void *)a2, a3);        break;
    case SYS_GETPID:  ret = current_proc->pid;                   break;
    case SYS_SLEEP:   sleep_ms(a1); ret = 0;                     break;
    case SYS_YIELD:   yield();      ret = 0;                     break;
    case SYS_UPTIME:  ret = (int)get_uptime_ms();                break;
    case SYS_PUTCHAR: putchar((char)(a1 & 0xFF)); ret = 0;       break;
    case SYS_GETINFO: ret = sys_getinfo((void *)a1);             break;
    default:
        KWARN("SYSCALL", "pid %d: invalid syscall %u", current_proc->pid, f->eax);
        ret = -E_NOSYS;
        break;
    }
    f->eax = (uint32_t)ret;
}
