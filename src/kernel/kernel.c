#include "kernel.h"
#include "common.h"
#include "simplefs.h"
#include "vga.h"
#include "ide.h"
#include "keyboard.h"

extern char __kernel_base[];
extern char __stack_top[];
extern char __bss[], __bss_end[];
extern char __free_ram[], __free_ram_end[];

struct process procs[PROCS_MAX];
struct process *current_proc;
struct process *idle_proc;

// Memory allocation
paddr_t alloc_pages(uint32_t n) {
    static paddr_t next_paddr = (paddr_t) __free_ram;
    paddr_t paddr = next_paddr;
    next_paddr += n * PAGE_SIZE;

    if (next_paddr > (paddr_t) __free_ram_end)
        PANIC("out of memory");

    memset((void *) paddr, 0, n * PAGE_SIZE);
    return paddr;
}

// x86 paging - two-level page tables
void map_page(uint32_t *page_dir, uint32_t vaddr, paddr_t paddr, uint32_t flags) {
    if (!is_aligned(vaddr, PAGE_SIZE))
        PANIC("unaligned vaddr %x", vaddr);

    if (!is_aligned(paddr, PAGE_SIZE))
        PANIC("unaligned paddr %x", paddr);

    uint32_t pde_index = vaddr >> 22;
    uint32_t pte_index = (vaddr >> 12) & 0x3ff;

    // Check if page table exists
    if ((page_dir[pde_index] & PAGE_PRESENT) == 0) {
        uint32_t pt_paddr = alloc_pages(1);
        page_dir[pde_index] = pt_paddr | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }

    uint32_t *page_table = (uint32_t *) (page_dir[pde_index] & ~0xfff);
    page_table[pte_index] = paddr | flags | PAGE_PRESENT;
}

// Serial port I/O for console
#define PORT_COM1 0x3f8

void serial_init(void) {
    outb(PORT_COM1 + 1, 0x00);
    outb(PORT_COM1 + 3, 0x80);
    outb(PORT_COM1 + 0, 0x03);
    outb(PORT_COM1 + 1, 0x00);
    outb(PORT_COM1 + 3, 0x03);
    outb(PORT_COM1 + 2, 0xC7);
    outb(PORT_COM1 + 4, 0x0B);
}

void putchar(char ch) {
    // Send to both serial (for QEMU) and VGA (for VirtualBox)
    while ((inb(PORT_COM1 + 5) & 0x20) == 0);
    outb(PORT_COM1, ch);
    vga_putchar(ch);  // Also display on screen
}

long getchar(void) {
    // Try keyboard first (for VirtualBox)
    long ch = kb_getchar();
    if (ch >= 0) {
        return ch;
    }
    
    // Fall back to serial (for QEMU)
    if ((inb(PORT_COM1 + 5) & 1) == 0)
        return -1;
    return inb(PORT_COM1);
}

// Disk I/O wrapper for SimpleFS
void read_write_disk(void *buf, unsigned sector, int is_write) {
    if (is_write) {
        ide_write_sector(sector, buf);
    } else {
        ide_read_sector(sector, buf);
    }
}

// IDT setup
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t offset_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct idt_entry idt[256];
struct idt_ptr idtp;

void idt_set_gate(uint8_t num, uint32_t handler, uint16_t sel, uint8_t flags) {
    idt[num].offset_low = handler & 0xFFFF;
    idt[num].selector = sel;
    idt[num].zero = 0;
    idt[num].type_attr = flags;
    idt[num].offset_high = (handler >> 16) & 0xFFFF;
}

// Interrupt handlers (assembly stubs will call these)
extern void isr0(void);
extern void isr33(void);  // Keyboard interrupt (IRQ1)
extern void isr128(void); // Syscall interrupt

// PIC (Programmable Interrupt Controller) initialization
void pic_init(void) {
    // Initialize PIC - remap IRQs to 0x20-0x2F
    outb(0x20, 0x11);  // Start init sequence (ICW1)
    outb(0xA0, 0x11);
    outb(0x21, 0x20);  // IRQ base for master PIC (ICW2)
    outb(0xA1, 0x28);  // IRQ base for slave PIC
    outb(0x21, 0x04);  // Tell master about slave (ICW3)
    outb(0xA1, 0x02);
    outb(0x21, 0x01);  // 8086 mode (ICW4)
    outb(0xA1, 0x01);
    
    // Unmask only keyboard interrupt (IRQ1)
    outb(0x21, 0xFD);  // 11111101 - only IRQ1 enabled
    outb(0xA1, 0xFF);  // Disable all slave PIC interrupts
}

void idt_init(void) {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (uint32_t)&idt;

    memset(&idt, 0, sizeof(struct idt_entry) * 256);

    // Set up keyboard interrupt (IRQ1 = INT 0x21)
    idt_set_gate(33, (uint32_t)isr33, 0x08, 0x8E); // 0x8E = kernel interrupt gate
    
    // Set up syscall gate (int 0x80)
    idt_set_gate(128, (uint32_t)isr128, 0x08, 0xEE); // 0xEE = user-level interrupt gate

    load_idt(&idtp);
    pic_init();  // Initialize PIC after setting up IDT
}

// Process management
__attribute__((naked)) void user_entry(void) {
    __asm__ __volatile__(
        "mov $0x23, %%ax\n"      // User data segment
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%esp, %%eax\n"
        "pushl $0x23\n"          // SS
        "pushl %%eax\n"          // ESP
        "pushf\n"                // EFLAGS
        "popl %%eax\n"
        "orl $0x200, %%eax\n"    // Enable interrupts
        "pushl %%eax\n"
        "pushl $0x1B\n"          // CS (user code segment)
        "pushl %0\n"             // EIP
        "iret\n"
        : : "r"(USER_BASE)
    );
}

__attribute__((naked)) void switch_context(uint32_t *prev_sp, uint32_t *next_sp) {
    __asm__ __volatile__(
        "pushl %%ebp\n"
        "pushl %%ebx\n"
        "pushl %%esi\n"
        "pushl %%edi\n"
        "movl %%esp, (%%eax)\n"
        "movl (%%edx), %%esp\n"
        "popl %%edi\n"
        "popl %%esi\n"
        "popl %%ebx\n"
        "popl %%ebp\n"
        "ret\n"
        : : : "memory"
    );
}

struct process *create_process(const void *image, size_t image_size) {
    struct process *proc = NULL;
    int i;
    for (i = 0; i < PROCS_MAX; i++) {
        if (procs[i].state == PROC_UNUSED) {
            proc = &procs[i];
            break;
        }
    }

    if (!proc)
        PANIC("no free process slots");

    uint32_t *sp = (uint32_t *) &proc->stack[sizeof(proc->stack)];
    *--sp = 0;  // edi
    *--sp = 0;  // esi
    *--sp = 0;  // ebx
    *--sp = 0;  // ebp
    *--sp = (uint32_t) user_entry;  // return address

    // Create page directory
    uint32_t *page_dir = (uint32_t *) alloc_pages(1);

    // Map kernel pages
    for (paddr_t paddr = (paddr_t) __kernel_base;
         paddr < (paddr_t) __free_ram_end; paddr += PAGE_SIZE)
        map_page(page_dir, paddr, paddr, PAGE_WRITE);

    // Map user pages
    for (uint32_t off = 0; off < image_size; off += PAGE_SIZE) {
        paddr_t page = alloc_pages(1);
        size_t remaining = image_size - off;
        size_t copy_size = PAGE_SIZE <= remaining ? PAGE_SIZE : remaining;
        memcpy((void *) page, image + off, copy_size);
        map_page(page_dir, USER_BASE + off, page, PAGE_USER | PAGE_WRITE);
    }

    proc->pid = i + 1;
    proc->state = PROC_RUNNABLE;
    proc->sp = (uint32_t) sp;
    proc->page_table = page_dir;
    return proc;
}

void yield(void) {
    struct process *next = idle_proc;
    for (int i = 0; i < PROCS_MAX; i++) {
        struct process *proc = &procs[(current_proc->pid + i) % PROCS_MAX];
        if (proc->state == PROC_RUNNABLE && proc->pid > 0) {
            next = proc;
            break;
        }
    }

    if (next == current_proc)
        return;

    struct process *prev = current_proc;
    current_proc = next;

    load_cr3((uint32_t)next->page_table);
    switch_context(&prev->sp, &next->sp);
}

void handle_syscall(struct trap_frame *f) {
    switch (f->eax) {
        case SYS_PUTCHAR:
            putchar(f->ebx);
            break;
        case SYS_GETCHAR:
            while (1) {
                long ch = getchar();
                if (ch >= 0) {
                    f->ebx = ch;
                    break;
                }
                yield();
            }
            break;
        case SYS_EXIT:
            printf("process %d exited\n", current_proc->pid);
            current_proc->state = PROC_EXITED;
            yield();
            PANIC("unreachable");
        default:
            PANIC("unexpected syscall eax=%x\n", f->eax);
    }
}

void handle_interrupt(struct trap_frame *f) {
    if (f->int_no == 128) {  // Syscall
        handle_syscall(f);
    } else {
        PANIC("unexpected interrupt: int_no=%d, err=%d, eip=%x\n", 
              f->int_no, f->err_code, f->eip);
    }
}

void kernel_main(void) {
    memset(__bss, 0, (size_t) __bss_end - (size_t) __bss);
    
    vga_init();       // Initialize VGA for VirtualBox
    serial_init();    // Initialize serial for QEMU
    
    printf("\n\nx86 OS - SimpleFS (IDE Disk)\n");
    printf("=====================================\n");
    printf("Output: VGA + Serial Console\n");
    
    // Initialize interrupts and keyboard
    idt_init();
    keyboard_init();
    __asm__ __volatile__("sti");  // Enable interrupts
    
    printf("Keyboard: Enabled (IRQ1)\n");
    
    // Initialize IDE disk
    ide_init();
    printf("\n");
    
    // Try to mount filesystem, if fails, format it
    simplefs_mount();
    if (!fs.mounted) {
        printf("Formatting new filesystem...\n");
        simplefs_format();
    }
    printf("\n");
    
    // Simple shell without processes
    while (1) {
        printf("> ");
        char cmdline[128];
        int i = 0;
        
        // Read command
        while (i < (int)sizeof(cmdline) - 1) {
            long ch = getchar();
            if (ch >= 0) {
                if (ch == '\r' || ch == '\n') {
                    putchar('\n');
                    cmdline[i] = '\0';
                    break;
                }
                putchar(ch);
                cmdline[i++] = ch;
            }
        }
        cmdline[i] = '\0';
        
        // Parse and execute commands
        if (strcmp(cmdline, "hello") == 0) {
            printf("Hello from x86 OS with SimpleFS!\n");
        }
        else if (strcmp(cmdline, "ls") == 0) {
            simplefs_ls();
        }
        else if (strncmp(cmdline, "cat ", 4) == 0) {
            char *filename = cmdline + 4;
            simplefs_cat(filename);
        }
        else if (strncmp(cmdline, "create ", 7) == 0) {
            char *filename = cmdline + 7;
            int ret = simplefs_create(filename);
            if (ret == 0) {
                printf("File '%s' created successfully\n", filename);
            } else if (ret == -1) {
                printf("Error: File '%s' already exists\n", filename);
            } else {
                printf("Error: No space for new files\n");
            }
        }
        else if (strncmp(cmdline, "write ", 6) == 0) {
            char *filename = cmdline + 6;
            printf("Enter content (end with empty line):\n");
            char content[2048];
            int content_len = 0;
            
            while (content_len < 2000) {
                printf("| ");
                char line[128];
                int line_len = 0;
                
                while (line_len < 127) {
                    long ch = getchar();
                    if (ch >= 0) {
                        if (ch == '\r' || ch == '\n') {
                            putchar('\n');
                            line[line_len] = '\0';
                            break;
                        }
                        putchar(ch);
                        line[line_len++] = ch;
                    }
                }
                line[line_len] = '\0';
                
                // Empty line ends input
                if (line_len == 0) {
                    break;
                }
                
                // Add line to content
                for (int i = 0; i < line_len && content_len < 2000; i++) {
                    content[content_len++] = line[i];
                }
                if (content_len < 2000) {
                    content[content_len++] = '\n';
                }
            }
            
            int ret = simplefs_write(filename, content, content_len);
            if (ret >= 0) {
                printf("Wrote %d bytes to '%s'\n", ret, filename);
            } else {
                printf("Error: File '%s' not found\n", filename);
            }
        }
        else if (strncmp(cmdline, "rm ", 3) == 0) {
            char *filename = cmdline + 3;
            int ret = simplefs_delete(filename);
            if (ret == 0) {
                printf("File '%s' deleted\n", filename);
            } else {
                printf("Error: File '%s' not found\n", filename);
            }
        }
        else if (strcmp(cmdline, "format") == 0) {
            printf("Are you sure? This will erase all data! Type 'yes' to confirm: ");
            char confirm[10];
            int j = 0;
            while (j < 9) {
                long ch = getchar();
                if (ch >= 0) {
                    if (ch == '\r' || ch == '\n') {
                        putchar('\n');
                        confirm[j] = '\0';
                        break;
                    }
                    putchar(ch);
                    confirm[j++] = ch;
                }
            }
            confirm[j] = '\0';
            if (strcmp(confirm, "yes") == 0) {
                simplefs_format();
            } else {
                printf("Format cancelled.\n");
            }
        }
        else if (strcmp(cmdline, "help") == 0) {
            printf("Available commands:\n");
            printf("  hello           - Print greeting\n");
            printf("  ls              - List all files\n");
            printf("  cat <file>      - Display file contents\n");
            printf("  create <file>   - Create new file\n");
            printf("  write <file>    - Write content to file\n");
            printf("  rm <file>       - Delete file\n");
            printf("  format          - Format filesystem\n");
            printf("  help            - Show this help\n");
            printf("  exit            - Exit shell\n");
        }
        else if (strcmp(cmdline, "exit") == 0) {
            printf("Goodbye!\n");
            break;
        }
        else if (cmdline[0] != '\0') {
            printf("Unknown command: %s\n", cmdline);
            printf("Type 'help' for available commands\n");
        }
    }
    
    printf("\nKernel shutting down...\n");
    while (1) { __asm__ __volatile__("hlt"); }
}

__attribute__((section(".text.boot")))
__attribute__((naked))
void boot(void) {
    __asm__ __volatile__(
        "mov %[stack_top], %%esp\n"
        "call kernel_main\n"
        "1: hlt\n"
        "jmp 1b\n"
        :
        : [stack_top] "r" (__stack_top)
    );
}

