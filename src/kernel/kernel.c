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
 *   - Paging — identity map with kernel/user split
 *   - Physical allocator — bitmap-based
 *   - Process scheduler — preemptive round-robin
 *   - IDE disk — ATA PIO
 *   - PCI — bus scan
 *   - RTL8139 NIC — polling
 *   - SimpleFS — persistent disk filesystem
 *   - Shell — interactive colored terminal
 */

#include "kernel.h"
#include "common.h"
#include "../fs/simplefs.h"
#include "../drivers/vga.h"
#include "../drivers/ide.h"
#include "../drivers/pci.h"
#include "../drivers/rtl8139.h"
#include "../net/ethernet.h"
#include "../net/arp.h"
#include "../net/ipv4.h"
#include "../net/icmp.h"
#include "../net/netconfig.h"

/* Linker-provided symbols */
extern char __kernel_base[];
extern char __stack_top[];
extern char __bss[], __bss_end[];
extern char __free_ram[], __free_ram_end[];

/* =========================================================================
 * VibeCagOS Version
 * ========================================================================= */

#define VIBECAGOS_VERSION "0.2.0"
#define VIBECAGOS_BUILD   "2026-10-02"

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
     * Mask all IRQs initially.
     * We unmask IRQ0 (timer) and IRQ1 (keyboard) after setting up handlers.
     * Bit=0 means unmasked (enabled), bit=1 means masked (disabled).
     * IRQ0=bit0, IRQ1=bit1, etc.
     */
    outb(PIC1_DATA, 0xFC);  /* Enable IRQ0 (timer) and IRQ1 (keyboard) only */
    outb(PIC2_DATA, 0xFF);  /* All slave IRQs masked */
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
 * Physical Memory Allocator (bump allocator)
 * The linker provides __free_ram and __free_ram_end.
 * ========================================================================= */

static paddr_t next_paddr;
static uint32_t pages_allocated = 0;
static uint32_t pages_total     = 0;

void pmm_init(void) {
    next_paddr = (paddr_t)__free_ram;
    pages_total = ((paddr_t)__free_ram_end - (paddr_t)__free_ram) / PAGE_SIZE;
}

paddr_t alloc_pages(uint32_t n) {
    paddr_t paddr = next_paddr;
    next_paddr += n * PAGE_SIZE;

    if (next_paddr > (paddr_t)__free_ram_end)
        PANIC("Out of physical memory (%u pages requested)", n);

    memset((void *)paddr, 0, n * PAGE_SIZE);
    pages_allocated += n;
    return paddr;
}

/* =========================================================================
 * Paging — 2-level x86 page tables
 * ========================================================================= */

void map_page(uint32_t *page_dir, uint32_t vaddr, paddr_t paddr, uint32_t flags) {
    uint32_t pde_idx = vaddr >> 22;
    uint32_t pte_idx = (vaddr >> 12) & 0x3FF;

    if ((page_dir[pde_idx] & PAGE_PRESENT) == 0) {
        uint32_t pt_paddr = alloc_pages(1);
        page_dir[pde_idx] = pt_paddr | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }

    uint32_t *page_table = (uint32_t *)(page_dir[pde_idx] & ~0xFFFu);
    page_table[pte_idx]  = paddr | flags | PAGE_PRESENT;
}

/*
 * Create a kernel page directory with the kernel identity-mapped.
 */
static uint32_t *create_kernel_page_dir(void) {
    uint32_t *pd = (uint32_t *)alloc_pages(1);

    /* Identity-map everything from 0 to __free_ram_end */
    for (paddr_t p = 0; p < (paddr_t)__free_ram_end; p += PAGE_SIZE) {
        map_page(pd, p, p, PAGE_WRITE);
    }

    return pd;
}

static uint32_t *kernel_page_dir = NULL;

void paging_init(void) {
    kernel_page_dir = create_kernel_page_dir();
    load_cr3((uint32_t)kernel_page_dir);
    enable_paging();
    printf("[  OK  ] Paging enabled (identity map)\n");
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

    va_list args;
    va_start(args, fmt);
    /* We don't have vprintf, use printf with the format */
    (void)args;
    printf(fmt);  /* Best effort — can't forward va_list easily */
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
                   current_proc->pid, (int)f->ebx);
            current_proc->state = PROC_ZOMBIE;
            yield();
            break;
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
    printf("VibeCagOS %s — Shell Commands\n", VIBECAGOS_VERSION);
    vga_set_color(0x07);  /* Light gray */
    printf("  help             Show this help\n");
    printf("  hello            Print greeting\n");
    printf("  uname            Show system information\n");
    printf("  uptime           Show system uptime\n");
    printf("  mem              Show memory statistics\n");
    printf("  ps               List processes\n");
    printf("  ls               List filesystem files\n");
    printf("  cat <file>       Display file contents\n");
    printf("  create <file>    Create new empty file\n");
    printf("  write <file>     Write content to file\n");
    printf("  rm <file>        Delete file\n");
    printf("  format           Format filesystem\n");
    printf("  ping <ip>        Ping an IP address\n");
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
    printf("Filesystem  : SimpleFS on IDE disk\n");
    printf("Network     : RTL8139 (polling)\n");
}

static void cmd_uptime(void) {
    uint32_t ms  = get_uptime_ms();
    uint32_t sec = ms / 1000;
    uint32_t min = sec / 60;
    sec %= 60;
    printf("Uptime: %u min %u sec (%u ms, %u ticks)\n", min, sec, ms, ticks);
}

static void cmd_mem(void) {
    uint32_t total_bytes = (uint32_t)__free_ram_end - (uint32_t)__free_ram;
    uint32_t used_bytes  = (next_paddr - (paddr_t)__free_ram);
    uint32_t free_bytes  = total_bytes - used_bytes;

    printf("Physical Memory:\n");
    printf("  Free RAM pool : 0x%08x – 0x%08x\n",
           (uint32_t)__free_ram, (uint32_t)__free_ram_end);
    printf("  Total         : %u KB\n", total_bytes / 1024);
    printf("  Allocated     : %u KB (%u pages)\n", used_bytes / 1024, pages_allocated);
    printf("  Free          : %u KB\n", free_bytes / 1024);
    printf("Kernel image    : 0x%08x\n", (uint32_t)__kernel_base);
    printf("Kernel stack top: 0x%08x\n", (uint32_t)__stack_top);
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

    int ret = simplefs_write(filename, content, (size_t)clen);
    if (ret >= 0)
        printf("Wrote %d bytes to '%s'\n", ret, filename);
    else
        printf("Error: '%s' not found\n", filename);
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
    printf("  |          Version %s  %s               |\n",
           VIBECAGOS_VERSION, VIBECAGOS_BUILD);
    vga_set_color(0x07);  /* Light gray */
    printf("  |          i386 Protected Mode OS                 |\n");
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

    while (1) {
        /* Colored prompt */
        vga_set_color(0x0A);  /* Bright green */
        printf("vcos");
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
        } else if (strcmp(cmdline, "ls") == 0) {
            simplefs_ls();
        } else if (strncmp(cmdline, "cat ", 4) == 0) {
            simplefs_cat(cmdline + 4);
        } else if (strncmp(cmdline, "create ", 7) == 0) {
            int r = simplefs_create(cmdline + 7);
            if (r == 0)
                printf("Created '%s'\n", cmdline + 7);
            else if (r == -1)
                printf("Error: '%s' already exists\n", cmdline + 7);
            else
                printf("Error: No space for new file\n");
        } else if (strncmp(cmdline, "write ", 6) == 0) {
            cmd_write_file(cmdline + 6);
        } else if (strncmp(cmdline, "rm ", 3) == 0) {
            int r = simplefs_delete(cmdline + 3);
            if (r == 0) printf("Deleted '%s'\n", cmdline + 3);
            else        printf("Error: '%s' not found\n", cmdline + 3);
        } else if (strcmp(cmdline, "format") == 0) {
            printf("Type 'yes' to confirm format: ");
            char confirm[8];
            shell_readline(confirm, sizeof(confirm));
            if (strcmp(confirm, "yes") == 0)
                simplefs_format();
            else
                printf("Format cancelled.\n");
        } else if (strncmp(cmdline, "ping ", 5) == 0) {
            cmd_ping(cmdline + 5);
        } else if (strcmp(cmdline, "clear") == 0) {
            vga_clear();
        } else if (strcmp(cmdline, "exit") == 0) {
            vga_set_color(0x0E);
            printf("System halting. Goodbye!\n");
            vga_set_color(0x07);
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

void kernel_main(void) {
    /* Zero BSS */
    memset(__bss, 0, (size_t)((uint32_t)__bss_end - (uint32_t)__bss));

    /* Early I/O */
    vga_init();
    serial_init();

    /* Boot splash */
    print_splash();

    /* --- Phase 1: Architecture Foundation --- */
    printf("Initializing kernel subsystems...\n\n");

    /* Physical memory allocator */
    pmm_init();
    print_ok("Physical memory allocator");

    /* GDT with proper segment descriptors and TSS */
    gdt_init();
    print_ok("GDT (kernel/user segments + TSS)");

    /* IDT — exception + IRQ + syscall handlers */
    idt_init();
    print_ok("IDT (exceptions + IRQs + syscall)");

    /* PIC — remapped, IRQ0+IRQ1 unmasked */
    pic_init();
    print_ok("PIC (IRQ0/timer + IRQ1/keyboard enabled)");

    /* PIT timer at 100 Hz */
    pit_init(100);
    print_ok("PIT timer (100 Hz preemptive)");

    /* Enable interrupts */
    sti();
    print_ok("Interrupts enabled");

    /* Paging — identity map */
    paging_init();

    /* Keyboard */
    keyboard_init();
    print_ok("PS/2 keyboard driver");

    /* --- Phase 2: Devices --- */
    ide_init();
    print_ok("IDE disk controller");

    pci_init();
    print_ok("PCI bus scan");

    nic_init();
    print_ok("RTL8139 NIC");

    /* --- Phase 3: Filesystem --- */
    simplefs_mount();
    if (!fs.mounted) {
        printf("  Formatting new filesystem...\n");
        simplefs_format();
    }
    print_ok("SimpleFS filesystem");

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

    /* Halt */
    printf("\nVibeCagOS halted.\n");
    cli();
    while (1) halt();
}
