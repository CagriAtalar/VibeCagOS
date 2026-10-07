/*
 * VibeCagOS - pipes (the first IPC primitive).
 *
 * A pipe is a kernel-owned byte stream between two Ring-3 processes. It is the
 * building block for shell pipelines (`ls | grep foo`).
 *
 *   writer  --pipe_write()-->  [ struct pipe ]  --pipe_read()-->  reader
 *
 * OWNERSHIP / LIFETIME
 *   A pipe is allocated with kmalloc() by pipe_alloc() and references counted
 *   by its open ends: nreaders and nwriters. It is freed by the last close
 *   (pipe_close_read / pipe_close_write) once both counts reach zero, so a
 *   pipeline survives the exit of whichever process closes first.
 *
 * BLOCKING CONTRACT (both callers are syscalls, IF may be 1 on entry)
 *   pipe_read()  - returns available bytes as soon as any are present
 *                  (a short read, never blocks for a full buffer). Blocks while
 *                  the pipe is empty AND at least one write end is open.
 *                  Returns 0 (EOF) once every write end has been closed.
 *   pipe_write() - writes as much as fits and returns that count (short write).
 *                  Blocks while the pipe is full. Returns -E_PIPE if every
 *                  read end has been closed (nothing could ever read the data).
 *
 *   Blocking uses the process wait channel with the pipe address as the
 *   channel, so wakeup() releases every waiter; the loop re-tests its condition
 *   on every iteration, which makes the wake-all storm safe.
 *
 * NOT PROVIDED YET (deliberate, see docs/ROADMAP.md)
 *   - O_NONBLOCK / F_SETFL: reads and writes always block.
 *   - Message boundaries: a pipe is a byte stream, not a datagram channel.
 */
#pragma once
#include "common.h"
#include "../abi/syscall.h"   /* VIBE_PIPE_SIZE */

/* Buffer capacity in bytes. Shared with user space via the ABI so that a user
 * program can size its writes (see VIBE_PIPE_SIZE in abi/syscall.h). */
#define PIPE_SIZE VIBE_PIPE_SIZE

struct pipe {
    uint8_t  buf[PIPE_SIZE];
    uint32_t head;        /* offset of the next byte to read  */
    uint32_t tail;        /* offset of the next byte to write */
    uint32_t count;       /* bytes currently buffered */
    uint32_t nreaders;    /* number of open read  ends */
    uint32_t nwriters;    /* number of open write ends */
};

/* Create a pipe with one read end and one write end. NULL on OOM. */
struct pipe *pipe_alloc(void);

/* Release one end. Frees the pipe when the last end goes away. */
void pipe_close_read(struct pipe *p);
void pipe_close_write(struct pipe *p);

/* Take an extra reference on one end. Used when a child inherits a pipe fd
 * across spawn(): without this the parent's close would drop the count to zero
 * and free the pipe while the child is still using it. */
void pipe_dup_read(struct pipe *p);
void pipe_dup_write(struct pipe *p);

/* Transfer bytes. Both may block (see the contract above) and are the only
 * places where a process can sleep on a pipe. */
int pipe_read(struct pipe *p, char *dst, uint32_t n);
int pipe_write(struct pipe *p, const char *src, uint32_t n);