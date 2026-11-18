/*
 * EL1 identity map. One kernel address space. Page permissions can change.
 * This is not a set of protected user processes, and it is not completed
 * three-tier isolation. Device pages stay device memory.
 *
 * 4 KB pages cover the kernel data, the system-reserve tail (framebuffer),
 * and the WebKit structure/arena window. Everything else in RAM is a 2 MB
 * block, so the page tables leave room for the framebuffer.
 */
#include "quotaos.h"

#define PTE_VALID  1ull
#define PTE_TABLE  2ull
#define PTE_AF     (1ull << 10)
#define PTE_PXN    (1ull << 53)
#define PTE_UXN    (1ull << 54)
#define PTE_NG     (1ull << 11)
#define ATTR_NORMAL 0ull
#define ATTR_DEVICE (1ull << 2)
#define AP_RO      (1ull << 7)
#define SH_INNER   (3ull << 8)

static uint64_t l0[512] __attribute__((aligned(4096)));
static uint64_t l1[512] __attribute__((aligned(4096)));
static uint64_t l2_lo[512] __attribute__((aligned(4096)));
static uint64_t l2_ram[512] __attribute__((aligned(4096)));
/* Blocks 6-7 and 32-63. 34 tables, not 64. */
static uint64_t l3_storage[34][512] __attribute__((aligned(4096)));
static uint64_t *l3_for[64];
static int mmu_on;
static volatile int expect_fault;
static volatile int fault_hit;
static uint64_t kernel_ttbr_value;

uint64_t mmu_kernel_ttbr(void)
{
    return kernel_ttbr_value;
}

uint64_t mmu_l2_ram_phys(void)
{
    return (uint64_t)l2_ram;
}

int mmu_fault_expected(void)
{
    return expect_fault;
}

void mmu_note_fault(void)
{
    fault_hit = 1;
    expect_fault = 0;
}

static uint64_t ram_pte(int ro, int present)
{
    if (!present)
        return 0;
    /* Bit 1 must be 1 for a level-3 page descriptor. RAM stays executable. */
    uint64_t p = PTE_VALID | PTE_TABLE | PTE_AF | ATTR_NORMAL | SH_INNER | PTE_UXN;
    if (ro)
        p |= AP_RO;
    return p;
}

static void tlb_page(uint64_t va)
{
    asm volatile("tlbi vae1, %0" :: "r"(va >> 12) : "memory");
}

int mmu_protect(uint64_t addr, uint64_t len, int prot)
{
    if (!mmu_on)
        return -1;
    if (addr < 0x40000000ull || len == 0)
        return -1;
    uint64_t end = addr + len;
    if (end > 0x48000000ull)
        return -1;
    int ro = (prot & 2) == 0;
    int present = prot != 0;
    for (uint64_t va = addr & ~4095ull; va < end; va += 4096) {
        uint64_t blk = (va - 0x40000000ull) >> 21;
        uint64_t l3i = (va >> 12) & 511ull;
        if (blk >= 64 || !l3_for[blk])
            return -1;
        uint64_t *pte = &l3_for[blk][l3i];
        *pte = (va & ~4095ull) | ram_pte(ro, present);
        asm volatile("dc civac, %0" :: "r"(pte) : "memory");
        asm volatile("dsb ish" ::: "memory");
        tlb_page(va);
    }
    asm volatile("dsb ish; isb" ::: "memory");
    return 0;
}

void mmu_init(void)
{
    for (int i = 0; i < 512; i++) {
        l0[i] = 0;
        l1[i] = 0;
        l2_lo[i] = 0;
        l2_ram[i] = 0;
    }
    /* First GiB: 2 MB device blocks for the QEMU virt MMIO window. */
    for (uint64_t va = 0x08000000ull; va < 0x0c000000ull; va += 0x200000ull) {
        uint64_t l2i = (va >> 21) & 511ull;
        l2_lo[l2i] = va | PTE_VALID | PTE_AF | ATTR_DEVICE | PTE_PXN | PTE_UXN;
    }
    l1[0] = (uint64_t)l2_lo | PTE_VALID | PTE_TABLE;
    int tables = 0;
    for (int blk = 0; blk < 64; blk++) {
        uint64_t va = 0x40000000ull + ((uint64_t)blk << 21);
        uint64_t l2i = (va >> 21) & 511ull;
        /* Data, reserve tail, structure heap, and malloc arena. */
        int pages = (blk >= 6 && blk <= 7) || blk >= 32;
        if (!pages) {
            l2_ram[l2i] = va | PTE_VALID | PTE_AF | ATTR_NORMAL | SH_INNER | PTE_UXN;
            l3_for[blk] = 0;
            continue;
        }
        if (tables >= 34) {
            printf("[mmu] page-table pool exhausted\n");
            for (;;)
                asm volatile("wfi");
        }
        uint64_t *table = l3_storage[tables++];
        for (int p = 0; p < 512; p++)
            table[p] = (va + ((uint64_t)p << 12)) | ram_pte(0, 1);
        l2_ram[l2i] = (uint64_t)table | PTE_VALID | PTE_TABLE;
        l3_for[blk] = table;
    }
    l1[1] = (uint64_t)l2_ram | PTE_VALID | PTE_TABLE;
    l0[0] = (uint64_t)l1 | PTE_VALID | PTE_TABLE;

    printf("[mmu] tables ready\n");
    uint64_t mair = 0xFFull | (0x00ull << 8); /* normal WB, device nGnRnE */
    uint64_t tcr = 16ull | (1ull << 8) | (1ull << 10) | (3ull << 12) | (1ull << 23) | (1ull << 32);
    asm volatile("msr mair_el1, %0" :: "r"(mair));
    asm volatile("msr tcr_el1, %0" :: "r"(tcr));
    kernel_ttbr_value = (uint64_t)l0;
    asm volatile("msr ttbr0_el1, %0" :: "r"(kernel_ttbr_value));
    asm volatile("dsb ish; tlbi vmalle1; dsb ish; isb" ::: "memory");
    uint64_t sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ull << 0) | (1ull << 2) | (1ull << 12);
    asm volatile("msr sctlr_el1, %0; isb" :: "r"(sctlr) : "memory");
    mmu_on = 1;
    printf("[mmu] identity map on, EL1. RAM 0x40000000-0x48000000, devices 0x08000000-0x0c000000\n");
    printf("[mmu] kernel map is EL1-only. EL0 apps get a separate TTBR0. This is not a certification.\n");

    /* One aligned page inside RAM, not the last byte. Prove the PTE bits, then restore RW. */
    volatile uint32_t *page = (volatile uint32_t *)0x47ff0000ull;
    page[0] = 0x11111111u;
    asm volatile("dsb ish" ::: "memory");
    mmu_protect(0x47ff0000ull, 4096, 0);
    fault_hit = 0;
    expect_fault = 1;
    uint32_t got = 0xdeadbeefu;
    asm volatile("ldr %w0, [%1]" : "=r"(got) : "r"(page) : "memory");
    int none_hit = fault_hit;
    uint32_t none_got = got;
    expect_fault = 0;
    /* A fault skips the load, so the pattern must not come back. */
    int none_fault = none_hit && none_got != 0x11111111u;
    mmu_protect(0x47ff0000ull, 4096, 1);
    fault_hit = 0;
    expect_fault = 1;
    got = 0;
    asm volatile("ldr %w0, [%1]" : "=r"(got) : "r"(page) : "memory");
    int read_ok = !fault_hit && got == 0x11111111u;
    asm volatile("str %w0, [%1]" :: "r"(0x22222222u), "r"(page) : "memory");
    int write_fault = fault_hit && page[0] == 0x11111111u;
    expect_fault = 0;
    mmu_protect(0x47ff0000ull, 4096, 1 | 2);
    fault_hit = 0;
    page[0] = 0x33333333u;
    int rw_ok = !fault_hit && page[0] == 0x33333333u;
    printf("[mmu] protect none-fault=%d read=%d write-fault=%d rw=%d\n",
           none_fault, read_ok, write_fault, rw_ok);
}
