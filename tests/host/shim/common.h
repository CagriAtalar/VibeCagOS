/*
 * Host-test shim for the kernel's "common.h". The REAL kmalloc.c / kmalloc.h
 * are copied into build/hosttest/ by `make host-test` and compiled against this
 * header (and kernel.h / pmm.h next to it) instead of the freestanding kernel
 * headers. No allocator logic lives in tests/.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

typedef uintptr_t paddr_t;

#define PAGE_SIZE 4096u

/* Test harness hook: PANIC must not kill the process so invalid-free
 * detection can be asserted. Implemented in test_heap.c (longjmp). */
void host_panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
#define PANIC(...) host_panic(__VA_ARGS__)
