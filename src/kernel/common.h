#pragma once

/*
 * VibeCagOS — Common Types and Macros
 * Freestanding kernel environment definitions.
 */

/* Standard integer types */
typedef int            bool;
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef unsigned long long uint64_t;
typedef signed int     int32_t;
typedef signed short   int16_t;
typedef signed char    int8_t;
typedef uint32_t       size_t;
typedef uint32_t       paddr_t;
typedef uint32_t       vaddr_t;
typedef uint32_t       uintptr_t;
typedef int32_t        intptr_t;

#define true   1
#define false  0
#define NULL   ((void *)0)

/* Page constants */
#define PAGE_SIZE  4096
#define PAGE_SHIFT 12

/* Alignment helpers (using clang/gcc builtins) */
#define align_up(value, align)   __builtin_align_up(value, align)
#define align_down(value, align) ((value) & ~((align) - 1))
#define is_aligned(value, align) __builtin_is_aligned(value, align)
#define offsetof(type, member)   __builtin_offsetof(type, member)

/* va_list */
#define va_list  __builtin_va_list
#define va_start __builtin_va_start
#define va_end   __builtin_va_end
#define va_arg   __builtin_va_arg

/* Array size */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Min/Max */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/*
 * Syscall numbers
 */

/*
 * Standard library functions (implemented in common.c)
 */
void  *memset(void *buf, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
int    memcmp(const void *s1, const void *s2, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
int    strcmp(const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, int n);
size_t strlen(const char *s);
char  *strcat(char *dst, const char *src);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);

/* Formatted output (kernel printf — outputs via putchar()) */
void printf(const char *fmt, ...);

/* putchar is declared in kernel.h as it needs hardware access */
void putchar(char ch);
