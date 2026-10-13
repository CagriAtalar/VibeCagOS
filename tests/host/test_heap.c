/*
 * Host-side tests for the kernel heap (src/kernel/kmalloc.c).
 *
 * The REAL kmalloc.c is compiled (copied to build/hosttest by the Makefile) against
 * tests/host/shim/*; only the frame allocator underneath is faked. Each scenario
 * runs in its own forked child because the allocator keeps global state, so
 * every test starts from a pristine heap and a crash/assert fails only itself.
 *
 *   make host-test      builds and runs this with ASan+UBSan (release + KMALLOC_DEBUG)
 */
#define _GNU_SOURCE
#include "kmalloc.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- fake PMM -------------------------------------------------------------
 * A fixed arena of ARENA_FRAMES frames. `limit` caps how many frames may be
 * outstanding (OOM simulation). `gap` makes every allocation skip one frame
 * first, so two heap regions are never adjacent in memory — exactly the case
 * the old unconditional coalescing mishandled. */
#define ARENA_FRAMES 4096u
static uint8_t *arena;
static size_t   bump, outstanding, limit = (size_t)-1;
static bool     gap;
static size_t   frames_handed_out, free_calls;

paddr_t pmm_alloc_frames(size_t count) {
    if (count == 0 || outstanding + count > limit) return 0;
    size_t start = bump + (gap ? 1 : 0);
    if (start + count > ARENA_FRAMES) return 0;
    bump = start + count;
    outstanding += count;
    frames_handed_out += count;
    memset(arena + start * PAGE_SIZE, 0, count * PAGE_SIZE);   /* PMM returns zeroed frames */
    return (paddr_t)(arena + start * PAGE_SIZE);
}
void pmm_free_frames(paddr_t p, size_t count) { (void)p; outstanding -= count; free_calls++; }

/* ---- PANIC capture ---- */
static jmp_buf panic_jb;
static bool    panic_armed;
static char    panic_msg[160];
void host_panic(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(panic_msg, sizeof panic_msg, fmt, ap);
    va_end(ap);
    if (!panic_armed) { fprintf(stderr, "UNEXPECTED PANIC: %s\n", panic_msg); _exit(99); }
    longjmp(panic_jb, 1);
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "    CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); _exit(1); } } while (0)
#define CHECK_VALID() do { int v_ = kmalloc_validate(); if (v_) { fprintf(stderr, "    kmalloc_validate()=%d at line %d\n", v_, __LINE__); _exit(1); } } while (0)

static uint32_t rng_state;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

static void fresh(void) {
    arena = aligned_alloc(PAGE_SIZE, (size_t)ARENA_FRAMES * PAGE_SIZE);
    if (!arena) _exit(98);
    bump = outstanding = frames_handed_out = free_calls = 0;
    limit = (size_t)-1; gap = false; rng_state = 12345;
}
static void fill(void *p, size_t n, uint8_t seed) { for (size_t i = 0; i < n; i++) ((uint8_t *)p)[i] = (uint8_t)(seed + i * 7); }
static bool verify(const void *p, size_t n, uint8_t seed) { for (size_t i = 0; i < n; i++) if (((const uint8_t *)p)[i] != (uint8_t)(seed + i * 7)) return false; return true; }

/* ---- tests ---- */
static void t_zero_size(void) {
    kmalloc_init(); CHECK_VALID();
    CHECK(kmalloc(0) == NULL);
    struct kmalloc_stats s; kmalloc_get_stats(&s);
    CHECK(s.allocated_bytes == 0 && s.num_allocs == 0);
    kfree(NULL);                       /* documented safe */
    CHECK_VALID();
}
static void t_min_and_alignment(void) {
    kmalloc_init();
    void *p[64];
    for (size_t i = 0; i < 64; i++) {
        size_t sz = 1 + i * 3;                          /* 1,4,7,... */
        p[i] = kmalloc(sz);
        CHECK(p[i] != NULL);
        CHECK(((uintptr_t)p[i] & 7u) == 0);             /* documented 8-byte alignment */
        fill(p[i], sz, (uint8_t)i);
    }
    CHECK_VALID();
    for (size_t i = 0; i < 64; i++) CHECK(verify(p[i], 1 + i * 3, (uint8_t)i));
    for (size_t i = 0; i < 64; i++) kfree(p[i]);
    CHECK_VALID();
}
static void t_reuse(void) {
    kmalloc_init();
    void *a = kmalloc(100), *b = kmalloc(100);
    CHECK(a && b && a != b);
    kfree(a);
    void *c = kmalloc(100);
    CHECK(c == a);                                       /* first fit reuses the freed block */
    kfree(b); kfree(c);
    CHECK_VALID();
}
static void t_large(void) {
    kmalloc_init();
    size_t big = 1u << 20;                               /* 1 MiB: forces growth far past the initial 4 frames */
    uint8_t *p = kmalloc(big);
    CHECK(p != NULL);
    fill(p, big, 3); CHECK(verify(p, big, 3));
    struct kmalloc_stats s; kmalloc_get_stats(&s);
    CHECK(s.heap_frames * PAGE_SIZE >= big);
    CHECK_VALID();
    kfree(p);
    kmalloc_get_stats(&s);
    CHECK(s.allocated_bytes == 0);
    CHECK_VALID();
}
static int orders_idx;
static void t_orders_one(void) {
    static const int perm[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    kmalloc_init();
    struct kmalloc_stats before, after; kmalloc_get_stats(&before);
    void *p[3] = { kmalloc(64), kmalloc(128), kmalloc(256) };
    CHECK(p[0] && p[1] && p[2]);
    for (int i = 0; i < 3; i++) kfree(p[perm[orders_idx][i]]);
    CHECK_VALID();
    kmalloc_get_stats(&after);
    CHECK(after.allocated_bytes == before.allocated_bytes && after.num_blocks == before.num_blocks);
}
static void t_overflow(void) {
    kmalloc_init();
    struct kmalloc_stats a, b; kmalloc_get_stats(&a);
    size_t bad[] = { (size_t)-1, (size_t)-2, (size_t)-8, (size_t)-16, (size_t)-4096, (size_t)-(4096 + 64),
                     (size_t)1 << (sizeof(size_t) * 8 - 1) };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(kmalloc(bad[i]) == NULL);
    kmalloc_get_stats(&b);
    CHECK(a.allocated_bytes == b.allocated_bytes && a.heap_frames == b.heap_frames && a.num_allocs == b.num_allocs);
    CHECK_VALID();
}
static void t_oom_and_recovery(void) {
    kmalloc_init();
    limit = 40;                                          /* 40 frames total */
    void *p[256]; size_t n = 0;
    for (; n < 256; n++) { p[n] = kmalloc(3000); if (!p[n]) break; }
    CHECK(n > 8 && n < 256);                             /* it did run out... */
    CHECK_VALID();                                       /* ...without corrupting the heap */
    CHECK(kmalloc(3000) == NULL);                        /* still NULL, not a panic */
    CHECK(outstanding <= limit);
    for (size_t i = 0; i < n; i++) kfree(p[i]);
    CHECK_VALID();
    void *q = kmalloc(3000);                             /* freed memory is usable again */
    CHECK(q != NULL);
    kfree(q);
    struct kmalloc_stats s; kmalloc_get_stats(&s);
    CHECK(s.allocated_bytes == 0);
}
static void t_noncontiguous_regions(void) {
    kmalloc_init();
    gap = true;                                          /* every later region sits behind a 1-frame hole */
    void *p[40]; size_t n = 0;
    for (; n < 40; n++) { p[n] = kmalloc(3000); CHECK(p[n]); }
    CHECK_VALID();
    for (size_t i = 0; i < n; i++) kfree(p[i]);
    CHECK_VALID();                                       /* code 7 if blocks were merged across a hole */
    struct kmalloc_stats s; kmalloc_get_stats(&s);
    CHECK(s.allocated_bytes == 0);
    CHECK(s.num_blocks >= 2);                            /* regions stay separate blocks */
}
static void t_double_free_detected(void) {
    kmalloc_init();
    void *a = kmalloc(32), *keep = kmalloc(32);
    CHECK(a && keep);
    kfree(a);
    panic_armed = true;
    if (setjmp(panic_jb) == 0) { kfree(a); CHECK(!"double free went undetected"); }
    panic_armed = false;
    CHECK(strstr(panic_msg, "double-free") || strstr(panic_msg, "canary"));
    CHECK_VALID();                                       /* detection happens before any mutation */
    kfree(keep);
    CHECK_VALID();
}
static void t_corruption_detected(void) {
    kmalloc_init();
    uint8_t *a = kmalloc(64), *b = kmalloc(64);
    CHECK(a && b);
    CHECK_VALID();
    memset(a + 64, 0xEE, 16);                            /* overrun a into b's header (a classic heap overflow) */
    CHECK(kmalloc_validate() != 0);                      /* the overflow must be noticed */
}
static void t_stress(void) {
    kmalloc_init();
    enum { SLOTS = 300, OPS = 30000 };
    static void *slot[SLOTS]; static size_t sz[SLOTS]; static uint8_t seed[SLOTS];
    for (int op = 0; op < OPS; op++) {
        unsigned i = rnd() % SLOTS;
        if (slot[i]) {
            CHECK(verify(slot[i], sz[i], seed[i]));      /* neighbours never clobbered it */
            kfree(slot[i]); slot[i] = NULL;
        } else {
            sz[i] = 1 + rnd() % (rnd() % 8 == 0 ? 6000 : 300);
            seed[i] = (uint8_t)rnd();
            slot[i] = kmalloc(sz[i]);
            CHECK(slot[i] != NULL);
            CHECK(((uintptr_t)slot[i] & 7u) == 0);
            fill(slot[i], sz[i], seed[i]);
        }
        if (op % 500 == 0) CHECK_VALID();
    }
    CHECK_VALID();
    for (int i = 0; i < SLOTS; i++) if (slot[i]) { CHECK(verify(slot[i], sz[i], seed[i])); kfree(slot[i]); }
    CHECK_VALID();
    struct kmalloc_stats s; kmalloc_get_stats(&s);
    CHECK(s.allocated_bytes == 0);
    CHECK(s.heap_frames == frames_handed_out);           /* heap accounting matches the PMM's view */
    CHECK(s.heap_frames <= 64);                          /* bounded high-water mark for this workload */
    CHECK(s.num_allocs == s.num_frees);
}
static void t_retained_not_leaked(void) {
    /* Documents the lifecycle: the heap keeps its frames after everything is
     * freed (no pmm_free_frames calls), but that growth is bounded and
     * repeated cycles do NOT grow it further. */
    kmalloc_init();
    struct kmalloc_stats s0, s1, s2;
    for (int round = 0; round < 3; round++) {
        void *p[20];
        for (int i = 0; i < 20; i++) { p[i] = kmalloc(5000); CHECK(p[i]); }
        for (int i = 0; i < 20; i++) kfree(p[i]);
        if (round == 0) kmalloc_get_stats(&s0);
        if (round == 1) kmalloc_get_stats(&s1);
        if (round == 2) kmalloc_get_stats(&s2);
    }
    CHECK(s0.heap_frames > 4);                           /* it grew */
    CHECK(s1.heap_frames == s0.heap_frames && s2.heap_frames == s0.heap_frames);  /* and stopped growing */
    CHECK(free_calls == 0);                              /* documented: never returns frames */
    CHECK_VALID();
}

/* ---- runner ---- */
struct test { const char *name; void (*fn)(void); };
static struct test tests[] = {
    { "zero-size alloc/free(NULL)",          t_zero_size },
    { "min sizes + 8-byte alignment",         t_min_and_alignment },
    { "freed block reuse (first fit)",        t_reuse },
    { "large (1 MiB) allocation",             t_large },
    { "overflow / invalid sizes -> NULL",     t_overflow },
    { "OOM returns NULL, heap intact, recovers", t_oom_and_recovery },
    { "no merge across non-contiguous regions",  t_noncontiguous_regions },
    { "double free detected before mutation", t_double_free_detected },
    { "heap overflow into neighbour detected", t_corruption_detected },
    { "bounded retention (no growth per cycle)", t_retained_not_leaked },
    { "stress: 30000 random alloc/free ops",   t_stress },
};

static int run_child(void (*fn)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) { fresh(); fn(); _exit(0); }
    int st; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

int main(void) {
    int failed = 0;
#ifdef KMALLOC_DEBUG
    const char *mode = "KMALLOC_DEBUG (canaries+poison)";
#else
    const char *mode = "release";
#endif
    printf("== host heap tests: %s ==\n", mode);
    for (size_t i = 0; i < sizeof tests / sizeof *tests; i++) {
        int rc = run_child(tests[i].fn);
        printf("  %s  %s\n", rc == 0 ? "PASS" : "FAIL", tests[i].name);
        if (rc) { printf("        (exit status %d)\n", rc); failed++; }
    }
    for (orders_idx = 0; orders_idx < 6; orders_idx++) {   /* each free order in its own pristine heap */
        int rc = run_child(t_orders_one);
        printf("  %s  free order permutation %d coalesces back to baseline\n", rc == 0 ? "PASS" : "FAIL", orders_idx);
        if (rc) failed++;
    }
    printf("%s (%d failed)\n", failed ? "FAILED" : "ALL PASSED", failed);
    return failed != 0;
}
