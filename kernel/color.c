/*
 * E4：缓存着色。物理页按缓存颜色分配：每个 Tier 1 任务独占自己的颜色，
 * 别人（Tier 2/3、演示里的申请）只从其余颜色拿页。
 *
 * 颜色模型：按 2 MB、16 路组相联的末级缓存、4 KB 页算，颜色数 = 2 MB / (16 * 4 KB) = 32，
 * 颜色 = (物理地址 >> 12) & 31。QEMU 不模拟缓存，这里只能验证分配规则，
 * 实际隔离效果要在真实板子上量（缺真实板子）。
 *
 * 页池 = RAM 里系统预留 16 MB 之后的部分。一个 Tier 1 任务需要的颜色数 = 向上取整
 * （它的内存 / 每种颜色在页池里的容量）；这些颜色里它用不完的页别人也不能用，
 * 算作内存的「保护量」（着色补齐），准入时扣掉。
 */
#include "quotaos.h"

#define NCOLORS 32
#define PAGE 4096ull
#define MAX_T1 8
#define MAX_T1_PAGES 4096
#define MAX_SHARED_PAGES 1024

extern char __image_start[];

static uint64_t pool_base, pool_pages, per_color;
static int color_owner[NCOLORS];        /* -1 共享，否则 Tier 1 表里的下标 */
static uint32_t t1_pages[MAX_T1_PAGES]; /* 页号（相对 pool_base） */
static uint8_t t1_who[MAX_T1_PAGES];
static uint32_t n_t1;
static uint64_t pad_kb;
static uint32_t t1_colors_used;
static uint32_t shared_next[NCOLORS];   /* 每种共享颜色下一个没发出去的序号 */
static uint32_t shared_pages[MAX_SHARED_PAGES];
static uint32_t n_shared;

static uint32_t color_of(uint64_t pa)
{
    return (uint32_t)((pa >> 12) & (NCOLORS - 1));
}

/* 页池里颜色 c 的第 k 页（k 从 0 起）的页号。 */
static uint64_t nth_page(uint32_t c, uint64_t k)
{
    uint64_t first = (c - color_of(pool_base) + NCOLORS) % NCOLORS;
    return first + k * NCOLORS;
}

void color_pool_range(uint64_t *start, uint64_t *end)
{
    if (start)
        *start = pool_base;
    if (end)
        *end = pool_base + pool_pages * 4096;
}

void color_init(uint64_t ram_bytes)
{
    uint64_t base = (uint64_t)__image_start & ~((16ull << 20) - 1);
    pool_base = base + (uint64_t)system_reserve_kb() * 1024;
    uint64_t structure = webkit_structure_base();
    uint64_t end = structure ? structure : base + ram_bytes;
    pool_pages = end > pool_base ? (end - pool_base) / PAGE : 0;
    per_color = pool_pages / NCOLORS;
    for (int c = 0; c < NCOLORS; c++)
        color_owner[c] = -1;
    int n = 0;
    const struct tier1_decl *d = resource_tier1(&n);
    uint32_t next = 0;
    n_t1 = 0;
    pad_kb = 0;
    for (int i = 0; i < n && i < MAX_T1; i++) {
        uint64_t need = (uint64_t)d[i].mem_kb * 1024 / PAGE;
        uint64_t nc = per_color ? (need + per_color - 1) / per_color : 0;
        if (next + nc > NCOLORS)
            break;
        for (uint64_t k = 0; k < nc; k++)
            color_owner[next + k] = i;
        /* 依次从这几种颜色里拿页，并在页首写下主人编号（真的用到这些物理页） */
        for (uint64_t p = 0; p < need && n_t1 < MAX_T1_PAGES; p++) {
            uint32_t c = next + (uint32_t)(p % nc);
            uint64_t pg = nth_page(c, p / nc);
            t1_pages[n_t1] = (uint32_t)pg;
            t1_who[n_t1] = (uint8_t)i;
            *(volatile uint32_t *)(pool_base + pg * PAGE) = 0x51000000u | (uint32_t)i;
            n_t1++;
        }
        pad_kb += (nc * per_color - need) * PAGE / 1024;
        next += (uint32_t)nc;
    }
    t1_colors_used = next;
    memset(shared_next, 0, sizeof shared_next);
    n_shared = 0;
    printf("[boot] cache colors: %d (model: 2 MB 16-way cache, 4 KB pages);\n", NCOLORS);
    printf("       tier-1 pages in %u own colors, padding %lu KB\n", t1_colors_used, pad_kb);
}

/* 着色补齐：Tier 1 颜色里 Tier 1 用不完、别人也不能用的内存。 */
uint64_t color_padding_kb(void)
{
    return pad_kb;
}

/* 从共享颜色里拿 n 页（演示用），轮流取各颜色。返回拿到的页数。 */
uint32_t color_alloc_shared(uint32_t n)
{
    uint32_t got = 0, c = 0;
    while (got < n && n_shared < MAX_SHARED_PAGES) {
        for (int tries = 0; tries < NCOLORS && color_owner[c] >= 0; tries++)
            c = (c + 1) % NCOLORS;
        if (color_owner[c] >= 0 || shared_next[c] >= per_color)
            break;
        uint64_t pg = nth_page(c, shared_next[c]++);
        shared_pages[n_shared++] = (uint32_t)pg;
        *(volatile uint32_t *)(pool_base + pg * PAGE) = 0x52000000u;
        got++;
        c = (c + 1) % NCOLORS;
    }
    return got;
}

void color_free_shared(void)
{
    n_shared = 0;
    memset(shared_next, 0, sizeof shared_next);
}

/* 第 10 项：每个 Tier 1 页的颜色属于它的主人；Tier 1 颜色互不相交；
 * 共享页没有一个落在 Tier 1 颜色里；页首写的主人编号还在。 */
void color_report(int qa_line)
{
    int n = 0;
    const struct tier1_decl *d = resource_tier1(&n);
    printf("page pool 0x%lx, %lu pages, %lu per color\n", pool_base, pool_pages, per_color);
    printf("  tier-1 task      mem_kb colors  pages\n");
    for (int i = 0; i < n && i < MAX_T1; i++) {
        int first = -1, cnt = 0;
        for (int c = 0; c < NCOLORS; c++)
            if (color_owner[c] == i) {
                if (first < 0)
                    first = c;
                cnt++;
            }
        uint32_t pages = 0;
        for (uint32_t k = 0; k < n_t1; k++)
            pages += t1_who[k] == i;
        printf("  ");
        put_padded(d[i].name, 14);
        printf(" %8u %3d-%-3d %6u\n", d[i].mem_kb, first, first + cnt - 1, pages);
    }
    uint32_t right = 0, marked = 0, shared_bad = 0;
    for (uint32_t k = 0; k < n_t1; k++) {
        uint64_t pa = pool_base + (uint64_t)t1_pages[k] * PAGE;
        if (color_owner[color_of(pa)] == t1_who[k])
            right++;
        if (*(volatile uint32_t *)pa == (0x51000000u | t1_who[k]))
            marked++;
    }
    for (uint32_t k = 0; k < n_shared; k++)
        if (color_owner[color_of(pool_base + (uint64_t)shared_pages[k] * PAGE)] >= 0)
            shared_bad++;
    printf("tier-1 pages %u: color right %u, owner mark intact %u\n", n_t1, right, marked);
    printf("shared pages handed out %u: in a tier-1 color %u\n", n_shared, shared_bad);
    printf("colors: %u tier-1 (one owner each), %u shared\n", t1_colors_used,
           NCOLORS - t1_colors_used);
    printf("isolation effect on a real cache: 缺真实板子 (QEMU has no cache model)\n");
    if (qa_line)
        printf("E4 item10 %s\n", n_t1 > 0 && right == n_t1 && marked == n_t1 && n_shared > 0 &&
                                      shared_bad == 0 ? "PASS" : "FAIL");
}
