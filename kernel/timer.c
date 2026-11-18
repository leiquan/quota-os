/*
 * ARM 通用定时器，EL1 虚拟定时器 CNTV_*（PPI 27）。频率从 CNTFRQ_EL0 读。
 * 调度器只写绝对比较值 CNTV_CVAL：周期起点 + 槽偏移，或运行起点 + 剩余预算。
 * 写 CVAL 的那一步在 boot/vectors.S 的出口里，紧挨着 eret。
 */
#include "quotaos.h"

static uint64_t cntfrq;

static inline uint64_t rd_cntfrq(void)
{
    uint64_t v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

uint64_t ticks_now(void)
{
    uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
}

void timer_init(void)
{
    cntfrq = rd_cntfrq();
    if (cntfrq == 0)
        cntfrq = 62500000;
    uint64_t far = ~0ull;
    asm volatile("msr cntv_cval_el0, %0" :: "r"(far));
    uint64_t ctl = 1; /* ENABLE, IMASK=0 */
    asm volatile("msr cntv_ctl_el0, %0; isb" :: "r"(ctl));
}

uint64_t timer_freq(void)
{
    return cntfrq;
}

uint64_t us_to_ticks(uint64_t us)
{
    return (us * cntfrq) / 1000000ull;
}

uint64_t ticks_to_us(uint64_t t)
{
    return (t / cntfrq) * 1000000ull + ((t % cntfrq) * 1000000ull) / cntfrq;
}

uint64_t now_us(void)
{
    return ticks_to_us(ticks_now());
}

void busy_ticks(uint64_t t)
{
    uint64_t start = ticks_now();
    while (ticks_now() - start < t)
        asm volatile("" ::: "memory");
}
