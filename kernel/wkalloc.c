/*
 * Kernel side of WTF::OSAllocator (reserve / commit, guard pages,
 * executable pages). WebKit is not linked. These calls only record
 * what this CPU can and cannot do.
 *
 * This allocator refuses executable pages and guard pages. JSC calls
 * mprotect for those. Ordinary bytes come from the 16 MB system
 * reserve, under the page pool, so they are not Tier-1 colored pages
 * and they are not lent to an application admission slot.
 */
#include "quotaos.h"

int wk_reserve(uint64_t bytes, int executable, unsigned guard_pages, uint64_t *pa)
{
    if (executable) {
        printf("[wk] refuse executable %lu bytes\n", bytes);
        return -1;
    }
    if (guard_pages) {
        printf("[wk] refuse %u guard pages\n", guard_pages);
        return -1;
    }
    if (kernel_reserve_alloc(bytes, 4096, pa) != 0) {
        printf("[wk] system reserve has no room for %lu bytes\n", bytes);
        return -1;
    }
    printf("[wk] reserved %lu bytes at 0x%lx in system reserve\n", bytes, *pa);
    return 0;
}

struct wk_span {
    uint64_t pa;
    uint64_t bytes;
    int live;
    int committed;
};

static struct wk_span spans[8];

static struct wk_span *span_of(void *p)
{
    uint64_t a = (uint64_t)p;
    for (int i = 0; i < 8; i++)
        if (spans[i].live && spans[i].pa == a)
            return &spans[i];
    return 0;
}

void *wk_os_reserve(uint64_t bytes, uint64_t align, int executable, unsigned guard_pages)
{
    uint64_t pa;
    if (executable || guard_pages)
        return 0;
    if (!align)
        align = 4096;
    for (int i = 0; i < 8; i++) {
        if (spans[i].live)
            continue;
        if (kernel_reserve_alloc(bytes, align, &pa) != 0)
            return 0;
        spans[i].pa = pa;
        spans[i].bytes = bytes;
        spans[i].live = 1;
        spans[i].committed = 0;
        return (void *)pa;
    }
    return 0;
}

void wk_os_release(void *p, uint64_t bytes)
{
    struct wk_span *s = span_of(p);
    (void)bytes;
    if (s)
        s->live = 0;
}

int wk_os_commit(void *p, uint64_t bytes, int executable)
{
    struct wk_span *s = span_of(p);
    if (!s || executable || bytes > s->bytes)
        return -1;
    memset((void *)s->pa, 0, bytes);
    s->committed = 1;
    return 0;
}

void wk_os_decommit(void *p, uint64_t bytes)
{
    struct wk_span *s = span_of(p);
    (void)bytes;
    if (s)
        s->committed = 0;
}

int wk_os_protect(void *p, uint64_t bytes, int readable, int writable)
{
    (void)p;
    (void)bytes;
    /* No MMU: pages stay readable and writable. Any other mode is refused. */
    if (readable && writable)
        return 0;
    return -1;
}

void wk_os_zero(void *p, uint64_t bytes)
{
    memset(p, 0, bytes);
}
