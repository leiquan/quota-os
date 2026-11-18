/* Freestanding C++ runtime for Quota OS. Not libstdc++. Not WebKit. */
#include "quotaos.h"

struct CxxBlk {
    uint32_t size;
    uint32_t free;
    CxxBlk *next;
};

static uint8_t cxx_heap[65536] __attribute__((aligned(16)));
static CxxBlk *cxx_free;
static int cxx_ready;

static void cxx_init(void)
{
    if (cxx_ready)
        return;
    cxx_free = (CxxBlk *)cxx_heap;
    cxx_free->size = sizeof cxx_heap - sizeof(CxxBlk);
    cxx_free->free = 1;
    cxx_free->next = 0;
    cxx_ready = 1;
}

void *operator new(unsigned long n)
{
    cxx_init();
    n = (n + 15u) & ~15u;
    for (CxxBlk *b = cxx_free; b; b = b->next) {
        if (!b->free || b->size < n)
            continue;
        if (b->size >= n + sizeof(CxxBlk) + 16) {
            CxxBlk *rest = (CxxBlk *)((uint8_t *)(b + 1) + n);
            rest->size = b->size - n - sizeof(CxxBlk);
            rest->free = 1;
            rest->next = b->next;
            b->size = n;
            b->next = rest;
        }
        b->free = 0;
        return b + 1;
    }
    printf("[cxx] heap exhausted\n");
    for (;;)
        asm volatile("wfi");
}

void *operator new[](unsigned long n)
{
    return operator new(n);
}

static void cxx_free_block(void *p)
{
    if (!p)
        return;
    CxxBlk *b = (CxxBlk *)p - 1;
    b->free = 1;
    for (CxxBlk *c = cxx_free; c && c->next; c = c->next) {
        uint8_t *end = (uint8_t *)(c + 1) + c->size;
        if (c->free && c->next->free && end == (uint8_t *)c->next) {
            c->size += sizeof(CxxBlk) + c->next->size;
            c->next = c->next->next;
        }
    }
}

void operator delete(void *p) noexcept
{
    cxx_free_block(p);
}

void operator delete(void *p, unsigned long) noexcept
{
    cxx_free_block(p);
}

void operator delete[](void *p) noexcept
{
    cxx_free_block(p);
}

void operator delete[](void *p, unsigned long) noexcept
{
    cxx_free_block(p);
}

extern "C" void __cxa_pure_virtual(void)
{
    printf("[cxx] pure virtual call\n");
    for (;;)
        asm volatile("wfi");
}
