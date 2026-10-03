/*
 * VibeCagOS — Kernel Ring-Buffer Logger
 *
 * Provides structured logging with levels and subsystem tags.
 * The ring buffer is readable via /proc/dmesg and the `dmesg` shell command.
 *
 * Levels:
 *   KLOG_DEBUG  — verbose debugging info
 *   KLOG_INFO   — normal operational messages
 *   KLOG_WARN   — unexpected but non-fatal conditions
 *   KLOG_ERROR  — serious errors, subsystem may be degraded
 *   KLOG_PANIC  — called just before kernel_panic
 *
 * Format (each entry):
 *   [uptime_ms] [LEVEL] [subsystem] message\n
 */

#pragma once
#include "common.h"

/* =========================================================================
 * Log levels
 * ========================================================================= */

#define KLOG_DEBUG  0
#define KLOG_INFO   1
#define KLOG_WARN   2
#define KLOG_ERROR  3
#define KLOG_PANIC  4

/* Minimum level to store in ring buffer */
#ifndef KLOG_LEVEL
#define KLOG_LEVEL KLOG_DEBUG
#endif

/* =========================================================================
 * Ring buffer capacity
 * ========================================================================= */

#define KLOG_BUF_SIZE   (16 * 1024)   /* 16 KB ring buffer */
#define KLOG_LINE_MAX   256            /* Max single message length */

/* =========================================================================
 * Public API
 * ========================================================================= */

void     klog_init(void);
void     klog(int level, const char *subsystem, const char *fmt, ...);
void     klog_dump(void);
uint32_t klog_read(char *buf, uint32_t len);
uint32_t klog_total_bytes(void);

/* =========================================================================
 * Convenience macros
 * ========================================================================= */

#define KDEBUG(sub, fmt, ...) klog(KLOG_DEBUG, sub, fmt, ##__VA_ARGS__)
#define KINFO(sub, fmt, ...)  klog(KLOG_INFO,  sub, fmt, ##__VA_ARGS__)
#define KWARN(sub, fmt, ...)  klog(KLOG_WARN,  sub, fmt, ##__VA_ARGS__)
#define KERROR(sub, fmt, ...) klog(KLOG_ERROR, sub, fmt, ##__VA_ARGS__)
