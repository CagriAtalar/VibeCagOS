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

#include "sysfile.h"
#include "../drivers/vga.h"

/* SYS_SPAWN / SYS_EXEC (exec.c). Declared here rather than in sysfile.h
 * because they own program loading, not file descriptors. */
int sys_spawn(const char *uname, const char *const *uargv);
int sys_exec(const char *upath, const char *const *uargv);

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
    case SYS_OPEN:    ret = sys_open((const char *)a1, a2, a3);  break;
    case SYS_CLOSE:   ret = sys_close(a1);                       break;
    case SYS_STAT:    ret = sys_stat((const char *)a1, (void *)a2); break;
    case SYS_FSTAT:   ret = sys_fstat(a1, (void *)a2);           break;
    case SYS_MKDIR:   ret = sys_mkdir((const char *)a1, a2);     break;
    case SYS_UNLINK:  ret = sys_unlink((const char *)a1);        break;
    case SYS_RMDIR:   ret = sys_rmdir((const char *)a1);         break;
    case SYS_LSEEK:   ret = sys_lseek(a1, (int)a2, a3);          break;
    case SYS_READDIR: ret = sys_readdir(a1, (void *)a2, a3);     break;
    case SYS_SPAWN:   ret = sys_spawn((const char *)a1, (const char *const *)a2); break;
    case SYS_EXEC:    ret = sys_exec((const char *)a1, (const char *const *)a2); break;
    case SYS_WAITPID: ret = sys_waitpid((int)a1, (int *)a2);     break;
    case SYS_CHDIR:   ret = sys_chdir((const char *)a1);         break;
    case SYS_GETCWD:  ret = sys_getcwd((char *)a1, a2);          break;
    case SYS_RENAME:  ret = sys_rename((const char *)a1, (const char *)a2); break;
    case SYS_KILL:    ret = sys_kill((int)a1);                   break;
    case SYS_CLEAR:   vga_clear(); ret = 0;                      break;
    case SYS_PIPE:    ret = sys_pipe((int *)a1);                 break;
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
