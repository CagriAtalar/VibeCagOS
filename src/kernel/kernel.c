/*
 * VibeCagOS — Main Kernel
 *
 * Subsystems initialized here:
 *   - Serial (COM1) — output + input
 *   - VGA text mode — colored output
 *   - GDT — proper segment descriptors with TSS
 *   - IDT — all exception/IRQ/syscall handlers
 *   - PIC — 8259, remapped, IRQ0+IRQ1 unmasked
 *   - PIT — preemptive timer at ~100 Hz
 *   - PS/2 Keyboard — scancode decoder with ring buffer
 *   - Kernel Heap (kmalloc/kfree) — first-fit allocator
 *   - Paging — identity map with kernel/user split
 *   - Physical allocator — bump allocator
 *   - Process scheduler — preemptive round-robin
 *   - IDE disk — ATA PIO
 *   - PCI — bus scan
 *   - RTL8139 NIC — polling
 *   - VFS — virtual filesystem layer
 *   - VibeFS — hierarchical disk filesystem
 *   - procfs — /proc pseudo-filesystem
 *   - Shell — interactive colored terminal with directory navigation
 */

#include "kernel.h"
#include "common.h"
#include "kmalloc.h"
#include "klog.h"
#include "pmm.h"
#include "vmm.h"
#include "multiboot.h"
#include "../fs/simplefs.h"
#include "../fs/vfs.h"
#include "../fs/vibefs.h"
#include "../fs/procfs.h"
#include "../fs/devfs.h"
#include "../drivers/vga.h"
#include "../drivers/ide.h"
#include "../drivers/pci.h"
#include "../drivers/rtl8139.h"
#include "../drivers/rtc.h"
#include "../drivers/mouse.h"
#include "../drivers/gui.h"
#include "../net/ethernet.h"
#include "../net/arp.h"
#include "../net/ipv4.h"
#include "../net/icmp.h"
#include "../net/udp.h"
#include "../net/dns.h"
#include "../net/netconfig.h"

/* Multiboot globals */
struct multiboot_info *g_mb_info = NULL;
uint32_t               g_mb_magic = 0;

/* Linker-provided symbols */
extern char __kernel_base[];
extern char __stack_top[];
extern char __bss[], __bss_end[];
extern char __free_ram[], __free_ram_end[];

/* =========================================================================
 * VibeCagOS Version
 * ========================================================================= */

#define VIBECAGOS_VERSION "0.4.0"
#define VIBECAGOS_BUILD   "2026-10-03"

/* =========================================================================
 * Serial Port (COM1)
 * ========================================================================= */

#define PORT_COM1 0x3F8

void serial_init(void) {
    outb(PORT_COM1 + 1, 0x00);  /* Disable interrupts */
    outb(PORT_COM1 + 3, 0x80);  /* Enable DLAB (set baud divisor) */
    outb(PORT_COM1 + 0, 0x03);  /* Divisor low: 38400 baud */
    outb(PORT_COM1 + 1, 0x00);  /* Divisor high */
    outb(PORT_COM1 + 3, 0x03);  /* 8 bits, no parity, 1 stop bit */
    outb(PORT_COM1 + 2, 0xC7);  /* FIFO on, clear, 14-byte threshold */
    outb(PORT_COM1 + 4, 0x0B);  /* RTS/DSR set */
}

long serial_getchar(void) {
    if ((inb(PORT_COM1 + 5) & 1) == 0)
        return -1;
    return inb(PORT_COM1);
}

static void serial_putchar(char ch) {
    while ((inb(PORT_COM1 + 5) & 0x20) == 0)
        ;
    outb(PORT_COM1, ch);
}

/* =========================================================================
 * putchar — sends to both serial and VGA
 * ========================================================================= */

void putchar(char ch) {
    if (ch == '\n')
        serial_putchar('\r');
    serial_putchar(ch);
    vga_putchar(ch);
}

/* =========================================================================
 * GDT Setup
 * ========================================================================= */

static struct gdt_entry gdt[7];  /* null, kcode, kdata, ucode, udata, tss, spare */
static struct gdt_ptr   gdtp;
static struct tss       kernel_tss;

static void gdt_set_entry(int idx, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t gran) {
    gdt[idx].base_low   = base & 0xFFFF;
    gdt[idx].base_mid   = (base >> 16) & 0xFF;
    gdt[idx].base_high  = (base >> 24) & 0xFF;
    gdt[idx].limit_low  = limit & 0xFFFF;
    gdt[idx].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[idx].access     = access;
}

/*
 * Set a TSS descriptor (different format from normal segments).
 */
static void gdt_set_tss(int idx, uint32_t base, uint32_t limit) {
    gdt[idx].limit_low   = limit & 0xFFFF;
    gdt[idx].base_low    = base & 0xFFFF;
    gdt[idx].base_mid    = (base >> 16) & 0xFF;
    gdt[idx].access      = 0x89;  /* Present, DPL=0, 32-bit TSS Available */
    gdt[idx].granularity = ((limit >> 16) & 0x0F);
    gdt[idx].base_high   = (base >> 24) & 0xFF;
}

void gdt_init(void) {
    /* Entry 0: Null descriptor */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* Entry 1 (0x08): Kernel code — ring 0, 32-bit, 4KB granularity, 4GB limit */
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    /* Entry 2 (0x10): Kernel data — ring 0, 32-bit, 4KB granularity, 4GB limit */
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /* Entry 3 (0x18): User code — ring 3, 32-bit, 4KB granularity, 4GB limit */
    gdt_set_entry(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    /* Entry 4 (0x20): User data — ring 3, 32-bit, 4KB granularity, 4GB limit */
    gdt_set_entry(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    /* Entry 5 (0x28): TSS */
    memset(&kernel_tss, 0, sizeof(kernel_tss));
    kernel_tss.ss0 = SEL_KERNEL_DATA;
    kernel_tss.iomap_base = sizeof(kernel_tss);  /* No I/O map */
    gdt_set_tss(5, (uint32_t)&kernel_tss, sizeof(kernel_tss) - 1);

    /* Load GDT */
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint32_t)&gdt;
    load_gdt(&gdtp);

    /* Reload segment registers */
    __asm__ __volatile__(
        "mov $0x10, %%ax\n"   /* GDT_KERNEL_DATA */
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        "ljmp $0x08, $1f\n"   /* Far jump to reload CS = GDT_KERNEL_CODE */
        "1:\n"
        : : : "eax"
    );

    /* Load TSS */
    load_tr(SEL_TSS);
}

void tss_set_kernel_stack(uint32_t esp0) {
    kernel_tss.esp0 = esp0;
}

/* =========================================================================
 * IDT Setup
 * ========================================================================= */

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

void idt_set_gate(uint8_t num, uint32_t handler, uint16_t sel, uint8_t flags) {
    idt[num].offset_low  = handler & 0xFFFF;
    idt[num].selector    = sel;
    idt[num].zero        = 0;
    idt[num].type_attr   = flags;
    idt[num].offset_high = (handler >> 16) & 0xFFFF;
}

/* Assembly ISR stubs */
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr6(void);
extern void isr8(void);
extern void isr13(void);
extern void isr14(void);
extern void isr32(void);
extern void isr33(void);
extern void isr34(void);
extern void isr46(void);
extern void isr128(void);

void idt_init(void) {
    memset(&idt, 0, sizeof(idt));

    /* CPU exceptions — ring 0, interrupt gate (IF=0 on entry) */
    idt_set_gate(0,  (uint32_t)isr0,  SEL_KERNEL_CODE, 0x8E);
    idt_set_gate(1,  (uint32_t)isr1,  SEL_KERNEL_CODE, 0x8E);
    idt_set_gate(2,  (uint32_t)isr2,  SEL_KERNEL_CODE, 0x8E);
    idt_set_gate(3,  (uint32_t)isr3,  SEL_KERNEL_CODE, 0x8E);
    idt_set_gate(6,  (uint32_t)isr6,  SEL_KERNEL_CODE, 0x8E);
    idt_set_gate(8,  (uint32_t)isr8,  SEL_KERNEL_CODE, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, SEL_KERNEL_CODE, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, SEL_KERNEL_CODE, 0x8E);

    /* Hardware IRQs — remapped by PIC to 0x20–0x2F */
    idt_set_gate(32, (uint32_t)isr32, SEL_KERNEL_CODE, 0x8E);  /* Timer */
    idt_set_gate(33, (uint32_t)isr33, SEL_KERNEL_CODE, 0x8E);  /* Keyboard */
    idt_set_gate(34, (uint32_t)isr34, SEL_KERNEL_CODE, 0x8E);  /* Cascade */
    idt_set_gate(43, (uint32_t)isr43, SEL_KERNEL_CODE, 0x8E);  /* NIC (RTL8139) */
    idt_set_gate(44, (uint32_t)isr44, SEL_KERNEL_CODE, 0x8E);  /* Mouse */
    idt_set_gate(46, (uint32_t)isr46, SEL_KERNEL_CODE, 0x8E);  /* IDE */

    /* Syscall — DPL=3 so userspace can call it */
    idt_set_gate(128, (uint32_t)isr128, SEL_KERNEL_CODE, 0xEE);

    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;
    load_idt(&idtp);
}

/* =========================================================================
 * PIC (8259) Setup
 * ========================================================================= */

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

void pic_init(void) {
    /* Start initialisation sequence (ICW1) */
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);
    io_wait();

    /* ICW2: Interrupt vector base */
    outb(PIC1_DATA, 0x20);  /* IRQ0–7 → int 0x20–0x27 */
    outb(PIC2_DATA, 0x28);  /* IRQ8–15 → int 0x28–0x2F */
    io_wait();

    /* ICW3: Cascade */
    outb(PIC1_DATA, 0x04);  /* Master: slave at IRQ2 */
    outb(PIC2_DATA, 0x02);  /* Slave: cascade identity */
    io_wait();

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);
    io_wait();

    /*
     * Mask all IRQs except IRQ0 (timer), IRQ1 (keyboard), IRQ2 (cascade)
     * and IRQ12 (mouse).
     */
    outb(PIC1_DATA, 0xF8);  /* Enable IRQ0, IRQ1, IRQ2 (cascade) */
    outb(PIC2_DATA, 0xEF);  /* Enable IRQ12 (mouse) */
}

void pic_unmask_irq(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << 2));
    }
    outb(port, inb(port) & ~(1 << irq));
}

static void pic_eoi(uint8_t irq) {
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

/* =========================================================================
 * PIT Timer (8253/8254)
 * ========================================================================= */

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define PIT_CLOCK 1193182u

static volatile uint32_t ticks = 0;

void pit_init(uint32_t hz) {
    uint32_t divisor = PIT_CLOCK / hz;
    outb(PIT_CMD, 0x36);  /* Channel 0, lo/hi, rate generator, binary */
    outb(PIT_CH0, divisor & 0xFF);
    outb(PIT_CH0, (divisor >> 8) & 0xFF);
}

uint32_t get_ticks(void) {
    return ticks;
}

uint32_t get_uptime_ms(void) {
    return ticks * 10;  /* 100 Hz → 10 ms per tick */
}

/* =========================================================================
 * PS/2 Keyboard Driver
 * ========================================================================= */

#define KB_DATA_PORT   0x60
#define KB_STATUS_PORT 0x64
#define KB_BUF_SIZE    256

/* US keyboard scancode set 1 → ASCII (unshifted) */
static const char scancode_map[128] = {
    0,    27,  '1',  '2',  '3',  '4',  '5',  '6',
    '7',  '8', '9',  '0',  '-',  '=',  '\b', '\t',
    'q',  'w', 'e',  'r',  't',  'y',  'u',  'i',
    'o',  'p', '[',  ']',  '\n', 0,    'a',  's',
    'd',  'f', 'g',  'h',  'j',  'k',  'l',  ';',
    '\'', '`', 0,    '\\', 'z',  'x',  'c',  'v',
    'b',  'n', 'm',  ',',  '.',  '/',  0,    '*',
    0,    ' ', 0,    0,    0,    0,    0,    0,
    /* F1-F8, ... */
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    /* Keypad */
    0, 0, 0, '-', 0, 0, 0, '+',
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0
};

/* Shifted versions */
static const char scancode_shift_map[128] = {
    0,    27,  '!',  '@',  '#',  '$',  '%',  '^',
    '&',  '*', '(',  ')',  '_',  '+',  '\b', '\t',
    'Q',  'W', 'E',  'R',  'T',  'Y',  'U',  'I',
    'O',  'P', '{',  '}',  '\n', 0,    'A',  'S',
    'D',  'F', 'G',  'H',  'J',  'K',  'L',  ':',
    '"',  '~', 0,    '|',  'Z',  'X',  'C',  'V',
    'B',  'N', 'M',  '<',  '>',  '?',  0,    '*',
    0,    ' ', 0,    0,    0,    0,    0,    0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, '-', 0, 0, 0, '+',
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0
};

/* Special scancodes for arrow keys (extended, preceded by 0xE0) */
#define SCANCODE_UP    0x48
#define SCANCODE_DOWN  0x50
#define SCANCODE_LEFT  0x4B
#define SCANCODE_RIGHT 0x4D

/* Special return values for arrow keys */
#define KEY_UP    0x100
#define KEY_DOWN  0x101
#define KEY_LEFT  0x102
#define KEY_RIGHT 0x103

static volatile char  kb_buf[KB_BUF_SIZE];
static volatile int   kb_read  = 0;
static volatile int   kb_write = 0;
static          bool  shift_pressed  = false;
static          bool  ext_sequence   = false;

static void kb_buf_push(char c) {
    int next = (kb_write + 1) % KB_BUF_SIZE;
    if (next != kb_read) {
        kb_buf[kb_write] = c;
        kb_write = next;
    }
}

void keyboard_init(void) {
    /* Keyboard is enabled via PIC (IRQ1 unmasked in pic_init) */
}

/* Called from IRQ1 handler */
static void keyboard_handle_irq(void) {
    uint8_t sc = inb(KB_DATA_PORT);

    /* Extended key sequence */
    if (sc == 0xE0) {
        ext_sequence = true;
        return;
    }

    bool released = (sc & 0x80) != 0;
    uint8_t key   = sc & 0x7F;

    if (ext_sequence) {
        ext_sequence = false;
        if (!released) {
            /* Arrow keys etc. — push special codes as two chars */
            switch (key) {
                case SCANCODE_UP:
                    kb_buf_push('\x1B'); kb_buf_push('['); kb_buf_push('A');
                    break;
                case SCANCODE_DOWN:
                    kb_buf_push('\x1B'); kb_buf_push('['); kb_buf_push('B');
                    break;
                case SCANCODE_RIGHT:
                    kb_buf_push('\x1B'); kb_buf_push('['); kb_buf_push('C');
                    break;
                case SCANCODE_LEFT:
                    kb_buf_push('\x1B'); kb_buf_push('['); kb_buf_push('D');
                    break;
            }
        }
        return;
    }

    /* Track shift state (scancodes 0x2A = left shift, 0x36 = right shift) */
    if (key == 0x2A || key == 0x36) {
        shift_pressed = !released;
        return;
    }

    if (!released && key < 128) {
        char c = shift_pressed ? scancode_shift_map[key] : scancode_map[key];
        if (c != 0)
            kb_buf_push(c);
    }
}

int keyboard_getchar(void) {
    if (kb_read == kb_write)
        return -1;
    char c = kb_buf[kb_read];
    kb_read = (kb_read + 1) % KB_BUF_SIZE;
    return (unsigned char)c;
}

/* =========================================================================
 * Physical & Virtual Memory Subsystems
 * Core implementations in pmm.c and vmm.c
 * ========================================================================= */

void map_page(uint32_t *page_dir, uint32_t vaddr, paddr_t paddr, uint32_t flags) {
    vmm_map_page(page_dir, vaddr, paddr, flags);
}

void paging_init(void) {
    vmm_init();
}

/* =========================================================================
 * Process Scheduler
 * ========================================================================= */

struct process procs[PROCS_MAX];
struct process *current_proc = NULL;
struct process *idle_proc    = NULL;

static int next_pid = 1;

/* Forward declaration */
__attribute__((naked)) void switch_context(uint32_t *prev_sp, uint32_t *next_sp);

__attribute__((naked)) void switch_context(uint32_t *prev_sp, uint32_t *next_sp) {
    __asm__ __volatile__(
        "pushl %ebp\n"
        "pushl %ebx\n"
        "pushl %esi\n"
        "pushl %edi\n"
        "movl  %esp, (%eax)\n"  /* eax = prev_sp */
        "movl  (%edx), %esp\n"  /* edx = next_sp */
        "popl  %edi\n"
        "popl  %esi\n"
        "popl  %ebx\n"
        "popl  %ebp\n"
        "ret\n"
    );
}

void yield(void) {
    struct process *next = idle_proc;
    int start = current_proc ? current_proc->pid : 0;

    for (int i = 0; i < PROCS_MAX; i++) {
        int idx = (start + i) % PROCS_MAX;
        struct process *p = &procs[idx];
        if (p->state == PROC_RUNNABLE && p != current_proc) {
            next = p;
            break;
        }
    }

    if (next == current_proc)
        return;

    struct process *prev = current_proc;
    current_proc = next;

    if (next->page_table)
        load_cr3((uint32_t)next->page_table);

    tss_set_kernel_stack((uint32_t)&next->stack[KERNEL_STACK]);
    switch_context(&prev->sp, &next->sp);
}

void schedule(void) {
    /* Check for sleeping processes that should wake up */
    uint32_t now = ticks;
    for (int i = 0; i < PROCS_MAX; i++) {
        if (procs[i].state == PROC_SLEEPING && procs[i].sleep_until <= now)
            procs[i].state = PROC_RUNNABLE;
    }
    yield();
}

void sleep_ms(uint32_t ms) {
    if (!current_proc || current_proc == idle_proc)
        return;
    uint32_t wake_tick = ticks + (ms / 10);  /* 100 Hz = 10 ms per tick */
    current_proc->state       = PROC_SLEEPING;
    current_proc->sleep_until = wake_tick;
    yield();
}

/* =========================================================================
 * Idle Process
 * ========================================================================= */

static void idle_task(void) {
    while (1) {
        __asm__ __volatile__("sti\nhlt\ncli");
        yield();
    }
}

/* =========================================================================
 * User Entry Point (for ring 3 processes)
 * ========================================================================= */

__attribute__((naked)) void user_entry(void) {
    __asm__ __volatile__(
        "mov $0x23, %%ax\n"      /* User data segment (GDT_USER_DATA | RPL_USER = 0x20|3 = 0x23) */
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%esp, %%eax\n"
        "pushl $0x23\n"          /* SS */
        "pushl %%eax\n"          /* ESP */
        "pushf\n"
        "popl %%eax\n"
        "orl $0x200, %%eax\n"    /* Enable IF in EFLAGS */
        "pushl %%eax\n"
        "pushl $0x1B\n"          /* CS (GDT_USER_CODE | RPL_USER = 0x18|3 = 0x1B) */
        "pushl %0\n"             /* EIP */
        "iret\n"
        : : "r"(USER_BASE)
    );
}

/* =========================================================================
 * Create Process
 * ========================================================================= */

struct process *create_process(const void *image, size_t image_size, const char *name) {
    struct process *proc = NULL;

    for (int i = 0; i < PROCS_MAX; i++) {
        if (procs[i].state == PROC_UNUSED) {
            proc = &procs[i];
            break;
        }
    }

    if (!proc)
        PANIC("No free process slots");

    /* Set up kernel stack — switch_context will pop these */
    uint32_t *sp = (uint32_t *)&proc->stack[KERNEL_STACK];
    *--sp = 0;             /* edi */
    *--sp = 0;             /* esi */
    *--sp = 0;             /* ebx */
    *--sp = 0;             /* ebp */
    *--sp = (uint32_t)user_entry;  /* return address */

    /* Create private page directory */
    uint32_t *pd = (uint32_t *)alloc_pages(1);

    /* Identity-map kernel */
    for (paddr_t p = 0; p < (paddr_t)__free_ram_end; p += PAGE_SIZE)
        map_page(pd, p, p, PAGE_WRITE);

    /* Map user image */
    for (uint32_t off = 0; off < image_size; off += PAGE_SIZE) {
        paddr_t page  = alloc_pages(1);
        size_t  chunk = PAGE_SIZE <= (image_size - off) ? PAGE_SIZE : (image_size - off);
        memcpy((void *)page, (const uint8_t *)image + off, chunk);
        map_page(pd, USER_BASE + off, page, PAGE_USER | PAGE_WRITE);
    }

    proc->pid         = next_pid++;
    proc->state       = PROC_RUNNABLE;
    proc->sp          = (uint32_t)sp;
    proc->page_table  = pd;
    proc->exit_code   = 0;
    proc->sleep_until = 0;
    strncpy(proc->name, name, sizeof(proc->name) - 1);

    return proc;
}

void process_exit(int code) {
    if (current_proc) {
        current_proc->exit_code = code;
        current_proc->state     = PROC_ZOMBIE;
    }
    yield();
    PANIC("unreachable after exit");
}

/* =========================================================================
 * Kernel Panic
 * ========================================================================= */

void kernel_panic(const char *file, int line, const char *fmt, ...) {
    cli();

    /* Use VGA color for panic — bright red on black */
    vga_set_color(0x0C);  /* Bright red */

    printf("\n\n");
    printf("==============================================\n");
    printf("         VibeCagOS KERNEL PANIC              \n");
    printf("==============================================\n");
    printf("File    : %s\n", file);
    printf("Line    : %d\n", line);
    printf("Uptime  : %u ms\n", get_uptime_ms());
    printf("Message : ");

    /* Print format string with arguments properly */
    va_list args;
    va_start(args, fmt);
    /* Walk the format string manually for %s, %d, %u, %x */
    const char *p = fmt;
    while (*p) {
        if (*p != '%') {
            putchar(*p++);
            continue;
        }
        p++;  /* skip '%' */
        switch (*p) {
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s) putchar(*s++);
                break;
            }
            case 'd': {
                int v = va_arg(args, int);
                printf("%d", v);
                break;
            }
            case 'u': {
                unsigned v = va_arg(args, unsigned);
                printf("%u", v);
                break;
            }
            case 'x': {
                unsigned v = va_arg(args, unsigned);
                printf("%x", v);
                break;
            }
            case 'p': {
                unsigned v = va_arg(args, unsigned);
                printf("%p", v);
                break;
            }
            default:
                putchar('%');
                putchar(*p);
                break;
        }
        p++;
    }
    va_end(args);

    printf("\n");

    if (current_proc) {
        printf("Process : %s (pid %d)\n", current_proc->name, current_proc->pid);
    }

    printf("==============================================\n");
    printf("System halted. Connect GDB for debugging.\n");
    printf("  qemu ... -s -S\n");
    printf("  gdb kernel.elf -ex 'target remote :1234'\n");

    while (1)
        halt();
}

/* =========================================================================
 * Interrupt Handler (C dispatch)
 * ========================================================================= */

static void handle_exception(struct trap_frame *f) {
    const char *names[] = {
        "Divide by Zero",     /* 0 */
        "Debug",              /* 1 */
        "NMI",                /* 2 */
        "Breakpoint",         /* 3 */
        "Overflow",           /* 4 */
        "Bound Range",        /* 5 */
        "Invalid Opcode",     /* 6 */
        "Device Not Avail",   /* 7 */
        "Double Fault",       /* 8 */
        "Segment Overrun",    /* 9 */
        "Invalid TSS",        /* 10 */
        "Segment Not Present",/* 11 */
        "Stack Fault",        /* 12 */
        "General Protection", /* 13 */
        "Page Fault",         /* 14 */
    };

    const char *name = (f->int_no < 15) ? names[f->int_no] : "Unknown Exception";

    cli();
    vga_set_color(0x0C);
    printf("\n\n==============================================\n");
    printf("         VibeCagOS EXCEPTION\n");
    printf("==============================================\n");
    printf("Exception : #%u — %s\n", f->int_no, name);
    printf("Error Code: 0x%08x\n", f->err_code);
    printf("EIP       : 0x%08x\n", f->eip);
    printf("CS        : 0x%08x\n", f->cs);
    printf("EFLAGS    : 0x%08x\n", f->eflags);
    printf("EAX=%08x  EBX=%08x  ECX=%08x  EDX=%08x\n",
           f->eax, f->ebx, f->ecx, f->edx);
    printf("ESI=%08x  EDI=%08x  EBP=%08x\n",
           f->esi, f->edi, f->ebp);

    if (f->int_no == 14) {
        printf("CR2 (fault addr): 0x%08x\n", read_cr2());
        printf("Reason: %s, %s, %s\n",
               (f->err_code & 4) ? "User" : "Kernel",
               (f->err_code & 2) ? "Write" : "Read",
               (f->err_code & 1) ? "Protection violation" : "Not present");
    }

    if (current_proc)
        printf("Process: %s (pid %d)\n", current_proc->name, current_proc->pid);

    printf("==============================================\n");
    printf("System halted.\n");
    while (1) halt();
}

static void handle_syscall(struct trap_frame *f) {
    switch (f->eax) {
        case SYS_PUTCHAR:
            putchar((char)f->ebx);
            break;
        case SYS_GETCHAR:
            while (1) {
                int ch = keyboard_getchar();
                if (ch < 0)
                    ch = (int)serial_getchar();
                if (ch >= 0) {
                    f->ebx = (uint32_t)ch;
                    break;
                }
                yield();
            }
            break;
        case SYS_EXIT:
            printf("[sys] process %d exited with code %d\n",
                   current_proc ? current_proc->pid : 0, (int)f->ebx);
            if (current_proc) current_proc->state = PROC_ZOMBIE;
            yield();
            break;
        case SYS_READFILE: {
            const char *path = (const char *)f->ebx;
            void *buf = (void *)f->ecx;
            size_t len = (size_t)f->edx;
            struct file *file = vfs_open(path, FILE_READ, 0);
            if (!file) { f->eax = (uint32_t)-1; break; }
            int n = vfs_read(file, buf, len);
            vfs_close(file);
            f->eax = (uint32_t)n;
            break;
        }
        case SYS_WRITEFILE: {
            const char *path = (const char *)f->ebx;
            const void *buf = (const void *)f->ecx;
            size_t len = (size_t)f->edx;
            struct file *file = vfs_open(path, FILE_WRITE, VFS_PERM_DEFAULT_FILE);
            if (!file) { f->eax = (uint32_t)-1; break; }
            int n = vfs_write(file, buf, len);
            vfs_close(file);
            f->eax = (uint32_t)n;
            break;
        }
        case SYS_LS: {
            const char *path = (const char *)f->ebx;
            vibefs_ls(path ? path : "/");
            f->eax = 0;
            break;
        }
        case SYS_MKDIR: {
            const char *path = (const char *)f->ebx;
            f->eax = (uint32_t)vfs_mkdir(path, VFS_PERM_DEFAULT_DIR);
            break;
        }
        case SYS_UNLINK: {
            const char *path = (const char *)f->ebx;
            f->eax = (uint32_t)vfs_unlink(path);
            break;
        }
        case SYS_STAT: {
            const char *path = (const char *)f->ebx;
            struct vstat *st = (struct vstat *)f->ecx;
            f->eax = (uint32_t)vfs_stat(path, st);
            break;
        }
        case SYS_TIME: {
            struct rtc_time *t = (struct rtc_time *)f->ebx;
            if (t) rtc_get_time(t);
            f->eax = 0;
            break;
        }
        case SYS_GETPID:
            f->eax = current_proc ? (uint32_t)current_proc->pid : 0;
            break;
        case SYS_SLEEP:
            sleep_ms(f->ebx);
            break;
        case SYS_YIELD:
            yield();
            break;
        case SYS_UPTIME:
            f->eax = get_uptime_ms();
            break;
        default:
            printf("[syscall] Unknown: %u (eip=0x%08x)\n", f->eax, f->eip);
            f->eax = (uint32_t)-1;
            break;
    }
}

void handle_interrupt(struct trap_frame *f) {
    uint32_t n = f->int_no;

    if (n == 14) {
        /* Page Fault */
        vmm_page_fault_handler(f);
        return;
    }

    if (n < 32) {
        /* CPU exception */
        handle_exception(f);
        return;
    }

    if (n == 32) {
        /* PIT Timer — IRQ0 */
        ticks++;
        pic_eoi(0);
        /* Preempt: every tick, schedule */
        if (current_proc && current_proc != idle_proc)
            schedule();
        return;
    }

    if (n == 33) {
        /* PS/2 Keyboard — IRQ1 */
        keyboard_handle_irq();
        pic_eoi(1);
        return;
    }

    if (n == 44) {
        /* PS/2 Mouse — IRQ12 */
        uint8_t sc = inb(0x60);
        mouse_handle_byte(sc);
        pic_eoi(12);
        return;
    }

    if (n == 128) {
        /* Syscall */
        handle_syscall(f);
        return;
    }

    /* Unhandled IRQ — acknowledge and ignore */
    if (n >= 32 && n < 48)
        pic_eoi((uint8_t)(n - 32));
}

/* =========================================================================
 * Disk I/O wrapper for SimpleFS
 * ========================================================================= */

void read_write_disk(void *buf, unsigned sector, int is_write) {
    if (is_write)
        ide_write_sector(sector, buf);
    else
        ide_read_sector(sector, buf);
}

/* =========================================================================
 * IP address parser helper
 * ========================================================================= */

static bool parse_ipv4(const char *str, uint32_t *out) {
    uint32_t octets[4];
    int  idx = 0;
    uint32_t val = 0;
    bool has_digit = false;

    for (int i = 0; ; i++) {
        char c = str[i];
        if (c >= '0' && c <= '9') {
            val = val * 10 + (uint32_t)(c - '0');
            if (val > 255) return false;
            has_digit = true;
        } else if (c == '.' || c == '\0') {
            if (!has_digit || idx >= 4) return false;
            octets[idx++] = val;
            val = 0; has_digit = false;
            if (c == '\0') break;
        } else {
            return false;
        }
    }
    if (idx != 4) return false;
    *out = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return true;
}

/* =========================================================================
 * Shell Input — reads from keyboard or serial
 * ========================================================================= */

/* Command history */
#define HISTORY_SIZE 16
#define CMD_MAX      128

static char   history[HISTORY_SIZE][CMD_MAX];
static int    history_count  = 0;
/* history_pos used for future reverse-search feature */

static void history_add(const char *cmd) {
    if (cmd[0] == '\0') return;
    /* Don't duplicate last entry */
    if (history_count > 0 &&
        strcmp(history[(history_count - 1) % HISTORY_SIZE], cmd) == 0)
        return;
    strncpy(history[history_count % HISTORY_SIZE], cmd, CMD_MAX - 1);
    history_count++;
}

/*
 * Shell readline — supports:
 *   - backspace
 *   - arrow-up / arrow-down for history
 *   - accepts from both keyboard and serial
 */
static void shell_readline(char *buf, int maxlen) {
    int  len     = 0;
    int  hist_idx = history_count;  /* Pointer into history when browsing */
    char save[CMD_MAX] = {0};        /* Saves current input when browsing history */
    bool in_esc  = false;
    bool in_csi  = false;
    char csi_arg = 0;

    memset(buf, 0, (size_t)maxlen);

    while (1) {
        int ch = keyboard_getchar();
        if (ch < 0)
            ch = (int)serial_getchar();
        if (ch < 0) {
            /* No input — yield to avoid spinning */
            yield();
            continue;
        }

        /* ESC sequence handling */
        if (in_esc) {
            if (ch == '[') { in_csi = true; in_esc = false; continue; }
            in_esc = false;
            continue;
        }
        if (in_csi) {
            csi_arg = (char)ch;
            in_csi  = false;
            /* Arrow key dispatch */
            if (csi_arg == 'A') {
                /* Up — older history */
                if (hist_idx > 0 && hist_idx > history_count - HISTORY_SIZE) {
                    if (hist_idx == history_count)
                        strncpy(save, buf, CMD_MAX - 1);
                    hist_idx--;
                    /* Clear current line */
                    for (int i = 0; i < len; i++) { putchar('\b'); putchar(' '); putchar('\b'); }
                    strncpy(buf, history[hist_idx % HISTORY_SIZE], (size_t)(maxlen - 1));
                    len = (int)strlen(buf);
                    printf("%s", buf);
                }
            } else if (csi_arg == 'B') {
                /* Down — newer history */
                if (hist_idx < history_count) {
                    hist_idx++;
                    for (int i = 0; i < len; i++) { putchar('\b'); putchar(' '); putchar('\b'); }
                    if (hist_idx == history_count) {
                        strncpy(buf, save, (size_t)(maxlen - 1));
                    } else {
                        strncpy(buf, history[hist_idx % HISTORY_SIZE], (size_t)(maxlen - 1));
                    }
                    len = (int)strlen(buf);
                    printf("%s", buf);
                }
            }
            continue;
        }

        if (ch == '\x1B') { in_esc = true; continue; }

        if (ch == '\r' || ch == '\n') {
            putchar('\n');
            buf[len] = '\0';
            return;
        }

        if (ch == '\b' || ch == 127) {
            if (len > 0) {
                len--;
                buf[len] = '\0';
                putchar('\b'); putchar(' '); putchar('\b');
            }
            continue;
        }

        if (ch >= 32 && ch < 127 && len < maxlen - 1) {
            buf[len++] = (char)ch;
            buf[len]   = '\0';
            putchar((char)ch);
        }
    }
}

/* =========================================================================
 * Shell — Command Implementations
 * ========================================================================= */

static void cmd_help(void) {
    vga_set_color(0x0B);  /* Bright cyan */
    printf("VibeCagOS %s \u2014 Shell Commands\n", VIBECAGOS_VERSION);
    vga_set_color(0x07);  /* Light gray */
    printf("--- Filesystem ---\n");
    printf("  ls [path]        List directory contents\n");
    printf("  ls -l [path]     Detailed listing\n");
    printf("  pwd              Print working directory\n");
    printf("  cd <path>        Change directory\n");
    printf("  mkdir <path>     Create directory\n");
    printf("  mkdir -p <path>  Create directory tree\n");
    printf("  touch <file>     Create empty file\n");
    printf("  cat <file>       Display file contents\n");
    printf("  echo <msg> [> f] Print or write to file\n");
    printf("  cp <src> <dst>   Copy file\n");
    printf("  head <file>      Display first 10 lines\n");
    printf("  write <file>     Write to file interactively\n");
    printf("  rm <file>        Delete file\n");
    printf("  rmdir <dir>      Remove empty directory\n");
    printf("  mv <src> <dst>   Rename/move file\n");
    printf("  stat <path>      Show file information\n");
    printf("  hexdump <file>   Hex dump file contents\n");
    printf("--- System ---\n");
    printf("  uname            System information\n");
    printf("  uptime           System uptime\n");
    printf("  date             Current date and time (RTC)\n");
    printf("  free             Memory summary (PMM + Heap)\n");
    printf("  mem              Detailed memory statistics\n");
    printf("  ps               Process list\n");
    printf("  cpuinfo          CPU information\n");
    printf("  devices          List detected devices\n");
    printf("  pci              PCI device listing\n");
    printf("  dmesg            Kernel message log\n");
    printf("  mounts           Show mount table\n");
    printf("  fsinfo           Filesystem info\n");
    printf("  kill <pid>       Terminate a process\n");
    printf("  gui              Launch graphical desktop environment\n");
    printf("--- Network ---\n");
    printf("  ping <ip>        Ping IP address (ICMP)\n");
    printf("  dns <hostname>   Resolve domain name via DNS (UDP/53)\n");
    printf("  udpsend <ip> <p> <m> Send UDP datagram\n");
    printf("  udplisten <port> Listen for UDP packets\n");
    printf("--- Other ---\n");
    printf("  hello            Greeting\n");
    printf("  clear            Clear screen\n");
    printf("  exit             Halt system\n");
}

static void cmd_uname(void) {
    printf("VibeCagOS %s  (%s)\n", VIBECAGOS_VERSION, VIBECAGOS_BUILD);
    printf("Architecture: i386 (32-bit protected mode)\n");
    printf("Bootloader  : GRUB Multiboot v1\n");
    printf("Compiler    : Clang -m32\n");
    printf("Paging      : Enabled (identity map)\n");
    printf("Scheduler   : Preemptive round-robin (100 Hz)\n");
    printf("Heap        : kmalloc/kfree (first-fit)\n");
    printf("Filesystem  : VibeFS + procfs + devfs\n");
    printf("Network     : RTL8139 (polling) + ICMP\n");
    printf("Logger      : klog ring buffer (16 KB)\n");
}

/* =========================================================================
 * New Phase B commands
 * ========================================================================= */

static void cmd_dmesg(void) {
    uint32_t stored = klog_total_bytes();
    if (stored == 0) {
        printf("(kernel log is empty)\n");
        return;
    }
    vga_set_color(0x07);
    klog_dump();
}

static void cmd_hexdump(const char *filename) {
    while (*filename == ' ') filename++;
    if (!*filename) { printf("Usage: hexdump <file>\n"); return; }

    struct file *f = vfs_open(filename, FILE_READ, 0);
    if (!f) { printf("hexdump: cannot open '%s'\n", filename); return; }

    uint8_t buf[256];
    uint32_t addr = 0;
    int n;
    while ((n = vfs_read(f, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i += 16) {
            /* Address */
            vga_set_color(0x0B);
            printf("%08x  ", addr + (uint32_t)i);
            vga_set_color(0x07);
            /* Hex bytes */
            int end = i + 16 < n ? i + 16 : n;
            for (int j = i; j < i + 16; j++) {
                if (j < end)
                    printf("%02x ", (unsigned)buf[j]);
                else
                    printf("   ");
                if (j == i + 7) printf(" ");
            }
            printf(" |");
            /* ASCII */
            vga_set_color(0x0A);
            for (int j = i; j < end; j++) {
                char c = (char)buf[j];
                putchar((c >= 32 && c < 127) ? c : '.');
            }
            vga_set_color(0x07);
            printf("|\n");
        }
        addr += (uint32_t)n;
    }
    vfs_close(f);
    printf("\n%u bytes\n", addr);
}

static void cmd_cpuinfo(void) {
    printf("CPU Information:\n");
    printf("  Architecture : i386 (IA-32) 32-bit protected mode\n");
    printf("  Virtual CPU  : QEMU i386\n");
    printf("  Timer        : PIT 8254 @ 100 Hz\n");
    printf("  Paging       : 2-level (PD + PT) identity map\n");
    printf("  Uptime ticks : %u\n", ticks);
    printf("  Uptime ms    : %u\n", get_uptime_ms());
    /* Basic CPUID-like info via inline asm */
    uint32_t eax_out = 0, ebx_out = 0, ecx_out = 0, edx_out = 0;
    __asm__ __volatile__(
        "cpuid"
        : "=a"(eax_out), "=b"(ebx_out), "=c"(ecx_out), "=d"(edx_out)
        : "a"(0)
    );
    /* Vendor string is 12 bytes from EBX, EDX, ECX */
    char vendor[13];
    memcpy(vendor + 0, &ebx_out, 4);
    memcpy(vendor + 4, &edx_out, 4);
    memcpy(vendor + 8, &ecx_out, 4);
    vendor[12] = '\0';
    printf("  Vendor ID    : %s\n", vendor);
    /* Get max basic level */
    printf("  Max CPUID    : %u\n", eax_out);
    /* Get feature flags */
    __asm__ __volatile__("cpuid"
        : "=a"(eax_out), "=b"(ebx_out), "=c"(ecx_out), "=d"(edx_out)
        : "a"(1));
    printf("  CPUID[1].EDX : 0x%08x\n", edx_out);
    printf("  FPU          : %s\n", (edx_out & 1) ? "yes" : "no");
    printf("  APIC         : %s\n", (edx_out & (1<<9)) ? "yes" : "no");
    printf("  PAE          : %s\n", (edx_out & (1<<6)) ? "yes" : "no");
    printf("  SSE          : %s\n", (edx_out & (1<<25)) ? "yes" : "no");
}

static void cmd_devices(void) {
    printf("Detected Devices:\n");
    printf("  [CHRDEV] /dev/null     — null device (read=EOF, write=discard)\n");
    printf("  [CHRDEV] /dev/zero     — zero byte source\n");
    printf("  [CHRDEV] /dev/console  — kernel serial+VGA console\n");
    printf("  [CHRDEV] /dev/random   — pseudo-random bytes (xorshift32)\n");
    printf("  [CHRDEV] /dev/tty      — alias for /dev/console\n");
    printf("  [BLKDEV] IDE disk 0    — ATA PIO mode (2 MB disk.img)\n");
    printf("  [NETDEV] RTL8139       — 10/100 Ethernet (QEMU virtnet)\n");
    printf("  [TIMER]  PIT 8254      — 100 Hz preemptive timer\n");
    printf("  [INPUT]  PS/2 keyboard — scancode set 1 + ring buffer\n");
}

static void cmd_pci(void) {
    printf("PCI Devices:\n");
    if (nic_dev.found) {
        printf("  %02x:%02x.%x  Vendor: 0x%04x Device: 0x%04x  IO: 0x%04x  IRQ: %d  (Realtek RTL8139 Fast Ethernet)\n",
               nic_dev.bus, nic_dev.dev, nic_dev.func,
               nic_dev.vendor_id, nic_dev.device_id,
               nic_dev.io_base, nic_dev.irq_line);
    } else {
        printf("  No PCI devices enumerated\n");
    }
}

static void cmd_kill(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) { printf("Usage: kill <pid>\n"); return; }

    int target_pid = 0;
    while (*arg >= '0' && *arg <= '9') {
        target_pid = target_pid * 10 + (*arg - '0');
        arg++;
    }
    if (target_pid <= 0) { printf("kill: invalid PID\n"); return; }
    if (target_pid == 0) { printf("kill: cannot kill idle\n"); return; }

    for (int i = 0; i < PROCS_MAX; i++) {
        if (procs[i].pid == target_pid && procs[i].state != PROC_UNUSED) {
            if (procs[i].state == PROC_ZOMBIE) {
                printf("kill: pid %d is already zombie\n", target_pid);
                return;
            }
            procs[i].state    = PROC_ZOMBIE;
            procs[i].exit_code = -1;
            KINFO("KILL", "Process %d killed by shell", target_pid);
            printf("kill: sent SIGTERM to pid %d (%s)\n", target_pid, procs[i].name);
            return;
        }
    }
    printf("kill: no such process: %d\n", target_pid);
}

static void cmd_uptime(void) {
    uint32_t ms  = get_uptime_ms();
    uint32_t sec = ms / 1000;
    uint32_t min = sec / 60;
    sec %= 60;
    printf("Uptime: %u min %u sec (%u ms, %u ticks)\n", min, sec, ms, ticks);
}

static void cmd_free(void) {
    size_t total = pmm_get_total_bytes() / 1024;
    size_t used  = pmm_get_used_bytes() / 1024;
    size_t free  = pmm_get_free_bytes() / 1024;
    printf("             total        used        free\n");
    printf("Mem:    %8u KB %8u KB %8u KB  (%u frames free)\n",
           total, used, free, (unsigned)pmm_get_free_frames());
}

static void cmd_mem(void) {
    size_t total_bytes = pmm_get_total_bytes();
    size_t used_bytes  = pmm_get_used_bytes();
    size_t free_bytes  = pmm_get_free_bytes();

    printf("Physical Memory (PMM Bitmap Allocator):\n");
    printf("  Managed RAM pool: 0x%08x – 0x%08x\n",
           (uint32_t)__free_ram, (uint32_t)__free_ram_end);
    printf("  Total           : %u KB (%u frames)\n", total_bytes / 1024, (unsigned)pmm_get_total_frames());
    printf("  Allocated       : %u KB (%u frames)\n", used_bytes / 1024, (unsigned)pmm_get_used_frames());
    printf("  Free            : %u KB (%u frames)\n", free_bytes / 1024, (unsigned)pmm_get_free_frames());
    printf("Kernel image      : 0x%08x\n", (uint32_t)__kernel_base);
    printf("Kernel stack top  : 0x%08x\n", (uint32_t)__stack_top);
    printf("\n");
    kmalloc_dump_stats();
}

static void cmd_ps(void) {
    const char *state_names[] = {
        "UNUSED  ", "RUNNABLE", "RUNNING ",
        "SLEEPING", "ZOMBIE  ", "BLOCKED "
    };
    printf("PID  STATE     NAME\n");
    printf("---  --------  ----------------\n");
    for (int i = 0; i < PROCS_MAX; i++) {
        if (procs[i].state != PROC_UNUSED) {
            const char *sn = (procs[i].state < 6) ? state_names[procs[i].state] : "UNKNOWN ";
            printf("%-4d %s  %s\n", procs[i].pid, sn, procs[i].name);
        }
    }
}

static void cmd_ping(const char *ip_str) {
    while (*ip_str == ' ') ip_str++;
    if (*ip_str == '\0') {
        printf("Usage: ping <ip>\n");
        return;
    }

    uint32_t target_ip;
    if (!parse_ipv4(ip_str, &target_ip)) {
        printf("Error: Invalid IP address '%s'\n", ip_str);
        return;
    }

    uint8_t  dst_mac[6];
    uint32_t next_hop = target_ip;
    if ((target_ip & NET_NETMASK) != (NET_IP & NET_NETMASK))
        next_hop = NET_GATEWAY;

    bool arp_ok = false;
    for (int a = 0; a < 3; a++) {
        if (arp_resolve(next_hop, dst_mac)) { arp_ok = true; break; }
    }

    if (!arp_ok) {
        printf("Host unreachable (ARP failed)\n");
        return;
    }

    printf("PING %d.%d.%d.%d: %d bytes of data\n",
           (target_ip >> 24) & 0xFF, (target_ip >> 16) & 0xFF,
           (target_ip >> 8) & 0xFF,   target_ip & 0xFF,
           PING_DATA_LEN);

    struct ping_result res;
    ping(target_ip, 4, &res);

    int loss = res.sent > 0 ? ((res.sent - res.received) * 100) / res.sent : 100;
    int avg  = res.received > 0 ? (int)(res.total_ms / res.received) : 0;

    printf("--- Statistics ---\n");
    printf("%d transmitted, %d received, %d%% loss\n",
           res.sent, res.received, loss);
    if (res.received > 0)
        printf("min/avg/max = %d/%d/%d ms\n", res.min_ms, avg, res.max_ms);
}

static void cmd_write_file(const char *filename) {
    /* Skip leading spaces */
    while (*filename == ' ') filename++;

    printf("Enter content (empty line to finish):\n");
    char content[2048];
    int  clen = 0;

    while (clen < 2000) {
        printf("| ");
        char line[128];
        shell_readline(line, sizeof(line));
        if (line[0] == '\0') break;

        int ll = (int)strlen(line);
        for (int i = 0; i < ll && clen < 2000; i++)
            content[clen++] = line[i];
        if (clen < 2000)
            content[clen++] = '\n';
    }

    struct file *f = vfs_open(filename, FILE_WRITE, VFS_PERM_DEFAULT_FILE);
    if (!f) {
        /* Try creating first */
        if (vfs_mkdir(filename, 0) == 0) {
            printf("Error: '%s' is a directory\n", filename);
            return;
        }
        f = vfs_open(filename, FILE_WRITE, VFS_PERM_DEFAULT_FILE);
    }
    if (!f) {
        printf("Error: cannot open '%s' for writing\n", filename);
        return;
    }
    int ret = vfs_write(f, content, (size_t)clen);
    vfs_close(f);
    if (ret >= 0)
        printf("Wrote %d bytes to '%s'\n", ret, filename);
    else
        printf("Error writing to '%s'\n", filename);
}

static void cmd_ls(const char *path, bool detailed) {
    (void)detailed;
    char cwd[VFS_PATH_MAX];
    vfs_get_cwd(cwd, sizeof(cwd));

    const char *target = (path && path[0]) ? path : cwd;
    vibefs_ls(target);
}

static void cmd_cat_vfs(const char *filename) {
    while (*filename == ' ') filename++;
    if (!*filename) {
        printf("Usage: cat <file>\n");
        return;
    }

    struct file *f = vfs_open(filename, FILE_READ, 0);
    if (!f) {
        /* Fall back to procfs-style read */
        printf("cat: cannot open '%s'\n", filename);
        return;
    }

    char buf[512];
    int n;
    while ((n = vfs_read(f, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++)
            putchar(buf[i]);
    }
    if (n == 0)
        putchar('\n');
    vfs_close(f);
}

static void cmd_touch(const char *filename) {
    while (*filename == ' ') filename++;
    if (!*filename) { printf("Usage: touch <file>\n"); return; }

    struct file *f = vfs_open(filename, FILE_WRITE, VFS_PERM_DEFAULT_FILE);
    if (!f) {
        printf("touch: cannot create '%s'\n", filename);
        return;
    }
    vfs_close(f);
    printf("'%s' created\n", filename);
}

static void cmd_mkdir_p(const char *path, bool parents) {
    while (*path == ' ') path++;
    if (!*path) { printf("Usage: mkdir <path>\n"); return; }

    if (!parents) {
        int r = vfs_mkdir(path, VFS_PERM_DEFAULT_DIR);
        if (r == 0) printf("mkdir: created '%s'\n", path);
        else if (r == VFS_EEXIST) printf("mkdir: '%s' already exists\n", path);
        else printf("mkdir: cannot create '%s' (err %d)\n", path, r);
        return;
    }

    /* Parents mode — create all components */
    char work[VFS_PATH_MAX];
    strncpy(work, path, VFS_PATH_MAX - 1);
    work[VFS_PATH_MAX - 1] = '\0';

    char build[VFS_PATH_MAX] = "/";
    char *tok = work;
    /* Skip leading slash */
    if (*tok == '/') tok++;

    while (*tok) {
        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';

        /* Append component to build path */
        if (strlen(build) + strlen(tok) + 2 < VFS_PATH_MAX) {
            if (build[strlen(build) - 1] != '/')
                strcat(build, "/");
            strcat(build, tok);
        }

        int r = vfs_mkdir(build, VFS_PERM_DEFAULT_DIR);
        if (r != 0 && r != VFS_EEXIST) {
            printf("mkdir: cannot create '%s' (err %d)\n", build, r);
            if (slash) *slash = '/';
            return;
        }

        if (slash) {
            *slash = '/';
            tok = slash + 1;
        } else {
            break;
        }
    }
    printf("mkdir: created '%s'\n", path);
}

static void cmd_pwd(void) {
    char cwd[VFS_PATH_MAX];
    vfs_get_cwd(cwd, sizeof(cwd));
    printf("%s\n", cwd);
}

static void cmd_cd(const char *path) {
    while (*path == ' ') path++;
    if (!*path) path = "/";

    int r = vfs_set_cwd(path);
    if (r != 0)
        printf("cd: '%s': no such directory\n", path);
}

static void cmd_rm_vfs(const char *path) {
    while (*path == ' ') path++;
    if (!*path) { printf("Usage: rm <file>\n"); return; }

    int r = vfs_unlink(path);
    if (r == 0) printf("rm: removed '%s'\n", path);
    else if (r == VFS_EISDIR) printf("rm: '%s' is a directory; use rmdir\n", path);
    else printf("rm: cannot remove '%s' (err %d)\n", path, r);
}

static void cmd_rmdir_vfs(const char *path) {
    while (*path == ' ') path++;
    if (!*path) { printf("Usage: rmdir <dir>\n"); return; }

    int r = vfs_rmdir(path);
    if (r == 0) printf("rmdir: removed '%s'\n", path);
    else if (r == VFS_ENOTEMPTY) printf("rmdir: '%s': directory not empty\n", path);
    else printf("rmdir: cannot remove '%s' (err %d)\n", path, r);
}

static void cmd_mv(const char *src, const char *dst) {
    int r = vfs_rename(src, dst);
    if (r == 0) printf("mv: '%s' -> '%s'\n", src, dst);
    else printf("mv: cannot rename '%s' to '%s' (err %d)\n", src, dst, r);
}

static void cmd_stat(const char *path) {
    while (*path == ' ') path++;
    if (!*path) { printf("Usage: stat <path>\n"); return; }

    struct vstat st;
    int r = vfs_stat(path, &st);
    if (r != 0) {
        printf("stat: '%s': no such file or directory\n", path);
        return;
    }

    const char *typestr = "regular file";
    if (st.type == VFS_TYPE_DIR) typestr = "directory";
    else if (st.type == VFS_TYPE_CHRDEV) typestr = "character device";

    printf("  File  : %s\n", path);
    printf("  Inode : %u\n", st.ino);
    printf("  Type  : %s\n", typestr);
    printf("  Size  : %u bytes\n", st.size);
    printf("  Links : %u\n", st.nlink);
    printf("  Mode  : %o\n", (unsigned)st.mode);
}

static void cmd_echo(const char *arg) {
    while (*arg == ' ') arg++;
    /* Check for redirection: > */
    const char *redir = strchr(arg, '>');
    if (redir) {
        char text[CMD_MAX];
        char dest[CMD_MAX];
        int tlen = (int)(redir - arg);
        while (tlen > 0 && arg[tlen - 1] == ' ') tlen--;
        if (tlen >= CMD_MAX) tlen = CMD_MAX - 1;
        strncpy(text, arg, (size_t)tlen);
        text[tlen] = '\0';

        const char *dst = redir + 1;
        while (*dst == ' ') dst++;
        strncpy(dest, dst, CMD_MAX - 1);
        dest[CMD_MAX - 1] = '\0';

        if (dest[0] == '\0') {
            printf("echo: missing destination file\n");
            return;
        }

        struct file *f = vfs_open(dest, FILE_WRITE, VFS_PERM_DEFAULT_FILE);
        if (!f) {
            printf("echo: cannot open '%s' for writing\n", dest);
            return;
        }
        vfs_write(f, text, strlen(text));
        vfs_write(f, "\n", 1);
        vfs_close(f);
        return;
    }

    printf("%s\n", arg);
}

static void cmd_cp(const char *src, const char *dst) {
    while (*src == ' ') src++;
    while (*dst == ' ') dst++;

    if (!*src || !*dst) {
        printf("Usage: cp <src> <dst>\n");
        return;
    }

    struct file *f_in = vfs_open(src, FILE_READ, 0);
    if (!f_in) {
        printf("cp: cannot open '%s' for reading\n", src);
        return;
    }

    struct file *f_out = vfs_open(dst, FILE_WRITE, VFS_PERM_DEFAULT_FILE);
    if (!f_out) {
        printf("cp: cannot open '%s' for writing\n", dst);
        vfs_close(f_in);
        return;
    }

    char buf[512];
    int n;
    int total = 0;
    while ((n = vfs_read(f_in, buf, sizeof(buf))) > 0) {
        vfs_write(f_out, buf, (size_t)n);
        total += n;
    }

    vfs_close(f_in);
    vfs_close(f_out);
    printf("cp: copied %d bytes from '%s' to '%s'\n", total, src, dst);
}

static void cmd_head(const char *filename) {
    while (*filename == ' ') filename++;
    if (!*filename) { printf("Usage: head <file>\n"); return; }

    struct file *f = vfs_open(filename, FILE_READ, 0);
    if (!f) { printf("head: cannot open '%s'\n", filename); return; }

    char buf[512];
    int n;
    int lines = 0;
    while ((n = vfs_read(f, buf, sizeof(buf))) > 0 && lines < 10) {
        for (int i = 0; i < n && lines < 10; i++) {
            putchar(buf[i]);
            if (buf[i] == '\n') lines++;
        }
    }
    vfs_close(f);
}

static void cmd_date(void) {
    char buf[32];
    rtc_format_time(buf, sizeof(buf));
    printf("%s UTC\n", buf);
}

static void cmd_dns(const char *hostname) {
    while (*hostname == ' ') hostname++;
    if (!*hostname) { printf("Usage: dns <hostname>\n"); return; }

    printf("Resolving %s...\n", hostname);
    uint32_t ip = 0;
    int r = dns_resolve(hostname, &ip);
    if (r == 0) {
        printf("%s has address %d.%d.%d.%d\n",
               hostname,
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
               (ip >> 8) & 0xFF, ip & 0xFF);
    } else {
        printf("dns: resolution failed for '%s'\n", hostname);
    }
}

static void cmd_udpsend(const char *args) {
    while (*args == ' ') args++;
    char ip_str[32];
    char port_str[16];
    const char *p = args;

    while (*p && *p != ' ') p++;
    int iplen = (int)(p - args);
    if (iplen >= 32 || iplen == 0) { printf("Usage: udpsend <ip> <port> <message>\n"); return; }
    strncpy(ip_str, args, (size_t)iplen);
    ip_str[iplen] = '\0';
    while (*p == ' ') p++;

    const char *p2 = p;
    while (*p2 && *p2 != ' ') p2++;
    int plen = (int)(p2 - p);
    if (plen >= 16 || plen == 0) { printf("Usage: udpsend <ip> <port> <message>\n"); return; }
    strncpy(port_str, p, (size_t)plen);
    port_str[plen] = '\0';
    while (*p2 == ' ') p2++;

    uint32_t dst_ip;
    if (!parse_ipv4(ip_str, &dst_ip)) { printf("udpsend: invalid IP '%s'\n", ip_str); return; }

    uint16_t port = 0;
    for (int i = 0; port_str[i]; i++) {
        if (port_str[i] >= '0' && port_str[i] <= '9')
            port = port * 10 + (port_str[i] - '0');
    }

    const char *msg = p2;
    if (!*msg) msg = "Hello from VibeCagOS UDP!";

    int r = udp_send(dst_ip, 54321, port, msg, (uint16_t)strlen(msg));
    if (r == 0) {
        printf("Sent %u bytes to %s:%u\n", (unsigned)strlen(msg), ip_str, port);
    } else {
        printf("udpsend: failed to send packet\n");
    }
}

static void cmd_udplisten(const char *arg) {
    while (*arg == ' ') arg++;
    uint16_t port = 0;
    while (*arg >= '0' && *arg <= '9') {
        port = port * 10 + (*arg - '0');
        arg++;
    }
    if (port == 0) port = 7777;

    int sock = net_socket_open(port);
    if (sock < 0) { printf("udplisten: cannot open socket\n"); return; }

    printf("Listening for UDP packets on port %u (waiting up to 5s, press any key to abort)...\n", port);
    uint32_t start = get_uptime_ms();
    char buf[512];
    uint32_t src_ip = 0;
    uint16_t src_port = 0;

    while (get_uptime_ms() - start < 5000) {
        int n = net_socket_recvfrom(sock, buf, sizeof(buf) - 1, &src_ip, &src_port);
        if (n > 0) {
            buf[n] = '\0';
            printf("Received %d bytes from %d.%d.%d.%d:%u -> \"%s\"\n",
                   n, (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                   (src_ip >> 8) & 0xFF, src_ip & 0xFF, src_port, buf);
            net_socket_close(sock);
            return;
        }
        if (keyboard_getchar() >= 0) break;
        io_wait();
    }
    printf("udplisten: timed out\n");
    net_socket_close(sock);
}

static void cmd_gui(void) {
    gui_launch_desktop();
}

/* =========================================================================
 * Boot Splash
 * ========================================================================= */

static void print_splash(void) {
    vga_clear();
    vga_set_color(0x0B);  /* Bright cyan */
    printf("\n");
    printf("  +-------------------------------------------------+\n");
    printf("  |                                                 |\n");
    printf("  |            V I B E C A G O S                   |\n");
    printf("  |                                                 |\n");
    vga_set_color(0x03);  /* Cyan */
    printf("  |          Version %s  %s             |\n",
           VIBECAGOS_VERSION, VIBECAGOS_BUILD);
    vga_set_color(0x07);  /* Light gray */
    printf("  |          i386 Protected Mode  |  kmalloc + VFS  |\n");
    printf("  |          VibeFS + procfs  |  Round-Robin Sched  |\n");
    printf("  |                                                 |\n");
    printf("  +-------------------------------------------------+\n");
    printf("\n");
    vga_set_color(0x07);
}

static void print_ok(const char *subsystem) {
    vga_set_color(0x0A);  /* Bright green */
    printf("[  OK  ] ");
    vga_set_color(0x07);
    printf("%s\n", subsystem);
}

/* =========================================================================
 * Shell Main Loop
 * ========================================================================= */

static void run_shell(void) {
    vga_set_color(0x0B);
    printf("\nVibeCagOS %s shell ready. Type 'help' for commands.\n\n",
           VIBECAGOS_VERSION);
    vga_set_color(0x07);

    char cmdline[CMD_MAX];
    char cwd_buf[VFS_PATH_MAX];

    while (1) {
        /* Show cwd in prompt */
        vfs_get_cwd(cwd_buf, sizeof(cwd_buf));

        vga_set_color(0x0A);  /* Bright green */
        printf("vcos");
        vga_set_color(0x0E);  /* Yellow */
        printf(":%s", cwd_buf);
        vga_set_color(0x0F);  /* White */
        printf("$ ");
        vga_set_color(0x07);  /* Light gray */

        shell_readline(cmdline, sizeof(cmdline));
        history_add(cmdline);

        if (cmdline[0] == '\0') {
            continue;
        } else if (strcmp(cmdline, "help") == 0) {
            cmd_help();
        } else if (strcmp(cmdline, "hello") == 0) {
            printf("Hello from VibeCagOS %s!\n", VIBECAGOS_VERSION);
        } else if (strcmp(cmdline, "uname") == 0) {
            cmd_uname();
        } else if (strcmp(cmdline, "uptime") == 0) {
            cmd_uptime();
        } else if (strcmp(cmdline, "mem") == 0) {
            cmd_mem();
        } else if (strcmp(cmdline, "ps") == 0) {
            cmd_ps();
        } else if (strcmp(cmdline, "free") == 0) {
            cmd_free();
        } else if (strcmp(cmdline, "date") == 0) {
            cmd_date();
        } else if (strcmp(cmdline, "dmesg") == 0) {
            cmd_dmesg();
        } else if (strncmp(cmdline, "hexdump ", 8) == 0) {
            cmd_hexdump(cmdline + 8);
        } else if (strcmp(cmdline, "cpuinfo") == 0) {
            cmd_cpuinfo();
        } else if (strcmp(cmdline, "devices") == 0) {
            cmd_devices();
        } else if (strcmp(cmdline, "pci") == 0) {
            cmd_pci();
        } else if (strncmp(cmdline, "kill ", 5) == 0) {
            cmd_kill(cmdline + 5);
        } else if (strcmp(cmdline, "gui") == 0) {
            cmd_gui();

        /* --- Filesystem commands --- */
        } else if (strcmp(cmdline, "ls") == 0) {
            cmd_ls(NULL, false);
        } else if (strncmp(cmdline, "ls -l ", 6) == 0) {
            cmd_ls(cmdline + 6, true);
        } else if (strncmp(cmdline, "ls ", 3) == 0) {
            cmd_ls(cmdline + 3, false);
        } else if (strcmp(cmdline, "pwd") == 0) {
            cmd_pwd();
        } else if (strncmp(cmdline, "cd ", 3) == 0) {
            cmd_cd(cmdline + 3);
        } else if (strcmp(cmdline, "cd") == 0) {
            cmd_cd("/");
        } else if (strncmp(cmdline, "mkdir -p ", 9) == 0) {
            cmd_mkdir_p(cmdline + 9, true);
        } else if (strncmp(cmdline, "mkdir ", 6) == 0) {
            cmd_mkdir_p(cmdline + 6, false);
        } else if (strncmp(cmdline, "touch ", 6) == 0) {
            cmd_touch(cmdline + 6);
        } else if (strncmp(cmdline, "cat ", 4) == 0) {
            cmd_cat_vfs(cmdline + 4);
        } else if (strncmp(cmdline, "echo ", 5) == 0) {
            cmd_echo(cmdline + 5);
        } else if (strcmp(cmdline, "echo") == 0) {
            printf("\n");
        } else if (strncmp(cmdline, "cp ", 3) == 0) {
            char src[CMD_MAX], dst[CMD_MAX];
            const char *args = cmdline + 3;
            while (*args == ' ') args++;
            const char *sp = args;
            while (*sp && *sp != ' ') sp++;
            int slen = (int)(sp - args);
            strncpy(src, args, (size_t)slen);
            src[slen] = '\0';
            while (*sp == ' ') sp++;
            strncpy(dst, sp, CMD_MAX - 1);
            dst[CMD_MAX - 1] = '\0';
            if (src[0] && dst[0]) cmd_cp(src, dst);
            else printf("Usage: cp <src> <dst>\n");
        } else if (strncmp(cmdline, "head ", 5) == 0) {
            cmd_head(cmdline + 5);
        } else if (strncmp(cmdline, "write ", 6) == 0) {
            cmd_write_file(cmdline + 6);
        } else if (strncmp(cmdline, "rm ", 3) == 0) {
            cmd_rm_vfs(cmdline + 3);
        } else if (strncmp(cmdline, "rmdir ", 6) == 0) {
            cmd_rmdir_vfs(cmdline + 6);
        } else if (strncmp(cmdline, "mv ", 3) == 0) {
            /* Parse source and dest */
            char src[CMD_MAX], dst[CMD_MAX];
            const char *args = cmdline + 3;
            while (*args == ' ') args++;
            /* Find space separator */
            const char *sp = args;
            while (*sp && *sp != ' ') sp++;
            int slen = (int)(sp - args);
            strncpy(src, args, (size_t)slen);
            src[slen] = '\0';
            while (*sp == ' ') sp++;
            strncpy(dst, sp, CMD_MAX - 1);
            dst[CMD_MAX - 1] = '\0';
            if (src[0] && dst[0]) cmd_mv(src, dst);
            else printf("Usage: mv <src> <dst>\n");
        } else if (strncmp(cmdline, "stat ", 5) == 0) {
            cmd_stat(cmdline + 5);

        /* --- Old SimpleFS compat commands (for format/create) --- */
        } else if (strncmp(cmdline, "create ", 7) == 0) {
            cmd_touch(cmdline + 7);
        } else if (strcmp(cmdline, "format") == 0) {
            printf("Type 'yes' to confirm format: ");
            char confirm[8];
            shell_readline(confirm, sizeof(confirm));
            if (strcmp(confirm, "yes") == 0) {
                vibefs_format();
                vibefs_mount();
                /* Re-mount VFS */
                struct vnode *root = vibefs_get_vfs_root();
                if (root) {
                    vfs_unmount("/");
                    vfs_mount("/", &vibefs_vfs_ops, &vibefs, root);
                    vfs_vnode_put(root);
                }
                printf("Filesystem re-formatted and mounted.\n");
            } else {
                printf("Format cancelled.\n");
            }

        /* --- System info commands --- */
        } else if (strcmp(cmdline, "mounts") == 0) {
            vfs_dump_mounts();
        } else if (strcmp(cmdline, "fsinfo") == 0) {
            vibefs_info();
        } else if (strcmp(cmdline, "heap") == 0) {
            kmalloc_dump_stats();

        /* --- Network --- */
        } else if (strncmp(cmdline, "ping ", 5) == 0) {
            cmd_ping(cmdline + 5);
        } else if (strncmp(cmdline, "dns ", 4) == 0) {
            cmd_dns(cmdline + 4);
        } else if (strncmp(cmdline, "udpsend ", 8) == 0) {
            cmd_udpsend(cmdline + 8);
        } else if (strncmp(cmdline, "udplisten ", 10) == 0) {
            cmd_udplisten(cmdline + 10);

        } else if (strcmp(cmdline, "clear") == 0) {
            vga_clear();
        } else if (strcmp(cmdline, "exit") == 0) {
            vga_set_color(0x0E);
            printf("System halting. Goodbye!\n");
            vga_set_color(0x07);
            vfs_sync();
            break;
        } else {
            vga_set_color(0x0C);
            printf("Unknown command: ");
            vga_set_color(0x07);
            printf("%s\n", cmdline);
            printf("Type 'help' for available commands.\n");
        }
    }
}

/* =========================================================================
 * kernel_main — Entry point called by boot.s
 * ========================================================================= */

void kernel_main(uint32_t mb_magic, struct multiboot_info *mb_info) {
    /* Zero BSS */
    memset(__bss, 0, (size_t)((uint32_t)__bss_end - (uint32_t)__bss));

    /* Store multiboot arguments */
    g_mb_magic = mb_magic;
    g_mb_info  = mb_info;

    /* Early I/O */
    vga_init();
    serial_init();

    /* Boot splash */
    print_splash();

    /* --- Phase 0: Kernel logger (must be first) --- */
    klog_init();

    if (g_mb_magic == MULTIBOOT_BOOTLOADER_MAGIC && g_mb_info) {
        if (g_mb_info->flags & MULTIBOOT_INFO_BOOT_LOADER) {
            KINFO("BOOT", "Loaded by %s", (const char *)g_mb_info->boot_loader_name);
        }
        if (g_mb_info->flags & MULTIBOOT_INFO_MEMORY) {
            KINFO("BOOT", "BIOS memory lower=%u KB, upper=%u KB",
                  g_mb_info->mem_lower, g_mb_info->mem_upper);
        }
    }

    /* --- Phase 1: Architecture Foundation --- */
    printf("Initializing kernel subsystems...\n\n");

    /* Physical memory allocator */
    pmm_init((paddr_t)__free_ram, (paddr_t)__free_ram_end);
    KINFO("PMM", "Physical memory allocator ready (%u KB, %u frames)",
          (unsigned)(pmm_get_total_bytes() / 1024), (unsigned)pmm_get_total_frames());
    print_ok("Physical memory allocator (bitmap)");

    /* GDT with proper segment descriptors and TSS */
    gdt_init();
    KINFO("GDT", "GDT loaded (kernel/user segments + TSS)");
    print_ok("GDT (kernel/user segments + TSS)");

    /* IDT — exception + IRQ + syscall handlers */
    idt_init();
    KINFO("IDT", "IDT loaded (exceptions + IRQs + syscall 0x80)");
    print_ok("IDT (exceptions + IRQs + syscall)");

    /* PIC — remapped, IRQ0+IRQ1+IRQ2 unmasked */
    pic_init();
    print_ok("PIC (IRQ0/timer + IRQ1/keyboard + IRQ12/mouse)");

    /* PIT timer at 100 Hz */
    pit_init(100);
    print_ok("PIT timer (100 Hz preemptive)");

    /* Enable interrupts */
    sti();
    print_ok("Interrupts enabled");

    /* Paging — identity map via VMM */
    paging_init();
    print_ok("Paging enabled (2-level VMM)");

    /* Keyboard */
    keyboard_init();
    print_ok("PS/2 keyboard driver");

    /* CMOS Real-Time Clock */
    rtc_init();
    print_ok("CMOS Real-Time Clock");

    /* --- Phase 1.5: Kernel Heap --- */
    kmalloc_init();
    KINFO("HEAP", "kmalloc/kfree initialized");
    print_ok("Kernel heap (kmalloc/kfree)");

    /* --- Phase 2: Devices --- */
    ide_init();
    KINFO("IDE", "IDE disk controller initialized");
    print_ok("IDE disk controller");

    pci_init();
    KINFO("PCI", "PCI bus enumeration complete");
    print_ok("PCI bus scan");

    nic_init();
    KINFO("NIC", "RTL8139 NIC initialized");
    print_ok("RTL8139 NIC");

    /* UDP protocol stack */
    udp_init();
    print_ok("UDP protocol stack");

    /* --- Phase 3: Virtual Filesystem --- */
    vfs_init();
    print_ok("VFS layer");

    /* Mount VibeFS as root */
    if (vibefs_mount() == 0) {
        struct vnode *root = vibefs_get_vfs_root();
        if (root) {
            vfs_mount("/", &vibefs_vfs_ops, &vibefs, root);
            vfs_vnode_put(root);
            print_ok("VibeFS root filesystem");

            /* Create standard directory tree if not exists */
            static const char *stdirs[] = {
                "/bin", "/etc", "/home", "/home/user",
                "/tmp", "/var", "/var/log",
                "/usr", "/usr/bin", "/usr/lib",
                "/dev", "/proc", "/sys", "/mnt",
                NULL
            };
            for (int i = 0; stdirs[i]; i++) {
                int r = vfs_mkdir(stdirs[i], VFS_PERM_DEFAULT_DIR);
                if (r != 0 && r != VFS_EEXIST) {
                    printf("  [WARN] mkdir %s failed: %d\n", stdirs[i], r);
                }
            }

            /* Create /etc/version */
            struct file *f = vfs_open("/etc/version", FILE_WRITE, VFS_PERM_DEFAULT_FILE);
            if (f) {
                const char *ver = "VibeCagOS 0.4.0\n";
                vfs_write(f, ver, strlen(ver));
                vfs_close(f);
            }
            /* Create /etc/hostname */
            struct file *fh = vfs_open("/etc/hostname", FILE_WRITE, VFS_PERM_DEFAULT_FILE);
            if (fh) {
                const char *hn = "vibecagos\n";
                vfs_write(fh, hn, strlen(hn));
                vfs_close(fh);
            }
        } else {
            printf("  [FAIL] Could not get VibeFS root vnode\n");
        }
    } else {
        printf("  [FAIL] VibeFS mount failed\n");
    }

    /* Mount procfs at /proc */
    procfs_init();
    struct vnode *proc_root = procfs_get_root();
    if (proc_root) {
        vfs_mount("/proc", &procfs_vfs_ops, NULL, proc_root);
        KINFO("PROC", "procfs mounted at /proc");
        print_ok("procfs (/proc)");
    }

    /* Mount devfs at /dev */
    devfs_init();
    struct vnode *dev_root = devfs_get_root();
    if (dev_root) {
        vfs_mount("/dev", &devfs_vfs_ops, NULL, dev_root);
        vfs_vnode_put(dev_root);
        KINFO("DEV", "devfs mounted at /dev (null,zero,console,random,tty)");
        print_ok("devfs (/dev)");
    }

    /* --- Phase 4: Process subsystem --- */
    /* Initialize idle process as process 0 */
    memset(procs, 0, sizeof(procs));

    idle_proc = &procs[0];
    idle_proc->pid   = 0;
    idle_proc->state = PROC_RUNNABLE;
    strncpy(idle_proc->name, "idle", sizeof(idle_proc->name) - 1);

    uint32_t *sp = (uint32_t *)&idle_proc->stack[KERNEL_STACK];
    *--sp = 0;             /* edi */
    *--sp = 0;             /* esi */
    *--sp = 0;             /* ebx */
    *--sp = 0;             /* ebp */
    *--sp = (uint32_t)idle_task;
    idle_proc->sp = (uint32_t)sp;

    current_proc = idle_proc;
    tss_set_kernel_stack((uint32_t)&idle_proc->stack[KERNEL_STACK]);

    print_ok("Process scheduler");
    printf("\n");

    /* --- Shell --- */
    run_shell();

    /* Flush filesystem on exit */
    vfs_sync();

    /* Halt */
    printf("\nVibeCagOS halted.\n");
    /* QEMU / Bochs ACPI poweroff */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    cli();
    while (1) halt();
}
