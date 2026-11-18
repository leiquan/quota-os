/*
 * Quota OS implementation of WTF::OSAllocator from WebKit
 * 4d967f4ca7bdd75319e732247e714c2bc634555e.
 * This file is typechecked against that tree. It is not linked into
 * the kernel: the header pulls libstdc++, which this freestanding
 * image does not provide. The kernel calls the same C functions.
 */
#include "wtf/OSAllocator.h"

extern "C" {
void *wk_os_reserve(unsigned long bytes, unsigned long align, int executable, unsigned guard_pages);
void wk_os_release(void *p, unsigned long bytes);
int wk_os_commit(void *p, unsigned long bytes, int executable);
void wk_os_decommit(void *p, unsigned long bytes);
int wk_os_protect(void *p, unsigned long bytes, int readable, int writable);
void wk_os_zero(void *p, unsigned long bytes);
}

namespace WTF {

static void fail()
{
    for (;;)
        ;
}

void *OSAllocator::tryReserveAndCommit(size_t bytes, Usage usage, void *address, bool writable, bool executable, bool jitCageEnabled, unsigned numGuardPagesToAddOnEachEnd)
{
    void *p = tryReserveAndCommitImpl(bytes, usage, address, writable, executable, jitCageEnabled, numGuardPagesToAddOnEachEnd, false);
    return p;
}

void *OSAllocator::tryReserveAndCommitImpl(size_t bytes, Usage, void *address, bool, bool executable, bool, unsigned numGuardPagesToAddOnEachEnd, bool uncommitted)
{
    if (address)
        return nullptr;
    void *p = wk_os_reserve(bytes, 4096, executable ? 1 : 0, numGuardPagesToAddOnEachEnd);
    if (!p)
        return nullptr;
    if (!uncommitted && wk_os_commit(p, bytes, executable ? 1 : 0) != 0) {
        wk_os_release(p, bytes);
        return nullptr;
    }
    return p;
}

void *OSAllocator::tryReserveUncommitted(size_t bytes, Usage, void *address, bool, bool executable, bool, unsigned numGuardPagesToAddOnEachEnd)
{
    if (address)
        return nullptr;
    return wk_os_reserve(bytes, 4096, executable ? 1 : 0, numGuardPagesToAddOnEachEnd);
}

void *OSAllocator::reserveUncommitted(size_t bytes, Usage usage, void *address, bool writable, bool executable, bool jitCageEnabled, unsigned numGuardPagesToAddOnEachEnd)
{
    void *p = tryReserveUncommitted(bytes, usage, address, writable, executable, jitCageEnabled, numGuardPagesToAddOnEachEnd);
    if (!p)
        fail();
    return p;
}

void *OSAllocator::tryReserveUncommittedAligned(size_t bytes, size_t alignment, Usage, void *address, bool, bool executable, bool, unsigned numGuardPagesToAddOnEachEnd)
{
    if (address)
        return nullptr;
    if (alignment < 4096)
        alignment = 4096;
    return wk_os_reserve(bytes, alignment, executable ? 1 : 0, numGuardPagesToAddOnEachEnd);
}

void *OSAllocator::reserveAndCommit(size_t bytes, Usage usage, void *address, bool writable, bool executable, bool jitCageEnabled, unsigned numGuardPagesToAddOnEachEnd)
{
    void *p = tryReserveAndCommit(bytes, usage, address, writable, executable, jitCageEnabled, numGuardPagesToAddOnEachEnd);
    if (!p)
        fail();
    return p;
}

void OSAllocator::commit(void *address, size_t bytes, bool, bool executable)
{
    if (wk_os_commit(address, bytes, executable ? 1 : 0) != 0)
        fail();
}

void OSAllocator::decommit(void *address, size_t bytes)
{
    wk_os_decommit(address, bytes);
}

void OSAllocator::hintMemoryNotNeededSoon(void *, size_t)
{
}

void OSAllocator::releaseDecommitted(void *address, size_t bytes, unsigned)
{
    wk_os_release(address, bytes);
}

bool OSAllocator::tryProtect(void *address, size_t bytes, bool readable, bool writable)
{
    return wk_os_protect(address, bytes, readable ? 1 : 0, writable ? 1 : 0) == 0;
}

void OSAllocator::protect(void *address, size_t bytes, bool readable, bool writable)
{
    if (!tryProtect(address, bytes, readable, writable))
        fail();
}

void OSAllocator::zeroFill(void *base, size_t size)
{
    wk_os_zero(base, size);
}

}
