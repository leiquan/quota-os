/*
 * Substrate for the upstream CLoop link. malloc and ordinary mmap come from
 * the high half of the structure parent window. mprotect uses the EL1 map.
 */
#include "quotaos.h"
#include <stdarg.h>

void abort(void);

extern char __image_start[], __image_end[];
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

static uint8_t jsc_stack[1u << 20] __attribute__((aligned(16)));

int jsc_call_on_stack(int (*fn)(void))
{
    uint64_t top = ((uint64_t)jsc_stack + sizeof jsc_stack) & ~15ull;
    register uint64_t result asm("x0") = (uint64_t)fn;
    register uint64_t top_reg asm("x1") = top;
    asm volatile(
        "stp x29, x30, [sp, #-16]!\n"
        "mov x9, sp\n"
        "mov sp, x1\n"
        "blr x0\n"
        "mov sp, x9\n"
        "ldp x29, x30, [sp], #16\n"
        : "+r"(result), "+r"(top_reg)
        :
        : "x9", "memory", "cc");
    return (int)result;
}

static uint8_t *arena;
static size_t arena_size;
static size_t arena_used;
static uint64_t arena_limit;
static int arena_ready;
static int mmap_logs;
static int structure_handed;
static uint64_t structure_live_lo, structure_live_hi;
static int ownership_tested;

__thread int errno;
extern char __tls_start[];
extern char __tls_end[];

__attribute__((visibility("hidden"))) int pas_process_is_shutting_down(void)
{
    return 0;
}

int mkostemp(char *templ, int flags)
{
    (void)templ;
    (void)flags;
    errno = 38;
    return -1;
}

void quotaos_entropy(uint8_t *buf, size_t n)
{
    static int logged;
    static uint64_t state;
    if (!logged) {
        logged = 1;
        printf("[jsc] entropy is CNTVCT, not cryptographic\n");
    }
    for (size_t i = 0; i < n; i++) {
        uint64_t c;
        asm volatile("mrs %0, cntvct_el0" : "=r"(c));
        state = state * 6364136223846793005ull + c + 1;
        buf[i] = (uint8_t)(state >> 33);
    }
}

int *__errno_location(void)
{
    return &errno;
}

uint64_t jsc_arena_end(void)
{
    return arena_ready ? arena_limit : (uint64_t)__image_end;
}

void jsc_arena_init(void)
{
    uint64_t base = (uint64_t)__image_start & ~((16ull << 20) - 1);
    uint64_t pool = base + (uint64_t)system_reserve_kb() * 1024;
    uint64_t structure = webkit_structure_base();
    uint64_t window = webkit_structure_bytes();
    arena_ready = 0;
    arena_used = 0;
    /* The aligned structure heap uses the low half of the window. The high
     * half is released by the aligner and becomes the malloc arena. */
    if (structure && window >= (64ull << 20) && structure >= pool) {
        uint64_t start = structure + window / 2;
        arena = (uint8_t *)start;
        arena_limit = structure + window;
        arena_size = (size_t)(arena_limit - start);
        memset(arena, 0, arena_size);
        arena_ready = 1;
        printf("[jsc] arena 0x%lx %lu bytes\n", start, (uint64_t)arena_size);
        printf("[jsc] layout parent 0x%lx-0x%lx children heap 0x%lx-0x%lx arena 0x%lx-0x%lx\n",
               structure, structure + window, structure, start, start, arena_limit);
        boot_check_reservations(structure, window, start, arena_limit);
        return;
    }
    printf("[jsc] arena refused pool 0x%lx structure 0x%lx\n", pool, structure);
    arena_limit = (uint64_t)__image_end;
}

static void *bump(size_t n)
{
    n = (n + (size_t)15) & ~(size_t)15;
    if (!arena_ready || n > arena_size - arena_used)
        return 0;
    void *p = arena + arena_used;
    arena_used += n;
    return p;
}

static void *bump_pages(size_t n)
{
    size_t page = 4096;
    if (!arena_ready)
        return 0;
    size_t aligned = (arena_used + page - 1) & ~(page - 1);
    size_t bytes = (n + page - 1) & ~(page - 1);
    if (bytes > arena_size || aligned > arena_size - bytes)
        return 0;
    void *p = arena + aligned;
    arena_used = aligned + bytes;
    return p;
}

static int range_hit(uint64_t a, uint64_t n, uint64_t lo, uint64_t hi)
{
    return n && hi > lo && a < hi && a + n > lo;
}

/* The structure heap and the malloc arena are siblings inside one parent.
 * Fill the arena, prove the bump cannot enter the heap, then give the arena back. */
static void jsc_ownership_test(void)
{
    if (ownership_tested || !arena_ready || structure_live_hi <= structure_live_lo)
        return;
    if (range_hit((uint64_t)arena, arena_size, structure_live_lo, structure_live_hi))
        return;
    ownership_tested = 1;
    uint32_t *canary = (uint32_t *)structure_live_lo;
    uint32_t saved_canary = *canary;
    *canary = 0xA11CE001u;
    size_t saved_used = arena_used;
    arena_used = 0;
    size_t filled = 0;
    int crossed = 0;
    for (;;) {
        void *p = bump_pages(1u << 20);
        if (!p)
            break;
        uint64_t a = (uint64_t)p;
        if (a < (uint64_t)arena || a + (1u << 20) > arena_limit)
            crossed = 1;
        if (range_hit(a, 1u << 20, structure_live_lo, structure_live_hi))
            crossed = 1;
        *(uint32_t *)p = 0x22222222u;
        filled += 1u << 20;
    }
    for (;;) {
        void *p = bump_pages(4096);
        if (!p)
            break;
        uint64_t a = (uint64_t)p;
        if (a < (uint64_t)arena || a + 4096 > arena_limit)
            crossed = 1;
        if (range_hit(a, 4096, structure_live_lo, structure_live_hi))
            crossed = 1;
        filled += 4096;
    }
    arena_used = arena_size - 4096;
    void *last = bump_pages(4096);
    int exact = last && (uint64_t)last + 4096 == arena_limit;
    void *overflow = bump_pages(4096);
    int stopped = overflow == 0;
    int canary_ok = *canary == 0xA11CE001u;
    *canary = saved_canary;
    arena_used = saved_used;
    printf("[jsc] ownership heap 0x%lx-0x%lx arena 0x%lx-0x%lx filled %lu crossed %d exact %d stopped %d canary %d\n",
           structure_live_lo, structure_live_hi, (uint64_t)arena, arena_limit,
           (uint64_t)filled, crossed, exact, stopped, canary_ok);
    if (crossed || !exact || !stopped || !canary_ok || filled != arena_size) {
        printf("[jsc] ownership FAIL\n");
        abort();
    }
    printf("[jsc] ownership PASS\n");
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    (void)fd;
    (void)off;
    if (mmap_logs < 24) {
        mmap_logs++;
        printf("[jsc] mmap addr 0x%lx len %lu prot 0x%x flags 0x%x\n",
               (uint64_t)addr, (uint64_t)len, prot, flags);
    }
    /* MAP_FIXED would claim a chosen address. Refuse it. */
    if (flags & 0x10) {
        errno = 22;
        return (void *)-1;
    }
    if (!len) {
        errno = 22;
        return (void *)-1;
    }
    /* StructureID encoding needs one aligned reservation. The window is real
     * RAM at the top of the machine, excluded from the colored pool. */
    if (!structure_handed && (flags & 0x4000)) {
        uint64_t base = webkit_structure_base();
        uint64_t bytes = webkit_structure_bytes();
        if (base && len >= (32ull << 20) && len <= bytes) {
            structure_handed = 1;
            structure_live_lo = base;
            structure_live_hi = base + len;
            memset((void *)base, 0, len);
            printf("[jsc] structure window 0x%lx %lu bytes\n", base, (uint64_t)len);
            return (void *)base;
        }
    }
    if (structure_live_hi > structure_live_lo
        && range_hit((uint64_t)arena, arena_size, structure_live_lo, structure_live_hi)) {
        errno = 12;
        printf("[jsc] mmap refused len %lu while structure live still covers the arena\n", (uint64_t)len);
        return (void *)-1;
    }
    void *p = bump_pages(len);
    if (p && range_hit((uint64_t)p, len, structure_live_lo, structure_live_hi)) {
        errno = 12;
        printf("[jsc] mmap landed in the structure heap\n");
        return (void *)-1;
    }
    if (p && mmap_logs <= 24)
        printf("[jsc] mmap ok 0x%lx %lu\n", (uint64_t)p, (uint64_t)len);
    if (!p) {
        errno = 12;
        printf("[jsc] mmap refused len %lu used %lu\n", (uint64_t)len, (uint64_t)arena_used);
        return (void *)-1;
    }
    return p;
}

int mprotect(void *p, size_t n, int prot)
{
    if (mmu_protect((uint64_t)p, n, prot) == 0)
        return 0;
    errno = 13;
    printf("[jsc] mprotect refused prot 0x%x len %lu\n", prot, (uint64_t)n);
    return -1;
}

int munmap(void *p, size_t n)
{
    uint64_t a = (uint64_t)p;
    uint64_t b = a + n;
    if (structure_live_hi > structure_live_lo && a < structure_live_hi && b > structure_live_lo) {
        if (a <= structure_live_lo && b >= structure_live_hi)
            structure_live_lo = structure_live_hi = 0;
        else if (a <= structure_live_lo)
            structure_live_lo = b;
        else if (b >= structure_live_hi)
            structure_live_hi = a;
        else
            printf("[jsc] munmap left a hole in the structure heap\n");
        jsc_ownership_test();
        return 0;
    }
    if (!arena_ready || (uint8_t *)p < arena || (uint8_t *)p >= arena + arena_used)
        return -1;
    size_t off = (size_t)((uint8_t *)p - arena);
    size_t end = off + ((n + (size_t)15) & ~(size_t)15);
    if (end >= arena_used)
        arena_used = off;
    return 0;
}

int madvise(void *p, size_t n, int advice)
{
    (void)p;
    (void)n;
    /* MADV_WILLNEED: the pages are already resident RAM. */
    if (advice == 3)
        return 0;
    errno = 22;
    return -1;
}

int prctl(int op, unsigned long a, unsigned long b, unsigned long c, unsigned long d)
{
    (void)op;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    errno = 22;
    return -1;
}

struct blk {
    size_t n;
    struct blk *next;
};

static struct blk *free_list;

void *malloc(size_t n)
{
    if (!n)
        n = 1;
    n = (n + (size_t)15) & ~(size_t)15;
    struct blk **pp = &free_list;
    for (struct blk *b = free_list; b; pp = &b->next, b = b->next) {
        if (b->n >= n) {
            *pp = b->next;
            return b + 1;
        }
    }
    struct blk *b = bump(sizeof *b + n);
    if (!b) {
        printf("[jsc] malloc refused %lu used %lu\n", (uint64_t)n, (uint64_t)arena_used);
        return 0;
    }
    b->n = n;
    return b + 1;
}

void free(void *p)
{
    if (!p)
        return;
    struct blk *b = (struct blk *)p - 1;
    b->next = free_list;
    free_list = b;
}

void *calloc(size_t a, size_t b)
{
    size_t n = a * b;
    if (a && n / a != b)
        return 0;
    void *p = malloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return 0;
    }
    struct blk *b = (struct blk *)p - 1;
    if (b->n >= n)
        return p;
    void *q = malloc(n);
    if (!q)
        return 0;
    memcpy(q, p, b->n);
    free(p);
    return q;
}

void *aligned_alloc(size_t align, size_t n)
{
    if (align <= 16)
        return malloc(n);
    if (align & (align - 1))
        return 0;
    uintptr_t raw = (uintptr_t)bump(n + align);
    if (!raw)
        return 0;
    return (void *)((raw + align - 1) & ~(uintptr_t)(align - 1));
}

static char *empty_env[] = { 0 };
char **environ = empty_env;

char *getenv(const char *name)
{
    (void)name;
    return 0;
}

int getpid(void)
{
    return 1;
}

long sysconf(int name)
{
    if (name == 30)
        return 4096;
    if (name == 84)
        return 1;
    errno = 22;
    return -1;
}

struct timespec_j {
    long tv_sec;
    long tv_nsec;
};

int clock_gettime(int clk, struct timespec_j *ts)
{
    (void)clk;
    if (!ts)
        return -1;
    uint64_t us = now_us();
    ts->tv_sec = (long)(us / 1000000ull);
    ts->tv_nsec = (long)((us % 1000000ull) * 1000ull);
    return 0;
}

int gettimeofday(struct timespec_j *tv, void *tz)
{
    (void)tz;
    return clock_gettime(0, tv);
}

int getrlimit(int r, void *lim)
{
    /* RLIMIT_STACK. The guest runs on jsc_stack, not the 8 KB shell stack. */
    if (r == 3 && lim) {
        unsigned long *p = lim;
        p[0] = sizeof jsc_stack;
        p[1] = sizeof jsc_stack;
        return 0;
    }
    errno = 22;
    return -1;
}

int sysinfo(void *info)
{
    (void)info;
    errno = 22;
    return -1;
}

void abort(void)
{
    uint64_t lr;
    asm volatile("mov %0, x30" : "=r"(lr));
    printf("[jsc] abort lr 0x%lx\n", lr);
    for (;;)
        asm volatile("wfi");
}

void exit(int code)
{
    printf("[jsc] exit %d\n", code);
    for (;;)
        asm volatile("wfi");
}

void __assert_fail(const char *e, const char *file, unsigned line, const char *fn)
{
    printf("[jsc] assert %s %s:%u %s\n", e, file, line, fn);
    abort();
}

int usleep(unsigned us)
{
    uint64_t t0 = now_us();
    while (now_us() - t0 < us)
        ;
    return 0;
}

int sched_yield(void)
{
    return 0;
}

int open(const char *path, int flags, ...)
{
    (void)flags;
    (void)path;
    errno = 2;
    return -1;
}

long read(int fd, void *buf, size_t n)
{
    (void)fd;
    (void)buf;
    (void)n;
    errno = 9;
    return -1;
}

long write(int fd, const void *buf, size_t n)
{
    if (fd == 1 || fd == 2) {
        const char *s = buf;
        for (size_t i = 0; i < n; i++)
            uart_putc(s[i]);
        return (long)n;
    }
    errno = 9;
    return -1;
}

int close(int fd)
{
    (void)fd;
    return 0;
}

int fsync(int fd)
{
    (void)fd;
    return 0;
}

int ftruncate(int fd, long len)
{
    (void)fd;
    (void)len;
    errno = 22;
    return -1;
}

void *dlopen(const char *n, int f)
{
    (void)n;
    (void)f;
    return 0;
}

void *dlsym(void *h, const char *n)
{
    (void)h;
    (void)n;
    return 0;
}

int dlclose(void *h)
{
    (void)h;
    return -1;
}

int dladdr(const void *a, void *info)
{
    (void)a;
    (void)info;
    return 0;
}

int dl_iterate_phdr(void *cb, void *data)
{
    (void)cb;
    (void)data;
    return -1;
}

char *strerror_r(int err, char *buf, size_t n)
{
    if (buf && n)
        buf[0] = 0;
    (void)err;
    return buf;
}

char *setlocale(int cat, const char *locale)
{
    (void)cat;
    (void)locale;
    return 0;
}

int atexit(void (*fn)(void))
{
    (void)fn;
    return 0;
}

int atoi(const char *s)
{
    int n = 0, sign = 1;
    if (!s)
        return 0;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return sign * n;
}

int backtrace(void **buf, int size)
{
    (void)buf;
    (void)size;
    return 0;
}

long syscall(long n, ...)
{
    /* aarch64 SYS_gettid. One thread. */
    if (n == 178)
        return 1;
    printf("[jsc] syscall %ld refused\n", n);
    errno = 38;
    return -1;
}

/* Single thread. create does not report a started thread. */
typedef unsigned long pthread_t;
typedef unsigned pthread_key_t;

static void *tls_slots[32];
static int tls_n;

int pthread_key_create(pthread_key_t *key, void (*dtor)(void *))
{
    (void)dtor;
    if (tls_n >= 32)
        return 11;
    *key = (pthread_key_t)tls_n++;
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    if (key >= 32)
        return 0;
    return tls_slots[key];
}

int pthread_setspecific(pthread_key_t key, const void *p)
{
    if (key >= 32)
        return 22;
    tls_slots[key] = (void *)p;
    return 0;
}

int pthread_once(int *once, void (*fn)(void))
{
    if (!once)
        return 22;
    if (*once)
        return 0;
    *once = 1;
    if (fn)
        fn();
    return 0;
}

int pthread_create(void *t, const void *attr, void *(*fn)(void *), void *arg)
{
    (void)t;
    (void)attr;
    (void)fn;
    (void)arg;
    printf("[jsc] pthread_create refused\n");
    return 11;
}

int pthread_join(pthread_t t, void **ret)
{
    (void)t;
    (void)ret;
    return 22;
}

int pthread_detach(pthread_t t)
{
    (void)t;
    return 22;
}

pthread_t pthread_self(void)
{
    return 1;
}

int pthread_equal(pthread_t a, pthread_t b)
{
    return a == b;
}

int pthread_kill(pthread_t t, int sig)
{
    (void)t;
    (void)sig;
    return 22;
}

int pthread_sigmask(int how, const void *set, void *old)
{
    (void)how;
    (void)set;
    if (old)
        memset(old, 0, 16);
    return 0;
}

int pthread_attr_init(void *a)
{
    (void)a;
    return 0;
}

int pthread_attr_destroy(void *a)
{
    (void)a;
    return 0;
}

int pthread_attr_setstacksize(void *a, size_t n)
{
    (void)a;
    (void)n;
    return 0;
}

int pthread_attr_setstack(void *a, void *s, size_t n)
{
    (void)a;
    (void)s;
    (void)n;
    return 0;
}

int pthread_attr_getstack(const void *a, void **s, size_t *n)
{
    (void)a;
    if (s)
        *s = jsc_stack;
    if (n)
        *n = sizeof jsc_stack;
    return 0;
}

int pthread_getattr_np(pthread_t t, void *a)
{
    (void)t;
    (void)a;
    return 0;
}

int pthread_setschedparam(pthread_t t, int pol, const void *param)
{
    (void)t;
    (void)pol;
    (void)param;
    return 0;
}

int sched_get_priority_max(int pol)
{
    (void)pol;
    return 0;
}

int sched_get_priority_min(int pol)
{
    (void)pol;
    return 0;
}

int sched_setscheduler(int pid, int pol, const void *param)
{
    (void)pid;
    (void)pol;
    (void)param;
    return -1;
}

static int mu_held;

int pthread_mutex_init(void *m, const void *attr)
{
    (void)m;
    (void)attr;
    return 0;
}

int pthread_mutex_destroy(void *m)
{
    (void)m;
    return 0;
}

int pthread_mutex_lock(void *m)
{
    (void)m;
    if (mu_held) {
        printf("[jsc] mutex re-lock on the only thread\n");
        abort();
    }
    mu_held = 1;
    return 0;
}

int pthread_mutex_unlock(void *m)
{
    (void)m;
    mu_held = 0;
    return 0;
}

int pthread_cond_init(void *c, const void *attr)
{
    (void)c;
    (void)attr;
    return 0;
}

int pthread_cond_destroy(void *c)
{
    (void)c;
    return 0;
}

int pthread_cond_signal(void *c)
{
    (void)c;
    return 0;
}

int pthread_cond_broadcast(void *c)
{
    (void)c;
    return 0;
}

int pthread_cond_wait(void *c, void *m)
{
    (void)c;
    (void)m;
    printf("[jsc] pthread_cond_wait refused\n");
    return 22;
}

int pthread_cond_timedwait(void *c, void *m, const void *abs)
{
    (void)c;
    (void)m;
    (void)abs;
    return 22;
}

/* Matches glibc's aarch64 struct sigaction: handler, 128-bit-padded mask, flags, restorer. */
struct ksig {
    uint64_t handler;
    uint8_t mask[128];
    uint32_t flags;
    uint32_t pad;
    uint64_t restorer;
};

static struct ksig sig_table[64];

int sigaction(int sig, const void *act, void *old)
{
    if (sig <= 0 || sig >= 64) {
        errno = 22;
        return -1;
    }
    if (old)
        memcpy(old, &sig_table[sig], sizeof(struct ksig));
    if (act)
        memcpy(&sig_table[sig], act, sizeof(struct ksig));
    return 0;
}

int sigemptyset(void *set)
{
    if (set)
        memset(set, 0, 8);
    return 0;
}

int sigfillset(void *set)
{
    if (set)
        memset(set, 0xff, 8);
    return 0;
}

int sigaddset(void *set, int sig)
{
    (void)set;
    (void)sig;
    return 0;
}

int sigdelset(void *set, int sig)
{
    (void)set;
    (void)sig;
    return 0;
}

int sigsuspend(const void *set)
{
    (void)set;
    printf("[jsc] sigsuspend refused\n");
    return -1;
}

int sem_init(void *s, int shared, unsigned val)
{
    (void)s;
    (void)shared;
    (void)val;
    return 0;
}

int sem_post(void *s)
{
    (void)s;
    return 0;
}

int sem_wait(void *s)
{
    (void)s;
    printf("[jsc] sem_wait refused\n");
    return -1;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d == s || !n)
        return dst;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = s;
    while (n--) {
        if (*p == (uint8_t)c)
            return (void *)p;
        p++;
    }
    return 0;
}

void *memrchr(const void *s, int c, size_t n)
{
    const uint8_t *p = (const uint8_t *)s + n;
    while (n--) {
        if (*--p == (uint8_t)c)
            return (void *)p;
    }
    return 0;
}

void *memmem(const void *hay, size_t hn, const void *needle, size_t nn)
{
    if (!nn)
        return (void *)hay;
    if (nn > hn)
        return 0;
    const uint8_t *h = hay;
    for (size_t i = 0; i + nn <= hn; i++) {
        if (!memcmp(h + i, needle, nn))
            return (void *)(h + i);
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    if (!s)
        return 0;
    while (s[n])
        n++;
    return n;
}

char *strchr(const char *s, int c)
{
    if (!s)
        return 0;
    while (*s && *s != (char)c)
        s++;
    return *s == (char)c ? (char *)s : 0;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (!n)
        return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

size_t strnlen(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && s[i])
        i++;
    return i;
}

static int fold(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    return c;
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n && *a && fold(*a) == fold(*b)) {
        a++;
        b++;
        n--;
    }
    if (!n)
        return 0;
    return fold((unsigned char)*a) - fold((unsigned char)*b);
}

int strcasecmp(const char *a, const char *b)
{
    return strncasecmp(a, b, ~(size_t)0);
}

char *strstr(const char *h, const char *n)
{
    size_t ln = strlen(n);
    if (!ln)
        return (char *)h;
    size_t lh = strlen(h);
    return memmem(h, lh, n, ln);
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    uint8_t *b = base;
    uint8_t stack_tmp[128];
    uint8_t *tmp = stack_tmp;
    if (size > sizeof stack_tmp) {
        tmp = malloc(size);
        if (!tmp)
            return;
    }
    for (size_t i = 1; i < n; i++) {
        memcpy(tmp, b + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(b + (j - 1) * size, tmp) > 0) {
            memcpy(b + j * size, b + (j - 1) * size, size);
            j--;
        }
        memcpy(b + j * size, tmp, size);
    }
    if (tmp != stack_tmp)
        free(tmp);
}

/* libstdc++ calls these when an exception would be thrown. Exceptions are off. */
void std_terminate_halt(void)
{
    printf("[jsc] terminate\n");
    abort();
}

void jsc_run_constructors(void)
{
    size_t n = (size_t)(__init_array_end - __init_array_start);
    printf("[jsc] constructors %lu\n", (uint64_t)n);
    for (size_t i = 0; i < n; i++)
        __init_array_start[i]();
}

void jsc_enable_fp_and_tls(void)
{
    uint64_t cpacr;
    asm volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= 3ull << 20;
    asm volatile("msr cpacr_el1, %0; isb" ::"r"(cpacr));
    /* Local-exec offsets are TP + 16 + symbol. Point TP at the TCB. */
    for (char *p = __tls_start; p < __tls_end; p++)
        *p = 0;
    uint64_t tp = (uint64_t)__tls_start - 16;
    asm volatile("msr tpidr_el0, %0" ::"r"(tp));
}

unsigned long __stack_chk_guard = 0x00a5a5a5a5a5a5a5ull;

void __stack_chk_fail(void)
{
    printf("[jsc] stack check failed\n");
    abort();
}

void *__memcpy_chk(void *dst, const void *src, size_t n, size_t destlen)
{
    if (n > destlen)
        abort();
    return memcpy(dst, src, n);
}

char *__strncpy_chk(char *dst, const char *src, size_t n, size_t destlen)
{
    if (n > destlen)
        abort();
    return strncpy(dst, src, n);
}

long __read_chk(int fd, void *buf, size_t n, size_t buflen)
{
    if (n > buflen)
        abort();
    return read(fd, buf, n);
}

char *__cxa_demangle(const char *name, char *out, size_t *len, int *status)
{
    (void)name;
    (void)out;
    (void)len;
    if (status)
        *status = -2;
    return 0;
}

unsigned long __getauxval(unsigned long type)
{
    (void)type;
    return 0;
}

int mincore(void *addr, size_t length, unsigned char *vec)
{
    if (!vec || !length)
        return -1;
    uintptr_t a = (uintptr_t)addr;
    size_t pages = (length + 4095u) / 4096u;
    uintptr_t image = (uintptr_t)__image_start;
    uintptr_t image_end = (uintptr_t)__image_end;
    for (size_t i = 0; i < pages; i++) {
        uintptr_t page = (a + i * 4096u) & ~4095u;
        int in_image = page >= image && page < image_end;
        int in_arena = arena_ready && page >= (uintptr_t)arena && page < arena_limit;
        if (!in_image && !in_arena) {
            errno = 12;
            return -1;
        }
        vec[i] = 1;
    }
    return 0;
}

typedef struct jsc_file {
    int fd;
} FILE;

FILE jsc_stdout = { 1 };
FILE jsc_stderr = { 2 };
FILE *stdout = &jsc_stdout;
FILE *stderr = &jsc_stderr;

FILE *fopen(const char *path, const char *mode)
{
    (void)path;
    (void)mode;
    errno = 2;
    return 0;
}

int fclose(FILE *f)
{
    (void)f;
    return 0;
}

int fflush(FILE *f)
{
    (void)f;
    return 0;
}

size_t fread(void *p, size_t sz, size_t n, FILE *f)
{
    (void)p;
    (void)sz;
    (void)n;
    (void)f;
    return 0;
}

int fseek(FILE *f, long off, int whence)
{
    (void)f;
    (void)off;
    (void)whence;
    return -1;
}

long ftell(FILE *f)
{
    (void)f;
    return -1;
}

char *fgets(char *s, int n, FILE *f)
{
    (void)s;
    (void)n;
    (void)f;
    return 0;
}

int fputs(const char *s, FILE *f)
{
    if (!s)
        return -1;
    return (int)write(f ? f->fd : 1, s, strlen(s));
}

int puts(const char *s)
{
    if (s)
        write(1, s, strlen(s));
    write(1, "\n", 1);
    return 0;
}

int setvbuf(FILE *f, char *buf, int mode, size_t n)
{
    (void)f;
    (void)buf;
    (void)mode;
    (void)n;
    return 0;
}

unsigned long long __isoc23_strtoull(const char *s, char **end, int base)
{
    unsigned long long v = 0;
    if (!s)
        return 0;
    if (base == 0)
        base = 10;
    while (*s >= '0' && *s <= '9')
        v = v * (unsigned)base + (unsigned)(*s++ - '0');
    if (end)
        *end = (char *)s;
    return v;
}

int __isoc23_sscanf(const char *s, const char *fmt, ...)
{
    (void)s;
    (void)fmt;
    return 0;
}

int __isoc23_fscanf(FILE *f, const char *fmt, ...)
{
    (void)f;
    (void)fmt;
    return 0;
}

static void putc_uart(char c)
{
    uart_putc(c);
}

static int kvformat(void (*outc)(char), char *dst, size_t cap, const char *fmt, va_list ap)
{
    size_t n = 0;
    if (!fmt)
        fmt = "";
    while (*fmt) {
        char tmp[64];
        const char *add = 0;
        int addn = 0;
        if (*fmt != '%') {
            tmp[0] = *fmt++;
            add = tmp;
            addn = 1;
        } else {
            fmt++;
            if (*fmt == 'l')
                fmt++;
            switch (*fmt) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                add = s ? s : "(null)";
                addn = (int)strlen(add);
                fmt++;
                break;
            }
            case 'd':
            case 'u':
            case 'x':
            case 'p': {
                int hex = *fmt == 'x' || *fmt == 'p';
                int sig = *fmt == 'd';
                unsigned long v;
                if (*fmt == 'p')
                    v = (unsigned long)va_arg(ap, void *);
                else if (sig) {
                    long s = va_arg(ap, long);
                    if (s < 0) {
                        tmp[0] = '-';
                        add = tmp;
                        addn = 1;
                        v = (unsigned long)(-s);
                    } else
                        v = (unsigned long)s;
                } else
                    v = va_arg(ap, unsigned long);
                char num[32];
                int i = 0;
                if (!v)
                    num[i++] = '0';
                while (v) {
                    unsigned d = (unsigned)(v % (hex ? 16 : 10));
                    num[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                    v /= hex ? 16 : 10;
                }
                int p = 0;
                if (addn == 1)
                    tmp[p++] = '-';
                while (i)
                    tmp[p++] = num[--i];
                tmp[p] = 0;
                add = tmp;
                addn = p;
                fmt++;
                break;
            }
            case 'c':
                tmp[0] = (char)va_arg(ap, int);
                add = tmp;
                addn = 1;
                fmt++;
                break;
            default:
                tmp[0] = *fmt ? *fmt++ : '%';
                add = tmp;
                addn = 1;
                break;
            }
        }
        for (int k = 0; k < addn; k++) {
            if (dst) {
                if (n + 1 < cap)
                    dst[n] = add[k];
            } else if (outc)
                outc(add[k]);
            n++;
        }
    }
    if (dst && cap)
        dst[n < cap ? n : cap - 1] = 0;
    return (int)n;
}

int __fprintf_chk(FILE *f, int flag, const char *fmt, ...)
{
    (void)flag;
    (void)f;
    va_list ap;
    va_start(ap, fmt);
    int n = kvformat(putc_uart, 0, 0, fmt, ap);
    va_end(ap);
    return n;
}

int __printf_chk(int flag, const char *fmt, ...)
{
    (void)flag;
    va_list ap;
    va_start(ap, fmt);
    int n = kvformat(putc_uart, 0, 0, fmt, ap);
    va_end(ap);
    return n;
}

int __vfprintf_chk(FILE *f, int flag, const char *fmt, va_list ap)
{
    (void)f;
    (void)flag;
    return kvformat(putc_uart, 0, 0, fmt, ap);
}

int __vsnprintf_chk(char *s, size_t n, int flag, size_t slen, const char *fmt, va_list ap)
{
    (void)flag;
    if (slen < n)
        abort();
    return kvformat(0, s, n, fmt, ap);
}

int __snprintf_chk(char *s, size_t n, int flag, size_t slen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = __vsnprintf_chk(s, n, flag, slen, fmt, ap);
    va_end(ap);
    return r;
}
