/*
 * VibeCagOS - processes and the round-robin scheduler.
 *
 * MODEL (xv6-style, one kernel stack per process):
 *
 *   - Every process owns a private kernel stack (struct process::stack).
 *     TSS.esp0 is pointed at its top whenever the process is current, so a
 *     Ring3 -> Ring0 transition (int 0x80, IRQ, exception) lands there and
 *     the complete user context is saved on that stack as a trap_frame.
 *
 *   - A context switch happens INSIDE schedule(), i.e. on the old process's
 *     kernel stack, below its trap frame:
 *
 *        old kstack (high)                      new kstack (high)
 *        [ trap_frame of old user ctx ]         [ trap_frame of new user ctx ]
 *        [ isr_common -> handle_interrupt ]     [ isr_common -> handle_interrupt ]
 *        [ process_tick/syscall -> schedule ]   [ ... schedule ]
 *        [ switch_context  <-- ESP saved ]      [ switch_context --> ESP loaded ]
 *
 *     switch_context() only swaps callee-saved registers + ESP. When the new
 *     process is resumed it returns up its OWN call chain, through
 *     isr_common/trapret, and `iret`s to Ring 3 exactly where it was
 *     interrupted. Nothing is left half-finished on the old stack: its frame
 *     stays valid until it is scheduled again.
 *
 *   - A brand-new user process has a hand-built trap frame on its kernel
 *     stack and a fake switch_context frame whose return address is
 *     `trapret`; its first "resume" therefore iret's into Ring 3.
 *
 * PREEMPTION RULE: kernel mode is non-preemptible. The timer preempts only
 * when it interrupted Ring 3 (or the idle thread). Kernel threads and
 * syscalls give up the CPU only at explicit points (yield/block_on/sleep).
 * All of schedule() runs with IF=0.
 */
#include "kernel.h"
#include "vmm.h"
#include "pmm.h"
#include "klog.h"
#include "../abi/syscall.h"
#include "../fs/vfs.h"

struct process procs[PROCS_MAX];
struct process *current_proc = NULL;
struct process *idle_proc    = NULL;

static int next_pid = 1;

static inline void stack_canary_set(struct process *p)   { *(volatile uint32_t *)p->stack = STACK_CANARY; }
static inline void stack_canary_check(struct process *p) {
    if (*(volatile uint32_t *)p->stack != STACK_CANARY)
        PANIC("kernel stack overflow in pid %d (%s)", p->pid, p->name);
}

static inline uint32_t kstack_top(struct process *p) {
    return (uint32_t)&p->stack[KERNEL_STACK];
}

/* ------------------------------------------------------------------------ */
/* Scheduler                                                                */
/* ------------------------------------------------------------------------ */

/* Pick the next RUNNABLE process after the current one (round robin).
 * Caller has IF=0. Returns when this process is resumed. */
static void schedule_locked(void) {
    struct process *prev = current_proc;
    struct process *next = NULL;
    stack_canary_check(prev);

    if (prev->state == PROC_RUNNING)
        prev->state = PROC_RUNNABLE;

    int start = (int)(prev - procs);
    for (int i = 1; i <= PROCS_MAX; i++) {
        struct process *p = &procs[(start + i) % PROCS_MAX];
        if (p != idle_proc && p->state == PROC_RUNNABLE) { next = p; break; }
    }
    if (!next) next = idle_proc;

    next->state      = PROC_RUNNING;
    next->time_slice = TIME_SLICE_TICKS;
    if (next == prev) return;

    current_proc = next;
    uint32_t *pd = next->page_table ? next->page_table : vmm_get_kernel_dir();
    if ((uint32_t)pd != read_cr3()) load_cr3((uint32_t)pd);
    tss_set_kernel_stack(kstack_top(next));
    switch_context(&prev->sp, next->sp);
    /* resumed here later; current_proc was set by whoever switched to us */
}

void schedule(void) {
    uint32_t fl = irq_save();
    schedule_locked();
    irq_restore(fl);
}

void yield(void) { schedule(); }

/* Sleep on a wait channel. Caller has IF=0 (use irq_save). */
void block_on(void *chan) {
    current_proc->wait_chan = chan;
    current_proc->state     = PROC_BLOCKED;
    schedule_locked();
}

void wakeup(void *chan) {
    uint32_t fl = irq_save();
    for (int i = 0; i < PROCS_MAX; i++) {
        struct process *p = &procs[i];
        if (p->state == PROC_BLOCKED && p->wait_chan == chan) {
            p->wait_chan = NULL;
            p->state     = PROC_RUNNABLE;
        }
    }
    irq_restore(fl);
}

void sleep_ms(uint32_t ms) {
    if (!current_proc || current_proc == idle_proc) return;
    uint32_t t = (ms + 9) / 10;
    if (t == 0) t = 1;
    uint32_t fl = irq_save();
    current_proc->sleep_until = get_ticks() + t;
    current_proc->state       = PROC_SLEEPING;
    schedule_locked();
    irq_restore(fl);
}

/* Called from the PIT handler (IF=0, EOI already sent). */
void process_tick(struct trap_frame *f) {
    if (!current_proc) return;
    uint32_t now = get_ticks();
    bool any_runnable = false;

    for (int i = 0; i < PROCS_MAX; i++) {
        struct process *p = &procs[i];
        if (p->state == PROC_SLEEPING && (int32_t)(now - p->sleep_until) >= 0)
            p->state = PROC_RUNNABLE;
        if (p != idle_proc && p->state == PROC_RUNNABLE)
            any_runnable = true;
    }

    if (current_proc == idle_proc) {
        if (any_runnable) schedule_locked();
        return;
    }
    if (TF_FROM_USER(f)) {
        if (--current_proc->time_slice <= 0)
            schedule_locked();
    }
}

/* ------------------------------------------------------------------------ */
/* Creation                                                                 */
/* ------------------------------------------------------------------------ */

static struct process *alloc_slot(void) {
    for (int i = 0; i < PROCS_MAX; i++)
        if (procs[i].state == PROC_UNUSED) return &procs[i];
    return NULL;
}

static void kthread_start(void) {
    struct process *p = current_proc;
    sti();                               /* arrived with IF=0 from schedule() */
    p->kentry();
    process_exit(0);
}

static uint32_t *push_switch_frame(uint32_t *sp, uint32_t ret_addr) {
    *--sp = ret_addr;       /* popped by `ret` in switch_context */
    *--sp = 0;              /* ebp */
    *--sp = 0;              /* ebx */
    *--sp = 0;              /* esi */
    *--sp = 0;              /* edi  <- saved p->sp points here */
    return sp;
}

static void idle_task(void) {
    for (;;) {
        __asm__ __volatile__("sti; hlt" ::: "memory");
        /* detached zombies (background jobs nobody waits for) */
        uint32_t fl = irq_save();
        for (int i = 1; i < PROCS_MAX; i++) {
            struct process *p = &procs[i];
            if (p->state == PROC_ZOMBIE && p->ppid == -1) {
                vmm_destroy_address_space(p->page_table);
                memset(p, 0, sizeof(*p));      /* state = UNUSED */
            }
        }
        irq_restore(fl);
    }
}

struct process *process_create_kthread(const char *name, void (*entry)(void)) {
    uint32_t fl = irq_save();
    struct process *p = alloc_slot();
    if (!p) { irq_restore(fl); return NULL; }
    memset(p, 0, sizeof(*p));
    p->pid   = (p == &procs[0]) ? 0 : next_pid++;
    p->kentry = entry;
    strncpy(p->name, name, sizeof(p->name) - 1);

    stack_canary_set(p);
    uint32_t *sp = (uint32_t *)kstack_top(p);
    *--sp = 0;                                   /* fake return for kthread_start */
    sp = push_switch_frame(sp, (uint32_t)kthread_start);
    p->sp    = (uint32_t)sp;
    p->state = PROC_RUNNABLE;
    irq_restore(fl);
    return p;
}

/*
 * Load a flat "VBIN" user image (see abi/syscall.h) into a new private
 * address space and make the process runnable.
 *
 *   text pages  : PRESENT|USER            (read-only, no write)
 *   data/bss    : PRESENT|USER|WRITABLE
 *   stack       : PRESENT|USER|WRITABLE, USER_STACK_PAGES pages below
 *                 USER_STACK_TOP; the page below is left unmapped (guard)
 *   `arg` is delivered in EAX at entry (crt0 passes it to user_main).
 */
/* Write bytes into the (already mapped, writable) user stack of address
 * space `pd` through the identity map. Returns false if unmapped. */
static bool ustack_put(uint32_t *pd, uint32_t va, const void *src, size_t n) {
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        paddr_t pa;
        if (!vmm_user_lookup(pd, va, VMM_FLAG_USER | VMM_FLAG_WRITABLE, &pa)) return false;
        size_t chunk = PAGE_SIZE - (va & 0xFFFu);
        if (chunk > n) chunk = n;
        memcpy((void *)pa, s, chunk);
        s += chunk; va += (uint32_t)chunk; n -= chunk;
    }
    return true;
}

/*
 * Initial user stack (esp points at argc), System-V style:
 *
 *   high  | "arg0\0arg1\0..."   |
 *         | argv[argc] = NULL   |
 *         | argv[argc-1] ...    |
 *         | argv[0]             |  <- argv
 *   esp-> | argc                |
 *   low
 * crt0 hands (argc, argv) to user_main().
 */
static bool build_user_stack(struct process *p, int argc, const char *const *argv) {
    uint32_t va = USER_STACK_TOP - 16;
    uint32_t ptrs[SPAWN_ARGS_MAX + 1];
    if (argc < 0 || argc > SPAWN_ARGS_MAX) return false;

    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        if (len > SPAWN_ARG_LEN) return false;
        va -= (uint32_t)len;
        if (!ustack_put(p->page_table, va, argv[i], len)) return false;
        ptrs[i] = va;
    }
    ptrs[argc] = 0;
    va &= ~15u;
    va -= 4u * (uint32_t)(argc + 1);
    if (!ustack_put(p->page_table, va, ptrs, 4u * (uint32_t)(argc + 1))) return false;
    va -= 4;
    uint32_t n = (uint32_t)argc;
    if (!ustack_put(p->page_table, va, &n, 4)) return false;
    p->user_esp = va;
    return true;
}

struct process *process_create_user(const char *name, const void *image,
                                    size_t image_size, int argc, const char *const *argv) {
    const struct vbin_header *h = (const struct vbin_header *)image;
    if (image_size < sizeof(*h) || h->magic != VBIN_MAGIC) return NULL;
    if (h->file_size > image_size || h->file_size > h->mem_size) return NULL;
    if (h->mem_size == 0 || h->mem_size > (1u << 20)) return NULL;
    if ((h->text_size & 0xFFF) || h->text_size > h->mem_size) return NULL;
    if (h->entry < USER_BASE || h->entry >= USER_BASE + h->mem_size) return NULL;

    uint32_t fl = irq_save();
    struct process *p = alloc_slot();
    if (!p) { irq_restore(fl); return NULL; }
    memset(p, 0, sizeof(*p));
    p->state = PROC_CREATED;                /* reserve slot */
    p->pid   = next_pid++;
    irq_restore(fl);

    uint32_t *pd = vmm_create_address_space();
    if (!pd) { p->state = PROC_UNUSED; return NULL; }

    /* image */
    for (uint32_t off = 0; off < h->mem_size; off += PAGE_SIZE) {
        paddr_t frame = pmm_alloc_frame();          /* zero-filled */
        if (!frame) goto fail;
        if (off < h->file_size) {
            uint32_t n = h->file_size - off;
            if (n > PAGE_SIZE) n = PAGE_SIZE;
            memcpy((void *)frame, (const uint8_t *)image + off, n);
        }
        uint32_t fl2 = VMM_FLAG_USER | ((off < h->text_size) ? 0 : VMM_FLAG_WRITABLE);
        vmm_map_page(pd, USER_BASE + off, frame, fl2);
    }
    /* stack */
    for (int i = 1; i <= USER_STACK_PAGES; i++) {
        paddr_t frame = pmm_alloc_frame();
        if (!frame) goto fail;
        vmm_map_page(pd, USER_STACK_TOP - (uint32_t)i * PAGE_SIZE, frame,
                     VMM_FLAG_USER | VMM_FLAG_WRITABLE);
    }

    p->page_table = pd;
    p->user_entry = h->entry;
    p->ppid       = 0;
    if (!build_user_stack(p, argc, argv)) { p->page_table = NULL; goto fail; }
    strcpy(p->cwd, "/");
    strncpy(p->name, name, sizeof(p->name) - 1);
    fd_init_std(p);

    /* Initial kernel stack:
     *   [ trap_frame (ring-3 iret frame) ][ ret=trapret + callee-saved ]
     */
    stack_canary_set(p);
    uint32_t top = kstack_top(p);
    struct trap_frame *tf = (struct trap_frame *)(top - sizeof(*tf));
    memset(tf, 0, sizeof(*tf));
    tf->gs = tf->fs = tf->es = tf->ds = SEL_USER_DATA;
    tf->eax      = (uint32_t)argc;
    tf->eip      = p->user_entry;
    tf->cs       = SEL_USER_CODE;
    tf->eflags   = 0x202;                 /* IF=1, reserved bit 1 */
    tf->user_esp = p->user_esp;
    tf->user_ss  = SEL_USER_DATA;

    p->sp = (uint32_t)push_switch_frame((uint32_t *)tf, (uint32_t)trapret);

    fl = irq_save();
    p->state = PROC_RUNNABLE;
    irq_restore(fl);
    KINFO("PROC", "created pid %d '%s' entry=0x%x", p->pid, p->name, p->user_entry);
    return p;

fail:
    vmm_destroy_address_space(pd);
    p->state = PROC_UNUSED;
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Exit / wait / kill                                                       */
/* ------------------------------------------------------------------------ */

static struct process *find_pid(int pid) {
    for (int i = 0; i < PROCS_MAX; i++)
        if (procs[i].state != PROC_UNUSED && procs[i].pid == pid) return &procs[i];
    return NULL;
}

/* Children of a dying process become detached (ppid -1): the idle thread
 * reaps them when they exit. IF must be 0. */
static void orphan_children(struct process *p) {
    for (int i = 0; i < PROCS_MAX; i++)
        if (procs[i].state != PROC_UNUSED && procs[i].ppid == p->pid && p != &procs[i])
            procs[i].ppid = -1;
}

/* Common tail of exit/kill. IF must be 0. */
static void make_zombie(struct process *p, int code) {
    fd_close_all(p);
    orphan_children(p);
    p->exit_code = code;
    p->state     = PROC_ZOMBIE;
    wakeup(p);                                   /* kernel process_wait() */
    if (p->ppid > 0) {
        struct process *par = find_pid(p->ppid);
        if (par) wakeup(par);                    /* user waitpid() */
    }
}

void process_exit(int code) {
    cli();
    struct process *p = current_proc;
    make_zombie(p, code);
    schedule_locked();                  /* never returns: zombies don't run */
    PANIC("zombie pid %d was scheduled", p->pid);
    for (;;) halt();
}

/* Kernel-side waitpid: block until `p` exits, free it, return exit code. */
int process_wait(struct process *p) {
    uint32_t fl = irq_save();
    while (p->state != PROC_ZOMBIE)
        block_on(p);
    int code = p->exit_code;
    /* CR3 can't be p's: zombies never run again. */
    vmm_destroy_address_space(p->page_table);
    memset(p, 0, sizeof(*p));
    irq_restore(fl);
    return code;
}

/* waitpid(): reap a zombie child of the CURRENT process. pid == -1: any. */
int process_waitpid(int pid, int *status) {
    struct process *me = current_proc;
    uint32_t fl = irq_save();
    for (;;) {
        bool have_child = false;
        for (int i = 0; i < PROCS_MAX; i++) {
            struct process *c = &procs[i];
            if (c->state == PROC_UNUSED || c->ppid != me->pid || c == me) continue;
            if (pid != -1 && c->pid != pid) continue;
            have_child = true;
            if (c->state == PROC_ZOMBIE) {
                int cpid = c->pid;
                if (status) *status = c->exit_code;
                vmm_destroy_address_space(c->page_table);
                memset(c, 0, sizeof(*c));
                irq_restore(fl);
                return cpid;
            }
        }
        if (!have_child) { irq_restore(fl); return -E_CHILD; }
        block_on(me);                    /* woken by make_zombie(child) */
    }
}

int process_kill(int pid) {
    if (pid <= 1) return -1;                 /* idle + kernel init/shell */
    uint32_t fl = irq_save();
    for (int i = 2; i < PROCS_MAX; i++) {
        struct process *p = &procs[i];
        if (p->pid == pid && p->state != PROC_UNUSED && p != current_proc) {
            if (p->state == PROC_ZOMBIE) { irq_restore(fl); return -2; }
            make_zombie(p, -9);
            irq_restore(fl);
            return 0;
        }
    }
    irq_restore(fl);
    return -3;
}

/* ------------------------------------------------------------------------ */
/* Bring-up                                                                 */
/* ------------------------------------------------------------------------ */

void process_init(void) {
    memset(procs, 0, sizeof(procs));
    idle_proc = process_create_kthread("idle", idle_task);   /* slot 0, pid 0 */
}

/* Hand the CPU to the scheduler. Never returns: the boot stack is abandoned. */
void process_start(void) {
    cli();
    uint32_t boot_sp;
    current_proc = idle_proc;
    idle_proc->state = PROC_RUNNING;
    load_cr3((uint32_t)vmm_get_kernel_dir());
    tss_set_kernel_stack(kstack_top(idle_proc));
    switch_context(&boot_sp, idle_proc->sp);
    for (;;) halt();
}
