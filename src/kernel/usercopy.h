/*
 * VibeCagOS - user <-> kernel memory transfer.
 *
 * Every pointer that arrives from Ring 3 is UNTRUSTED. Kernel code must
 * never dereference it; it goes through these functions, which walk the
 * current process's page tables and require, for EVERY page touched:
 *   - the address is inside [USER_BASE, USER_END) and does not wrap
 *   - PDE and PTE are present and have the USER bit
 *   - for writes: the PTE is writable
 * All functions return 0 on success or -E_FAULT. Nothing is copied unless
 * the whole range validated (no partial copies).
 */
#pragma once
#include "common.h"

bool user_range_valid(const void *user_ptr, size_t len, bool write);
bool user_ptr_valid(const void *user_ptr, bool write);
int  copy_from_user(void *kernel_dst, const void *user_src, size_t len);
int  copy_to_user(void *user_dst, const void *kernel_src, size_t len);
/* Copies a NUL-terminated string (max bytes incl. NUL). Returns length
 * (without NUL), -E_FAULT, or -E_INVAL if no NUL within max. */
int  strncpy_from_user(char *kernel_dst, const char *user_src, size_t max);
