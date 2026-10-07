/* VibeCagOS - minimal user-space C library. Talks to the kernel only via int 0x80. */
#include "ulib.h"
#include <stdarg.h>

usize strlen(const char *s) { usize n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, usize n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) { } return r; }
char *strncpy(char *d, const char *s, usize n) {
    usize i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
void *memcpy(void *d, const void *s, usize n) {
    unsigned char *dp = d; const unsigned char *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}
void *memmove(void *d, const void *s, usize n) {
    unsigned char *dp = d; const unsigned char *sp = s;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}
void *memset(void *d, int c, usize n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
int memcmp(const void *a, const void *b, usize n) {
    const unsigned char *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}
int atoi(const char *s) {
    int neg = 0, v = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

int uputs(int fd, const char *s) {
    usize n = strlen(s), done = 0;
    while (done < n) {
        int r = sys_write(fd, s + done, n - done);
        if (r <= 0) return r < 0 ? r : (int)done;
        done += (usize)r;
    }
    return (int)done;
}

/* ---- printf ------------------------------------------------------------ */
struct obuf { int fd; char b[256]; int n; int total; };
static void oflush(struct obuf *o) { if (o->n) { sys_write(o->fd, o->b, (usize)o->n); o->n = 0; } }
static void oput(struct obuf *o, char c) { if (o->n == (int)sizeof(o->b)) oflush(o); o->b[o->n++] = c; o->total++; }

static void onum(struct obuf *o, unsigned v, unsigned base, int neg, int width, int left, char pad) {
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = "0123456789abcdef"[v % base]; v /= base; }
    if (neg) t[n++] = '-';
    int padn = width > n ? width - n : 0;
    if (!left) { if (pad == '0' && neg) { oput(o, '-'); n--; } while (padn--) oput(o, pad); }
    while (n) oput(o, t[--n]);
    if (left) while (padn--) oput(o, ' ');
}

int uprintf(int fd, const char *fmt, ...) {
    struct obuf o = { fd, {0}, 0, 0 };
    va_list ap; va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { oput(&o, *fmt); continue; }
        fmt++;
        int left = 0; char pad = ' '; int width = 0;
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        switch (*fmt) {
        case 'd': { int v = va_arg(ap, int); onum(&o, v < 0 ? -(unsigned)v : (unsigned)v, 10, v < 0, width, left, pad); break; }
        case 'u': onum(&o, va_arg(ap, unsigned), 10, 0, width, left, pad); break;
        case 'x': onum(&o, va_arg(ap, unsigned), 16, 0, width, left, pad); break;
        case 'o': onum(&o, va_arg(ap, unsigned), 8, 0, width, left, pad); break;
        case 'c': oput(&o, (char)va_arg(ap, int)); break;
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)";
                    int l = (int)strlen(s); int padn = width > l ? width - l : 0;
                    if (!left) while (padn--) oput(&o, ' ');
                    while (*s) oput(&o, *s++);
                    if (left) while (padn--) oput(&o, ' ');
                    break; }
        case '%': oput(&o, '%'); break;
        case 0: fmt--; break;
        default: oput(&o, '%'); oput(&o, *fmt); break;
        }
    }
    va_end(ap);
    oflush(&o);
    return o.total;
}

/* ---- line input (the console is a raw TTY: echo/editing happen here) ---- */
int ugetline(char *buf, int max) {
    int len = 0;
    for (;;) {
        char c;
        int r = sys_read(0, &c, 1);
        if (r < 0) return -1;
        if (r == 0) continue;
        if (c == '\r' || c == '\n') { sys_write(1, "\n", 1); buf[len] = 0; return len; }
        if (c == 0x7f || c == '\b') {
            if (len > 0) { len--; sys_write(1, "\b \b", 3); }
            continue;
        }
        if (c >= ' ' && c < 0x7f && len < max - 1) { buf[len++] = c; sys_write(1, &c, 1); }
    }
}
