#pragma once
#include "common.h"

/*
 * VibeCagOS — User memory access helpers
 *
 * Syscall arguments that are pointers come from untrusted ring 3 code. The
 * kernel shares the process's page directory, so it *could* dereference them
 * directly — which is exactly the problem: a user could hand over a kernel
 * address and make the kernel read or overwrite its own memory on their
 * behalf. Every user pointer MUST go through these helpers first.
 */

/* True if [ptr, ptr+len) lies fully inside user space and every page is
 * mapped user-accessible (and writable if write is true). len==0 is valid. */
bool user_range_ok(const void *ptr, size_t len, bool write);

/* Copy a NUL-terminated string from user space into dst (capacity max,
 * including the NUL). Returns the string length, or -1 if the pointer is bad
 * or the string does not fit. */
int  user_strncpy(char *dst, const char *usrc, size_t max);
