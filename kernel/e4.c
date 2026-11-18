/*
 * E4 演示：内存带宽按周期限流（MemGuard 的做法，计数是模拟的）和缓存着色。
 *
 * 带宽：每个任务每周期有访存次数预算。计数器是模拟的：运行 tick * 任务的访存密度
 * （QEMU 没有访存事件计数器）。内核按剩余预算算出还能跑多少 tick，和 CPU 预算一起取小的
 * 写进比较值，相当于计数器溢出中断；用完就停到下个周期，下个周期额度恢复，超出的部分不欠账。
 */
#include "quotaos.h"

void e4_demo_bandwidth(uint32_t periods, int qa_line)
{
    printf("\n=== 16 带宽限流：超额就停到下个周期，不欠账 ===\n");
    printf("Simulated access counter (QEMU has no memory-access event):\n");
    printf("accesses = ticks run * the task's access density.\n");
    decl_try_bw("cam-app", TIER2, 400000);
    decl_try_bw("bw-greedy", TIER2, 1700000);
    struct task *app = task_create("cam-app", TIER2, KIND_SPIN);
    app->min_t = us_to_ticks(3000);
    app->bw_rate = 2000;                /* 2 次 / tick */
    app->bw_budget = 400000;
    app->bench_owned = 1;
    struct task *hog = task_create("mem-hog", TIER3, KIND_SPIN);
    hog->bw_rate = 4000;                /* 4 次 / tick */
    hog->bw_budget = 150000;            /* Tier 3：系统给的固定小额度 */
    hog->bench_owned = 1;
    printf("cam-app: tier 2, min 3000 us, 2 accesses/tick, budget %lu\n", app->bw_budget);
    printf("mem-hog: tier 3, 4 accesses/tick, budget %lu per period\n", hog->bw_budget);
    task_start(app);
    task_start(hog);
    sched_bench(periods);
    struct task *h = hog;
    /* 停住最多晚 20 tick：出口尾巴的最短期限 12 tick + 中断进入 3 + 余量 */
    uint64_t bound = h->bw_rate * 20 / 1000 + 1;
    printf("mem-hog over %lu periods: stalled %lu, accesses per period\n", h->bw_w_periods,
           h->bw_w_stalled);
    printf("  min %lu max %lu (budget %lu); most over the budget %lu\n",
           h->bw_w_used_min, h->bw_w_used_max, h->bw_budget, h->bw_w_over_max);
    printf("  (bound %lu = 20 ticks of accesses); ran again after a stall in\n", bound);
    printf("  the same period %lu; lowest in a period after a stall %lu\n", h->bw_w_after,
           h->bw_w_restore_min == ~0ull ? 0 : h->bw_w_restore_min);
    printf("cam-app: accesses per period max %lu (budget %lu), stalled %lu;\n",
           app->bw_w_used_max, app->bw_budget, app->bw_w_stalled);
    printf("  CPU per period min %lu us (min 3000)\n",
           ticks_to_us(app->w.used_min == ~0ull ? 0 : app->w.used_min));
    sched_print_window();
    sched_print_kernel();
    printf("isolation effect on a real memory bus (MPAM): 缺真实板子\n");
    if (!qa_line)
        return;
    uint64_t ma, mb;
    sched_window_misses(&ma, &mb);
    int ok = h->bw_w_stalled > 0 && h->bw_w_after == 0 && h->bw_w_over_max <= bound &&
             h->bw_w_restore_min != ~0ull && h->bw_w_restore_min >= h->bw_budget &&
             app->w.short_periods == 0 && ma + mb == 0;
    printf("E4 item9 stalled %lu, ran after stall %lu, over max %lu <= %lu,\n",
           h->bw_w_stalled, h->bw_w_after, h->bw_w_over_max, bound);
    printf("  restored next period %s, tier-1 misses %lu\n",
           h->bw_w_restore_min != ~0ull && h->bw_w_restore_min >= h->bw_budget ? "yes" : "no",
           ma + mb);
    printf("E4 item9 %s\n", ok ? "PASS" : "FAIL");
}

void e4_demo_color(int qa_line)
{
    printf("\n=== 17 缓存着色：每个 Tier 1 任务独占自己的缓存颜色 ===\n");
    color_free_shared();
    uint32_t got = color_alloc_shared(512);
    printf("a tier-3 app asks for 512 pages: got %u, all from shared colors\n", got);
    color_report(qa_line);
}
