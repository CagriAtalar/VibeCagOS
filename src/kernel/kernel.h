#pragma once
#include "common.h"

/*
 * VibeCagOS — Kernel Definitions
 *
 * Contains: x86 hardware interface, GDT, IDT, TSS, process model,
 *           memory management constants, I/O port helpers.
 */

/* =========================================================================
 * Constants
 * ========================================================================= */

#define PROCS_MAX    16        /* Maximum number of concurrent processes */
#define KERNEL_STACK 32768     /* Per-process kernel stack (the Ring-0 shell and VFS
                                * use large locals; 8 KiB overflowed and corrupted
                                * the neighbouring process's stack) */
#define STACK_CANARY 0xC0DEF00Du

/* Process states */
#define PROC_UNUSED   0
#define PROC_RUNNABLE 1
#define PROC_RUNNING  2
#define PROC_SLEEPING 3
#define PROC_ZOMBIE   4
#define PROC_BLOCKED  5
#define PROC_CREATED  6        /* allocated, not yet runnable */

#define TIME_SLICE_TICKS 3     /* round-robin quantum (100 Hz => 30 ms) */

/* x86 Page table flags */
#define PAGE_PRESENT (1 << 0)
#define PAGE_WRITE   (1 << 1)
#define PAGE_USER    (1 << 2)

/*
 * Virtual address space layout (32-bit, 2-level paging, no PAE):
 *
 *   0x00000000 - __free_ram_end   kernel: identity mapped, supervisor-only,
 *                                 page tables SHARED by every address space
 *   0x10000000 (USER_BASE)        user image (text RO, data/bss RW)
 *   0x1FFFC000 - 0x20000000       user stack (4 pages), grows down
 *   up to USER_END (0xC0000000)   reserved for user (heap, mmap later)
 *   0xC0000000 - 0xFFFFFFFF       kernel-only (MMIO such as the NIC / LFB)
 *
 * User mappings live in page-directory slots >= 64, which are private per
 * process; the kernel slots are copied from the master directory.
 */
#define USER_BASE        0x10000000u
#define USER_STACK_TOP   0x20000000u   /* exclusive top of user stack */
#define USER_STACK_PAGES 4
#define USER_END         0xC0000000u   /* first non-user address */

/* =========================================================================
 * GDT Segment Selectors
 * ========================================================================= */

#define GDT_NULL        0x00   /* Null descriptor */
#define GDT_KERNEL_CODE 0x08   /* Ring 0 code */
#define GDT_KERNEL_DATA 0x10   /* Ring 0 data */
#define GDT_USER_CODE   0x18   /* Ring 3 code */
#define GDT_USER_DATA   0x20   /* Ring 3 data */
#define GDT_TSS         0x28   /* Task State Segment */

/* Selector RPL bits */
#define RPL_KERNEL 0
#define RPL_USER   3

/* Full selectors with RPL */
#define SEL_KERNEL_CODE  (GDT_KERNEL_CODE | RPL_KERNEL)
#define SEL_KERNEL_DATA  (GDT_KERNEL_DATA | RPL_KERNEL)
#define SEL_USER_CODE    (GDT_USER_CODE   | RPL_USER)
#define SEL_USER_DATA    (GDT_USER_DATA   | RPL_USER)
#define SEL_TSS          (GDT_TSS         | RPL_KERNEL)

/* =========================================================================
 * GDT / IDT Structures
 * ========================================================================= */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  type_attr;
    uint16_t offset_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* =========================================================================
 * Task State Segment (TSS)
 * ========================================================================= */

struct tss {
    uint32_t link;
    uint32_t esp0;   /* Kernel stack pointer for ring 0 */
    uint32_t ss0;    /* Kernel stack segment = GDT_KERNEL_DATA */
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

/* =========================================================================
 * Interrupt / Trap Frame
 * ========================================================================= */

struct trap_frame {
    /* Pushed by isr_common (lowest address first) */
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp;
    uint32_t esp_dummy;  /* Pushed by pusha - unusable */
    uint32_t ebx, edx, ecx, eax;
    /* Pushed by the per-vector stub */
    uint32_t int_no;
    uint32_t err_code;   /* real (CPU) or dummy 0 */
    /* Pushed by the CPU */
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
    /* ONLY present if the CPU changed privilege level (cs & 3 == 3).
     * For ring0->ring0 traps these two words are not on the stack. */
    uint32_t user_esp;
    uint32_t user_ss;
} __attribute__((packed));

/* True if the trap interrupted user mode. */
#define TF_FROM_USER(f) (((f)->cs & 3) == 3)

/* =========================================================================
 * Process Structure
 * ========================================================================= */

#define PROC_CWD_MAX 256

/* Per-process file descriptor table entry. */
#define OPEN_MAX 16
#define FD_NONE    0
#define FD_CONSOLE 1      /* kernel console (TTY): keyboard/serial in, VGA/serial out */
#define FD_VFS     2      /* open file in the VFS */
#define FD_PIPE    3      /* pipe end: kernel byte stream between processes */
struct file;
struct pipe;
struct fdent {
    uint8_t      type;    /* FD_* */
    uint8_t      can_read, can_write;
    struct file *file;    /* FD_VFS only */
    struct pipe *pipe;    /* FD_PIPE: shared pipe object, refcounted by its ends */
};

struct process {
    int      pid;
    int      ppid;                 /* 0 = owned by the kernel */
    int      state;                /* PROC_* */
    uint32_t sp;                   /* Saved kernel ESP while switched out */
    uint32_t *page_table;          /* Page directory (phys). NULL => kernel thread */
    uint32_t sleep_until;          /* Wake tick for PROC_SLEEPING */
    void    *wait_chan;            /* Channel for PROC_BLOCKED */
    int      time_slice;           /* Ticks left in the quantum */
    int      exit_code;
    uint32_t user_entry;           /* initial EIP (user procs) */
    uint32_t user_esp;             /* initial ESP (user procs) */
    void   (*kentry)(void);        /* entry point (kernel threads) */
    char     name[32];
    struct fdent fds[OPEN_MAX];    /* fd 0/1/2 = console */
    char     cwd[PROC_CWD_MAX];    /* per-process working directory (absolute) */
    /* Private kernel stack: TSS.esp0 points at its top while this process
     * runs. Holds the trap frame of the interrupted user context. */
    uint8_t  stack[KERNEL_STACK] __attribute__((aligned(16)));
};

/* =========================================================================
 * PANIC macro
 * ========================================================================= */

void kernel_panic(const char *file, int line, const char *fmt, ...);

#define PANIC(fmt, ...)                                          \
    do {                                                         \
        kernel_panic(__FILE__, __LINE__, fmt, ##__VA_ARGS__);   \
    } while (0)

/* =========================================================================
 * x86 Port I/O (inline)
 * ========================================================================= */

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

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ __volatile__("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ __volatile__("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline void cli(void) {
    __asm__ __volatile__("cli");
}

static inline void sti(void) {
    __asm__ __volatile__("sti");
}

static inline void halt(void) {
    __asm__ __volatile__("hlt");
}

static inline void load_idt(void *idt_ptr) {
    __asm__ __volatile__("lidt (%0)" : : "r"(idt_ptr));
}

static inline void load_gdt(void *gdt_ptr) {
    __asm__ __volatile__("lgdt (%0)" : : "r"(gdt_ptr));
}

static inline void load_tr(uint16_t sel) {
    __asm__ __volatile__("ltr %0" : : "r"(sel));
}

static inline void load_cr3(uint32_t pd) {
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(pd));
}

static inline uint32_t read_cr3(void) {
    uint32_t v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline uint32_t read_cr2(void) {
    uint32_t v;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline void enable_paging(void) {
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));
}

/* =========================================================================
 * Kernel API declarations
 * ========================================================================= */

/* Memory */
paddr_t   alloc_pages(uint32_t n);
void      free_pages(paddr_t paddr, uint32_t n);
void      map_page(uint32_t *page_dir, uint32_t vaddr, paddr_t paddr, uint32_t flags);

static inline uint32_t irq_save(void) {
    uint32_t f;
    __asm__ __volatile__("pushf; popl %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_restore(uint32_t f) {
    __asm__ __volatile__("pushl %0; popf" : : "r"(f) : "memory", "cc");
}

/* GDT / IDT */
void      gdt_init(void);
void      idt_init(void);
void      pic_init(void);
void      pic_unmask_irq(uint8_t irq);
void      idt_set_gate(uint8_t num, uint32_t handler, uint16_t sel, uint8_t flags);

/* Serial */
void      serial_init(void);
long      serial_getchar(void);

/* Scheduler */
void      yield(void);
void      schedule(void);
void      sleep_ms(uint32_t ms);

/* Process */
struct process *process_create_user(const char *name, const void *image,
                                    size_t image_size, int argc, const char *const *argv);
int       process_waitpid(int pid, int *status);   /* current process waits for a child */
struct user_prog { const char *name; const uint8_t *start, *end; };
const struct user_prog *user_prog_find(const char *name);
extern const struct user_prog user_progs[];
extern const int user_prog_count;
struct process *process_create_kthread(const char *name, void (*entry)(void));
void      process_init(void);
void      fd_init_std(struct process *p);      /* fds 0,1,2 -> console */
void      fd_close_all(struct process *p);
void      process_start(void) __attribute__((noreturn));
void      process_exit(int code) __attribute__((noreturn));
int       process_wait(struct process *p);      /* kernel-side wait + reap */
int       process_kill(int pid);
void      block_on(void *chan);                 /* IF must be 0 */
void      wakeup(void *chan);
void      process_tick(struct trap_frame *f);   /* timer hook */
extern struct process procs[];
extern struct process *current_proc;
extern struct process *idle_proc;
extern void switch_context(uint32_t *old_sp, uint32_t new_sp);
extern void trapret(void);
extern const uint32_t isr_stub_table[48];

/* Interrupt handlers (asm stubs) */
extern void isr0(void);   /* Divide by zero */
extern void isr1(void);   /* Debug */
extern void isr2(void);   /* NMI */
extern void isr3(void);   /* Breakpoint */
extern void isr6(void);   /* Invalid opcode */
extern void isr8(void);   /* Double fault */
extern void isr13(void);  /* GP fault */
extern void isr14(void);  /* Page fault */
extern void isr32(void);  /* Timer IRQ0 */
extern void isr33(void);  /* Keyboard IRQ1 */
extern void isr34(void);  /* Cascade IRQ2 */
extern void isr43(void);  /* NIC IRQ11 */
extern void isr44(void);  /* Mouse IRQ12 */
extern void isr46(void);  /* IDE IRQ14 */
extern void isr128(void); /* Syscall int 0x80 */

/* Handle interrupt (C) */
void      handle_interrupt(struct trap_frame *f);

/* Timer */
void      pit_init(uint32_t hz);
uint32_t  get_ticks(void);
uint32_t  get_uptime_ms(void);

/* Keyboard */
void      keyboard_init(void);
int       keyboard_getchar(void);  /* Returns -1 if no char */

/* TSS */
void      tss_set_kernel_stack(uint32_t esp0);
