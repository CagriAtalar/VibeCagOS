/*
 * VibeCagOS - pipe implementation. See pipe.h for the blocking contract.
 *
 * Everything here runs in syscall context, so interrupts may be enabled on
 * entry. Each critical section is taken with irq_save()/irq_restore(), and the
 * buffer is only ever touched inside one, which is safe because the kernel is
 * single-CPU and its critical sections are not preemptible by the scheduler.
 */
#include "pipe.h"
#include "kernel.h"
#include "kmalloc.h"
#include "../abi/syscall.h"

/* ------------------------------------------------------------------------ */
/* Lifetime                                                                  */
/* ------------------------------------------------------------------------ */

struct pipe *pipe_alloc(void) {
    struct pipe *p = kmalloc(sizeof(*p));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->nreaders = 1;          /* sys_pipe() hands out exactly one of each */
    p->nwriters = 1;
    return p;
}

/* True when no end remains open and the pipe can be freed. Caller holds IF=0. */
static inline bool pipe_dead(const struct pipe *p) {
    return p->nreaders == 0 && p->nwriters == 0;
}

void pipe_close_read(struct pipe *p) {
    if (!p) return;
    uint32_t fl = irq_save();
    if (p->nreaders) p->nreaders--;
    bool dead = pipe_dead(p);
    if (!dead) wakeup(p);     /* a writer blocked on a full buffer must give up */
    irq_restore(fl);
    if (dead) kfree(p);       /* freeing outside the critical section */
}

void pipe_close_write(struct pipe *p) {
    if (!p) return;
    uint32_t fl = irq_save();
    if (p->nwriters) p->nwriters--;
    bool dead = pipe_dead(p);
    if (!dead) wakeup(p);     /* blocked readers must observe EOF */
    irq_restore(fl);
    if (dead) kfree(p);
}

/* Extra reference on one end, taken when a descriptor is duplicated into
 * another process (spawn with inherited fds). */
void pipe_dup_read(struct pipe *p) {
    uint32_t fl = irq_save();
    if (p) p->nreaders++;
    irq_restore(fl);
}

void pipe_dup_write(struct pipe *p) {
    uint32_t fl = irq_save();
    if (p) p->nwriters++;
    irq_restore(fl);
}

/* ------------------------------------------------------------------------ */
/* Transfer                                                                  */
/* ------------------------------------------------------------------------ */

/* Copy `n` bytes out of the ring buffer at `off`, wrapping at PIPE_SIZE.
 * Returns the number of contiguous bytes actually copied, which is only less
 * than `n` when the requested run straddles the end of the buffer. */
static uint32_t pipe_copy_out(struct pipe *p, char *dst, uint32_t off, uint32_t n) {
    uint32_t run = PIPE_SIZE - off;
    if (run > n) run = n;
    memcpy(dst, &p->buf[off], run);
    return run;
}

/* The mirror of pipe_copy_out(): into the ring at `off`. */
static uint32_t pipe_copy_in(struct pipe *p, const char *src, uint32_t off, uint32_t n) {
    uint32_t run = PIPE_SIZE - off;
    if (run > n) run = n;
    memcpy(&p->buf[off], src, run);
    return run;
}

int pipe_read(struct pipe *p, char *dst, uint32_t n) {
    if (!p) return -E_BADF;
    if (n == 0) return 0;

    for (;;) {
        uint32_t fl = irq_save();

        if (p->count > 0) {
            uint32_t take = p->count < n ? p->count : n;
            uint32_t done = 0;
            while (done < take) {
                uint32_t chunk = pipe_copy_out(p, dst + done, p->head, take - done);
                p->head = (p->head + chunk) % PIPE_SIZE;
                done += chunk;
            }
            p->count -= take;
            wakeup(p);                      /* space freed: wake blocked writers */
            irq_restore(fl);
            return (int)take;
        }

        /* Empty: EOF only once every write end is closed. */
        if (p->nwriters == 0) {
            irq_restore(fl);
            return 0;
        }

        block_on(p);                        /* IF is 0 here; yields the CPU */
        irq_restore(fl);
        /* Re-test the loop condition: another process may have changed it. */
    }
}

int pipe_write(struct pipe *p, const char *src, uint32_t n) {
    if (!p) return -E_BADF;
    if (n == 0) return 0;

    for (;;) {
        uint32_t fl = irq_save();

        /* No reader can ever consume this data any more. */
        if (p->nreaders == 0) {
            irq_restore(fl);
            return -E_PIPE;
        }

        uint32_t space = PIPE_SIZE - p->count;
        if (space > 0) {
            uint32_t put = space < n ? space : n;
            uint32_t done = 0;
            while (done < put) {
                uint32_t chunk = pipe_copy_in(p, src + done, p->tail, put - done);
                p->tail = (p->tail + chunk) % PIPE_SIZE;
                done += chunk;
            }
            p->count += put;
            wakeup(p);                      /* data ready: wake blocked readers */
            irq_restore(fl);
            return (int)put;                /* short write if the buffer filled */
        }

        block_on(p);                        /* full: wait for a reader to drain */
        irq_restore(fl);
    }
}