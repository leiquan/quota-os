/*
 * 资源账本：系统预留、Tier 1、Tier 2 的最低值和建议值。
 *
 * 系统预留（内核）：CPU SYS_RESERVE_US / 周期、内存 KERNEL_RESERVE_KB，排在 Tier 1 前面，不外借。
 * Tier 1：厂商在编译映像时写死，每个任务一个固定值，开机一次性划走。
 * Tier 2：应用清单里的最低值和建议值。准入只看最低值：
 *     系统预留 + Tier 1 + ΣTier 2 最低值 <= 100%
 *   放不下就拒绝，并说明关掉哪个应用就能放下；比整机能给应用的还多，就是设备不满足最低要求。
 *   建议值只从空闲里给，有压力时收回。
 * Tier 3：没声明，分剩下的。
 */
#include "quotaos.h"

/* 槽号按这个顺序分：CPU 不为 0 的依次是 0、1、2、3。 */
static const struct tier1_decl tier1_table[] = {
    {"display-vsync", "显示提交，从上电就开始跑", 500, 1000, 2048},
    {"timer-server", "周期时钟，从上电就开始跑", 300, 3500, 1024},
    {"gic-server", "中断路由，只保留槽（空槽）", 300, 6000, 2048},
    {"blk-driver", "存储驱动的槽（失控演示借它）", 400, 8000, 4096},
    {"net-driver", "网络驱动，只占内存", 0, 0, 4096},
};

static const struct app_manifest catalog[] = {
    {"Pixel Racer", "游戏", 8 * 1024, 20 * 1024, 3000, 1000},
    {"Cinema Play", "视频播放", 8 * 1024, 16 * 1024, 3000, 500},
    {"Studio Cut", "剪辑", 8 * 1024, 24 * 1024, 3000, 1000},
    {"Neural Forge", "最低 80MB，小内存机器装不下", 80 * 1024, 96 * 1024, 1000, 0},
    {"Cluster Sim", "最低 512MB，超过这台机器", 512 * 1024, 512 * 1024, 1000, 0},
    {"Render Farm", "最低 CPU 9000us，超过整机能给的", 4 * 1024, 4 * 1024, 9000, 0},
};

static struct running_app apps[MAX_APPS];
static uint64_t ram_kb;
static int pressure;
static char reason[REASON_LEN];

static void radd(const char *s)
{
    int n = 0;
    while (reason[n])
        n++;
    while (*s && n < REASON_LEN - 1)
        reason[n++] = *s++;
    reason[n] = 0;
}

static void radd_u(uint64_t v)
{
    char tmp[24];
    int i = 0;
    do {
        tmp[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    char out[24];
    int n = 0;
    while (i)
        out[n++] = tmp[--i];
    out[n] = 0;
    radd(out);
}

const struct tier1_decl *resource_tier1(int *count)
{
    *count = (int)(sizeof tier1_table / sizeof tier1_table[0]);
    return tier1_table;
}

uint32_t resource_t1_cpu_us(void)
{
    uint32_t s = 0;
    for (unsigned i = 0; i < sizeof tier1_table / sizeof tier1_table[0]; i++)
        s += tier1_table[i].cpu_us;
    return s;
}

/* 每个 Tier 1 槽前有 GUARD_US 的保护时间，这段时间窗口任务不能用，准入时要扣掉 */
uint32_t resource_guard_us(void)
{
    uint32_t s = 0;
    for (unsigned i = 0; i < sizeof tier1_table / sizeof tier1_table[0]; i++)
        if (tier1_table[i].cpu_us)
            s += GUARD_US;
    return s;
}

uint32_t resource_t1_mem_kb(void)
{
    uint32_t s = 0;
    for (unsigned i = 0; i < sizeof tier1_table / sizeof tier1_table[0]; i++)
        s += tier1_table[i].mem_kb;
    return s;
}

uint64_t resource_ram_kb(void)
{
    return ram_kb;
}

static int boot_is_webkit(void)
{
    const char *args = dtb_bootargs();
    return args[0] == 'w' && strcmp(args, "webkit") == 0;
}

/* The kernel image stays in the 16 MB reserve on every boot. webkit mode
 * also keeps a 64 MB structure-heap window at the top of RAM, because
 * JavaScriptCore's StructureID encoding needs a power-of-two reservation
 * aligned to its own size. The smallest size that encoder still accepts
 * is a 32 MB heap, which the POSIX allocator maps as 64 MB so it can align
 * the result. Tier-1 declarations do not change. */
uint32_t system_reserve_kb(void)
{
    extern char __image_start[], __image_end[];
    uint64_t base = (uint64_t)__image_start & ~((16ull << 20) - 1);
    /* Image plus the 640x480 surface. Stay at 16 MB when that already fits. */
    uint64_t need = ((uint64_t)__image_end - base) + (640ull * 480ull * 4ull);
    uint32_t kb = (uint32_t)((need + 1023ull) / 1024ull);
    if (kb < KERNEL_RESERVE_KB)
        kb = KERNEL_RESERVE_KB;
    /* 2 MB alignment keeps the colored pool on a block boundary. */
    kb = (kb + 2047u) & ~2047u;
    return kb;
}

uint64_t webkit_structure_bytes(void)
{
    return boot_is_webkit() ? (64ull << 20) : 0;
}

uint64_t webkit_structure_base(void)
{
    if (!webkit_structure_bytes())
        return 0;
    extern char __image_start[];
    uint64_t base = (uint64_t)__image_start & ~((16ull << 20) - 1);
    return base + ram_kb * 1024 - webkit_structure_bytes();
}

uint64_t resource_app_mem_capacity_kb(void)
{
    /* v012 起再扣掉缓存着色补齐（Tier 1 颜色里别人不能用的页） */
    uint64_t fixed = (uint64_t)system_reserve_kb() + webkit_structure_bytes() / 1024
        + resource_t1_mem_kb() + color_padding_kb();
    return ram_kb > fixed ? ram_kb - fixed : 0;
}

uint32_t resource_app_cpu_capacity_us(void)
{
    uint32_t fixed = SYS_RESERVE_US + resource_t1_cpu_us() + resource_guard_us();
    return PERIOD_US > fixed ? PERIOD_US - fixed : 0;
}

uint32_t resource_cpu_min_sum_us(void)
{
    uint32_t s = 0;
    for (int i = 0; i < MAX_APPS; i++)
        if (apps[i].alive)
            s += apps[i].cpu_min_us;
    return s;
}

uint64_t resource_mem_min_sum_kb(void)
{
    uint64_t s = 0;
    for (int i = 0; i < MAX_APPS; i++)
        if (apps[i].alive)
            s += apps[i].mem_min_kb;
    return s;
}

static uint64_t sum_granted_kb(void)
{
    uint64_t s = 0;
    for (int i = 0; i < MAX_APPS; i++)
        if (apps[i].alive)
            s += apps[i].mem_granted_kb;
    return s;
}

void resource_init(uint64_t ram_bytes)
{
    memset(apps, 0, sizeof apps);
    ram_kb = ram_bytes / 1024;
    pressure = 0;
    reason[0] = 0;
    uint32_t reserve_kb = system_reserve_kb();
    printf("[boot] system reserve %u us/period, %u KB;\n       tier-1 %u us/period, %u KB; guard %u us/period\n",
           SYS_RESERVE_US, reserve_kb, resource_t1_cpu_us(), resource_t1_mem_kb(),
           resource_guard_us());
    if (reserve_kb > KERNEL_RESERVE_KB)
        printf("[boot] system reserve grew from %u KB to %u KB to hold the image and framebuffer; tier-1 declarations unchanged\n",
               KERNEL_RESERVE_KB, reserve_kb);
    if (webkit_structure_bytes())
        printf("[boot] webkit admission: structure reservation %lu KB is inside the 128 MB budget; system reserve %u KB includes the framebuffer; tier-1 declarations unchanged (%u KB, %u us)\n",
               webkit_structure_bytes() / 1024, reserve_kb, resource_t1_mem_kb(), resource_t1_cpu_us());
    printf("[boot] EL1 map is the kernel map. EL0 apps use a separate TTBR0. Tier-1 realtime is not claimed here.\n");
}

static int ranges_overlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1)
{
    return a1 > a0 && b1 > b0 && a0 < b1 && b0 < a1;
}

void boot_check_reservations(uint64_t structure, uint64_t window, uint64_t arena, uint64_t arena_end)
{
    extern char __image_start[], __image_end[];
    uint64_t image0 = (uint64_t)__image_start;
    uint64_t image1 = (uint64_t)__image_end;
    uint64_t pool0 = 0, pool1 = 0, fb0 = 0, fb_n = 0;
    color_pool_range(&pool0, &pool1);
    int have_fb = fb_reserved_range(&fb0, &fb_n) == 0;
    uint64_t fb1 = fb0 + fb_n;
    uint64_t parent1 = structure + window;
    uint64_t heap1 = structure + window / 2;
    /* The 64 MB window is one parent reservation. The structure heap and the
     * malloc arena are its two children. The parent contains the arena, so a
     * check of those two printed ranges is not an overlap failure. */
    int siblings = !ranges_overlap(structure, heap1, arena, arena_end)
        && arena == heap1 && arena_end == parent1;
    int bad = !siblings;
    if (ranges_overlap(image0, image1, pool0, pool1))
        bad = 1;
    if (ranges_overlap(image0, image1, structure, parent1))
        bad = 1;
    if (ranges_overlap(pool0, pool1, structure, parent1))
        bad = 1;
    if (have_fb && (ranges_overlap(image0, image1, fb0, fb1) || ranges_overlap(fb0, fb1, pool0, pool1)
            || ranges_overlap(fb0, fb1, structure, parent1) || fb0 < image1 || fb1 > pool0))
        bad = 1;
    printf("[boot] independent image 0x%lx-0x%lx fb 0x%lx-0x%lx pool 0x%lx-0x%lx parent 0x%lx-0x%lx\n",
           image0, image1, have_fb ? fb0 : 0, have_fb ? fb1 : 0, pool0, pool1, structure, parent1);
    printf("[boot] parent children structure-heap 0x%lx-0x%lx arena 0x%lx-0x%lx disjoint %d\n",
           structure, heap1, arena, arena_end, siblings);
    printf("[boot] reservation bad %d; framebuffer %lu KB stays inside the %u KB system reserve\n",
           bad, have_fb ? fb_n / 1024 : 0, system_reserve_kb());
}

int resource_pressure(void)
{
    return pressure;
}

/* 有压力：建议值的内存收回到最低值，CPU 建议值停发（调度器看这个标志）。 */
void resource_set_pressure(int on)
{
    pressure = on ? 1 : 0;
    if (!pressure)
        return;
    uint64_t got = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        if (apps[i].alive && apps[i].mem_granted_kb > apps[i].mem_min_kb) {
            got += apps[i].mem_granted_kb - apps[i].mem_min_kb;
            apps[i].mem_granted_kb = apps[i].mem_min_kb;
        }
    }
    printf("pressure ON: reclaimed %lu KB of suggested memory; minimums untouched.\n", got);
}

const char *resource_reason(void)
{
    return reason;
}

const struct app_manifest *resource_catalog(int *count)
{
    *count = (int)(sizeof catalog / sizeof catalog[0]);
    return catalog;
}

int resource_running_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_APPS; i++)
        n += apps[i].alive;
    return n;
}

struct running_app *resource_running(int index)
{
    int n = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        if (!apps[i].alive)
            continue;
        if (n++ == index)
            return &apps[i];
    }
    return 0;
}

int resource_check(const struct app_manifest *m)
{
    reason[0] = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        if (apps[i].alive && strcmp(apps[i].name, m->name) == 0) {
            radd("REFUSED: ");
            radd(m->name);
            radd(" is already running\n");
            return ADMIT_DUP;
        }
    }

    uint64_t mem_cap = resource_app_mem_capacity_kb();
    uint32_t cpu_cap = resource_app_cpu_capacity_us();
    int mem_over = m->mem_min_kb > mem_cap;
    int cpu_over = m->cpu_min_us > cpu_cap;
    if (mem_over || cpu_over) {
        radd("REFUSED: device does not meet minimum requirements\n");
        if (mem_over) {
            radd("  minimum memory ");
            radd_u(m->mem_min_kb);
            radd(" KB > ");
            radd_u(mem_cap);
            radd(" KB for apps\n  (RAM ");
            radd_u(ram_kb);
            radd(" KB - system reserve - tier-1 - color padding)\n");
        }
        if (cpu_over) {
            radd("  minimum CPU ");
            radd_u(m->cpu_min_us);
            radd(" us > ");
            radd_u(cpu_cap);
            radd(" us/period for apps\n  (period - system reserve - tier-1 - guard)\n");
        }
        radd("No app can be closed to make room: this device is too small.\n");
        return ADMIT_DEVICE;
    }

    uint64_t mem_left = mem_cap - resource_mem_min_sum_kb();
    uint32_t cpu_left = cpu_cap - resource_cpu_min_sum_us();
    if (m->mem_min_kb <= mem_left && m->cpu_min_us <= cpu_left)
        return ADMIT_OK;

    /* 关掉哪一个就能放下：挑最低值最小的那个，少打扰别人。 */
    int best = -1;
    for (int i = 0; i < MAX_APPS; i++) {
        if (!apps[i].alive)
            continue;
        if (m->mem_min_kb > mem_left + apps[i].mem_min_kb ||
            m->cpu_min_us > cpu_left + apps[i].cpu_min_us)
            continue;
        if (best < 0 || apps[i].cpu_min_us < apps[best].cpu_min_us)
            best = i;
    }
    radd("REFUSED: not enough resources for ");
    radd(m->name);
    radd("\n  CPU left ");
    radd_u(cpu_left);
    radd(" us/period, minimum needs ");
    radd_u(m->cpu_min_us);
    radd("\n  memory left ");
    radd_u(mem_left);
    radd(" KB, minimum needs ");
    radd_u(m->mem_min_kb);
    radd("\n");
    if (best >= 0) {
        radd("Please close ");
        radd(apps[best].name);
        radd(" first (frees ");
        radd_u(apps[best].cpu_min_us);
        radd(" us/period, ");
        radd_u(apps[best].mem_min_kb);
        radd(" KB).\n");
    } else {
        radd("Closing any single app is not enough; close several apps first.\n");
    }
    return ADMIT_OVERSUB;
}

struct running_app *resource_commit(const struct app_manifest *m)
{
    if (resource_check(m) != ADMIT_OK)
        return 0;
    int slot = -1;
    for (int i = 0; i < MAX_APPS && slot < 0; i++)
        if (!apps[i].alive)
            slot = i;
    if (slot < 0) {
        reason[0] = 0;
        radd("REFUSED: app table full\n");
        return 0;
    }
    /* 最低值必须落下。别人的建议值内存占着坑，就先收回。 */
    uint64_t cap = resource_app_mem_capacity_kb();
    if (sum_granted_kb() + m->mem_min_kb > cap) {
        for (int i = 0; i < MAX_APPS; i++)
            if (apps[i].alive)
                apps[i].mem_granted_kb = apps[i].mem_min_kb;
    }
    struct running_app *a = &apps[slot];
    memset(a, 0, sizeof *a);
    str_copy(a->name, m->name, (int)sizeof a->name);
    a->mem_min_kb = m->mem_min_kb;
    a->mem_sugg_kb = m->mem_sugg_kb;
    a->cpu_min_us = m->cpu_min_us;
    a->cpu_sugg_us = m->cpu_sugg_us;
    a->mem_granted_kb = m->mem_min_kb;
    a->alive = 1;
    uint64_t granted = sum_granted_kb();
    uint64_t room = cap > granted ? cap - granted : 0;
    uint32_t want = m->mem_sugg_kb > m->mem_min_kb ? m->mem_sugg_kb - m->mem_min_kb : 0;
    if (!pressure)
        a->mem_granted_kb += (uint32_t)(want < room ? want : room);
    return a;
}

int resource_release(struct running_app *a)
{
    if (!a || !a->alive)
        return -1;
    a->alive = 0;
    return 0;
}
