#pragma once
#include "common.h"

#define PROCS_MAX 8
#define PROC_UNUSED   0
#define PROC_RUNNABLE 1
#define PROC_EXITED   2

// x86 paging flags
#define PAGE_PRESENT  (1 << 0)
#define PAGE_WRITE    (1 << 1)
#define PAGE_USER     (1 << 2)

#define USER_BASE 0x1000000

struct process {
    int pid;
    int state;
    vaddr_t sp;
    uint32_t *page_table;
    uint8_t stack[8192];
};

struct trap_frame {
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t int_no;
    uint32_t err_code;
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
    uint32_t user_esp;
    uint32_t user_ss;
} __attribute__((packed));


#define PANIC(fmt, ...)                                                        \
    do {                                                                       \
        printf("PANIC: %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);  \
        while (1) { __asm__ __volatile__("hlt"); }                             \
    } while (0)

// x86 specific functions
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline void cli(void) {
    __asm__ __volatile__("cli");
}

static inline void sti(void) {
    __asm__ __volatile__("sti");
}

static inline void load_idt(void *idt_ptr) {
    __asm__ __volatile__("lidt (%0)" : : "r"(idt_ptr));
}

static inline void load_cr3(uint32_t pd) {
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(pd));
}

static inline uint32_t read_cr3(void) {
    uint32_t value;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(value));
    return value;
}

static inline void enable_paging(void) {
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));
}

// Syscall numbers
#define SYS_PUTCHAR    1
#define SYS_GETCHAR    2
#define SYS_LIST_FILES 3
#define SYS_READ_FILE  4
#define SYS_CREATE_FILE 5
#define SYS_WRITE_FILE 6
#define SYS_DELETE_FILE 7
#define SYS_FORMAT_FS  8
#define SYS_EXIT       9

// Function declarations
void enter_user_mode(void);
uint32_t parse_ip_address(const char *ip_str);
long syscall(int num, uint32_t arg1, uint32_t arg2, uint32_t arg3);

