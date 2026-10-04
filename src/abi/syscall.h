/*
 * VibeCagOS - syscall ABI (shared by kernel and user programs).
 * Pure #defines: must not include any kernel header.
 *
 *   int 0x80      EAX = number, EBX/ECX/EDX/ESI/EDI = arg1..arg5
 *   return        EAX (negative = -errno)
 *
 * Per-syscall contract (blocks? / touches user memory?):
 *   SYS_EXIT    (status)            never returns          no memory
 *   SYS_WRITE   (fd, buf, len)      may not block          reads user buf
 *   SYS_READ    (fd, buf, len)      BLOCKS until >=1 byte  writes user buf
 *   SYS_GETPID  ()                  -                      -
 *   SYS_SLEEP   (ms)                BLOCKS                 -
 *   SYS_YIELD   ()                  reschedules            -
 *   SYS_UPTIME  ()                  returns ms             -
 *   SYS_PUTCHAR (c)                 -                      -
 *   SYS_GETINFO (struct vibe_info*) -                      writes user buf
 */
#ifndef VIBE_ABI_SYSCALL_H
#define VIBE_ABI_SYSCALL_H

#define SYS_EXIT     1
#define SYS_WRITE    2
#define SYS_READ     3
#define SYS_GETPID   4
#define SYS_SLEEP    5
#define SYS_YIELD    6
#define SYS_UPTIME   7
#define SYS_PUTCHAR  8
#define SYS_GETINFO  9
#define SYS_MAX      9

#define E_PERM    1
#define E_BADF    9
#define E_FAULT  14
#define E_INVAL  22
#define E_NOSYS  38

struct vibe_info {
    unsigned int pid;
    unsigned int ppid;
    unsigned int ticks;
    unsigned int uptime_ms;
};

/* Flat user image header ("VBIN"), at offset 0 of every user binary.
 * All offsets are relative to USER_BASE. */
#define VBIN_MAGIC 0x4E494256u   /* 'V' 'B' 'I' 'N' little endian */
struct vbin_header {
    unsigned int magic;
    unsigned int entry;      /* absolute virtual address */
    unsigned int text_size;  /* bytes, page aligned: mapped read-only */
    unsigned int file_size;  /* bytes present in the file */
    unsigned int mem_size;   /* bytes in memory (>= file_size, rest = bss) */
};

#endif
