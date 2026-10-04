/*
 * VibeCagOS — Kernel Ring-Buffer Logger Implementation
 *
 * The ring buffer is a simple linear byte array used as a circular queue.
 * Messages are formatted and appended; old messages are overwritten when full.
 *
 * Thread safety: single CPU, no SMP.  Interrupts are disabled around writes
 * to keep the buffer consistent from IRQ context.
 */

#include "klog.h"
#include "common.h"
#include "kernel.h"   /* cli/sti, get_uptime_ms, putchar */

/* =========================================================================
 * Internal state
 * ========================================================================= */

static char     klog_buf[KLOG_BUF_SIZE];
static uint32_t klog_head = 0;   /* write position */
static uint32_t klog_size = 0;   /* bytes currently stored */
static bool     klog_ready = false;

/* Count of each level for stats */
static uint32_t klog_counts[5];

/* =========================================================================
 * Level names
 * ========================================================================= */

static const char *level_names[] = {
    "DEBUG", "INFO ", "WARN ", "ERROR", "PANIC"
};

/* =========================================================================
 * Internal: write one character into ring buffer (no locking)
 * ========================================================================= */

static void ring_putc(char c) {
    klog_buf[klog_head % KLOG_BUF_SIZE] = c;
    klog_head++;
    if (klog_size < KLOG_BUF_SIZE)
        klog_size++;
}

/* =========================================================================
 * Internal: write a string into ring buffer
 * ========================================================================= */

static void ring_puts(const char *s) {
    while (*s)
        ring_putc(*s++);
}

/* =========================================================================
 * Internal: write decimal uint32 into ring buffer
 * ========================================================================= */

static void ring_putu(uint32_t v) {
    char tmp[12];
    int  i = 0;
    if (v == 0) { ring_putc('0'); return; }
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i-- > 0) ring_putc(tmp[i]);
}

/* =========================================================================
 * Internal: write hex uint32 into ring buffer
 * ========================================================================= */

static void ring_putx(uint32_t v) {
    const char *hex = "0123456789abcdef";
    ring_puts("0x");
    bool leading = true;
    for (int s = 28; s >= 0; s -= 4) {
        uint32_t nibble = (v >> s) & 0xF;
        if (nibble != 0) leading = false;
        if (!leading || s == 0) ring_putc(hex[nibble]);
    }
}

/* =========================================================================
 * klog_init
 * ========================================================================= */

void klog_init(void) {
    klog_head  = 0;
    klog_size  = 0;
    klog_ready = true;
    for (int i = 0; i < 5; i++) klog_counts[i] = 0;
}

/* =========================================================================
 * klog — format and store a log entry
 * ========================================================================= */

void klog(int level, const char *subsystem, const char *fmt, ...) {
    if (level < KLOG_LEVEL) return;

    /* Disable interrupts to prevent buffer corruption from IRQ klog calls */
    uint32_t irq_flags = irq_save();   /* restore, never blindly sti(): we may be in an ISR
                                          or before the PIC is remapped */

    /* Clamp level */
    if (level < 0) level = 0;
    if (level > 4) level = 4;
    klog_counts[level]++;

    /* Prefix: [uptime_ms] [LEVEL] [subsystem] */
    ring_putc('[');
    ring_putu(get_uptime_ms());
    ring_puts("] [");
    ring_puts(level_names[level]);
    ring_puts("] [");
    ring_puts(subsystem ? subsystem : "?");
    ring_puts("] ");

    /* Format string */
    va_list args;
    va_start(args, fmt);
    const char *p = fmt;
    while (*p) {
        if (*p != '%') {
            ring_putc(*p++);
            continue;
        }
        p++; /* skip '%' */
        /* Skip flags and width like 08, 04, etc. */
        while (*p == '0' || (*p >= '1' && *p <= '9') || *p == '-') {
            p++;
        }
        switch (*p) {
            case 's': {
                const char *s = va_arg(args, const char *);
                ring_puts(s ? s : "(null)");
                break;
            }
            case 'd': {
                int v = va_arg(args, int);
                if (v < 0) { ring_putc('-'); v = -v; }
                ring_putu((uint32_t)v);
                break;
            }
            case 'u':
                ring_putu(va_arg(args, uint32_t));
                break;
            case 'x':
            case 'p':
                ring_putx(va_arg(args, uint32_t));
                break;
            case 'c':
                ring_putc((char)va_arg(args, int));
                break;
            case '%':
                ring_putc('%');
                break;
            default:
                ring_putc('%');
                ring_putc(*p);
                break;
        }
        p++;
    }
    va_end(args);
    ring_putc('\n');

    irq_restore(irq_flags);
}

/* =========================================================================
 * klog_dump — print entire ring buffer to console
 * ========================================================================= */

void klog_dump(void) {
    if (!klog_ready || klog_size == 0) {
        printf("(empty log)\n");
        return;
    }
    /* Calculate start position */
    uint32_t start = 0;
    if (klog_size >= KLOG_BUF_SIZE) {
        /* Buffer has wrapped; oldest byte is at head */
        start = klog_head % KLOG_BUF_SIZE;
    }
    /* Print */
    for (uint32_t i = 0; i < klog_size; i++) {
        putchar(klog_buf[(start + i) % KLOG_BUF_SIZE]);
    }
}

/* =========================================================================
 * klog_read — copy ring buffer contents to caller's buffer
 * ========================================================================= */

uint32_t klog_read(char *buf, uint32_t len) {
    if (!klog_ready || klog_size == 0 || !buf || len == 0)
        return 0;

    uint32_t n = klog_size < len ? klog_size : len;
    uint32_t start = 0;
    if (klog_size >= KLOG_BUF_SIZE) {
        start = klog_head % KLOG_BUF_SIZE;
    }
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = klog_buf[(start + i) % KLOG_BUF_SIZE];
    }
    return n;
}

/* =========================================================================
 * klog_total_bytes
 * ========================================================================= */

uint32_t klog_total_bytes(void) {
    return klog_size;
}
