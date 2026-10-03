#include "common.h"

/* ---------------------------------------------------------------------------
 * Memory operations
 * --------------------------------------------------------------------------- */

void *memset(void *buf, int c, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    while (n--)
        *p++ = (uint8_t)c;
    return buf;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = (const uint8_t *)s1;
    const uint8_t *b = (const uint8_t *)s2;
    while (n--) {
        if (*a != *b)
            return (int)*a - (int)*b;
        a++; b++;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * String operations
 * --------------------------------------------------------------------------- */

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while (*src)
        *d++ = *src++;
    *d = '\0';
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (n > 0 && *src) {
        *d++ = *src++;
        n--;
    }
    while (n-- > 0)
        *d++ = '\0';
    return dst;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        if (*s1 != *s2)
            break;
        s1++; s2++;
    }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char c1 = (unsigned char)s1[i];
        unsigned char c2 = (unsigned char)s2[i];
        if (c1 != c2)
            return (int)c1 - (int)c2;
        if (c1 == '\0')
            return 0;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (*s++)
        len++;
    return len;
}

char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d)
        d++;
    while (*src)
        *d++ = *src++;
    *d = '\0';
    return dst;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    if (c == '\0')
        return (char *)s;
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Formatted output
 *
 * Supports: %c %s %d %u %x %p
 * Flags   : - (left-justify), 0 (zero-pad)
 * Width   : numeric field width
 * --------------------------------------------------------------------------- */

void putchar(char ch);  /* provided by kernel.c */

/* Convert unsigned integer to string in given base. Returns string length. */
static int uint_to_str(unsigned value, int base, char *buf, int bufsz) {
    static const char digits[] = "0123456789abcdef";
    int len = 0;
    if (value == 0) {
        buf[len++] = '0';
    } else {
        /* Build reversed */
        while (value > 0 && len < bufsz - 1) {
            buf[len++] = digits[value % (unsigned)base];
            value /= (unsigned)base;
        }
        /* Reverse in-place */
        for (int a = 0, b = len - 1; a < b; a++, b--) {
            char t = buf[a]; buf[a] = buf[b]; buf[b] = t;
        }
    }
    buf[len] = '\0';
    return len;
}

/*
 * pad_print — print a string/number with padding.
 * str: the text to print
 * len: length of str
 * width: minimum field width
 * left: 1 = left-align, 0 = right-align
 * pad: padding character (' ' or '0')
 */
static void pad_print(const char *str, int len, int width, int left, char pad) {
    int spaces = width - len;
    if (!left)
        for (int i = 0; i < spaces; i++) putchar(pad);
    for (int i = 0; i < len; i++) putchar(str[i]);
    if (left)
        for (int i = 0; i < spaces; i++) putchar(' ');
}

void printf(const char *fmt, ...) {
    va_list vargs;
    va_start(vargs, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            putchar(*fmt++);
            continue;
        }
        fmt++;  /* skip '%' */

        /* Parse flags */
        int left_align = 0;
        int zero_pad   = 0;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') { left_align = 1; zero_pad = 0; }
            if (*fmt == '0' && !left_align) zero_pad = 1;
            fmt++;
        }

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        char pad = (zero_pad && !left_align) ? '0' : ' ';

        switch (*fmt) {
            case '\0':
                putchar('%');
                goto end;
            case '%':
                putchar('%');
                break;
            case 'c': {
                char c = (char)va_arg(vargs, int);
                char cs[2] = { c, 0 };
                pad_print(cs, 1, width, left_align, ' ');
                break;
            }
            case 's': {
                const char *s = va_arg(vargs, const char *);
                if (!s) s = "(null)";
                pad_print(s, (int)strlen(s), width, left_align, ' ');
                break;
            }
            case 'd': {
                int value = va_arg(vargs, int);
                char buf[16];
                int  neg = (value < 0);
                unsigned mag = neg ? (unsigned)(-value) : (unsigned)value;
                int  len = uint_to_str(mag, 10, buf, sizeof(buf));
                if (neg) {
                    /* Insert minus */
                    for (int i = len; i >= 0; i--) buf[i+1] = buf[i];
                    buf[0] = '-';
                    len++;
                }
                pad_print(buf, len, width, left_align, pad);
                break;
            }
            case 'u': {
                char buf[16];
                int  len = uint_to_str(va_arg(vargs, unsigned), 10, buf, sizeof(buf));
                pad_print(buf, len, width, left_align, pad);
                break;
            }
            case 'o': {
                char buf[16];
                int  len = uint_to_str(va_arg(vargs, unsigned), 8, buf, sizeof(buf));
                pad_print(buf, len, width, left_align, pad);
                break;
            }
            case 'x': {
                char buf[16];
                int  len = uint_to_str(va_arg(vargs, unsigned), 16, buf, sizeof(buf));
                /* For %08x style, use zero-pad */
                if (width == 8 && zero_pad)
                    pad_print(buf, len, width, 0, '0');
                else
                    pad_print(buf, len, width, left_align, pad);
                break;
            }
            case 'p': {
                /* Pointer: 0x followed by 8 hex digits */
                char buf[16];
                int  len = uint_to_str(va_arg(vargs, unsigned), 16, buf, sizeof(buf));
                /* Pad to 8 */
                char pbuf[12];
                pbuf[0] = '0'; pbuf[1] = 'x';
                for (int i = 0; i < 8 - len; i++) pbuf[2 + i] = '0';
                for (int i = 0; i < len; i++) pbuf[2 + (8 - len) + i] = buf[i];
                pbuf[10] = '\0';
                pad_print(pbuf, 10, width, left_align, ' ');
                break;
            }
            default:
                putchar('%');
                putchar(*fmt);
                break;
        }
        fmt++;
    }

end:
    va_end(vargs);
}
