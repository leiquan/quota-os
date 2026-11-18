/*
 * E2：预算令牌随 IPC 走。驱动是被动服务端，自己不被调度；应用调用它时，
 * 内核把应用的预算借给驱动用，驱动干活的时间记在应用头上（seL4 MCS 的做法），
 * 驱动自己的预算不动。
 */
#include "quotaos.h"

static uint64_t sys_call(uint64_t server, uint64_t req)
{
    register uint64_t x0 asm("x0") = server;
    register uint64_t x1 asm("x1") = req;
    asm volatile("svc #1" : "+r"(x0) : "r"(x1) : "memory");
    return x0;
}

static uint64_t sys_reply_wait(uint64_t reply)
{
    register uint64_t x0 asm("x0") = reply;
    asm volatile("svc #2" : "+r"(x0) : : "memory");
    return x0;
}

/* 驱动：收到的请求是要占用模拟设备多少 us；回复它自己量的耗时（只作参考）。 */
/* 第一次请求由内核放进 x0，从这里开始；以后每次从 svc #2 返回时拿到下一个请求。 */
static void driver_entry(uint64_t req)
{
    for (;;) {
        uint64_t t0 = ticks_now();
        busy_ticks(us_to_ticks(req));
        uint64_t r = ticks_now() - t0;
        req = sys_reply_wait(r);
    }
}

static int drv_id;
static int fwd_id;

/* v015：转发服务端 A：把调用者的请求原样转给驱动 B，再把 B 的回复转回去。全程用调用者的预算。 */
static void fwd_entry(uint64_t req)
{
    for (;;) {
        uint64_t r = sys_call((uint64_t)drv_id, req);
        req = sys_reply_wait(r);
    }
}

/* 应用：不停地调驱动，请求 50..400 us（固定的伪随机序列）。 */
static void app_entry(void)
{
    uint32_t x = 12345;
    for (;;) {
        x = x * 1103515245u + 12345u;
        sys_call((uint64_t)drv_id, 50 + (x >> 16) % 351);
    }
}

/* v014 的余量演示：短调用（5..60 us），内核路径多，预算常在内核路径里到头。 */
static void app_entry_short(void)
{
    uint32_t x = 777;
    for (;;) {
        x = x * 1103515245u + 12345u;
        sys_call((uint64_t)drv_id, 5 + (x >> 16) % 56);
    }
}

/* v015：同样的短调用，但打给转发服务端 A */
static void app_entry_fwd(void)
{
    uint32_t x = 777;
    for (;;) {
        x = x * 1103515245u + 12345u;
        sys_call((uint64_t)fwd_id, 5 + (x >> 16) % 56);
    }
}

static void e2_run(uint32_t periods, int qa_line, int hold, int fwd);

void e2_demo(uint32_t periods, int qa_line)
{
    e2_run(periods, qa_line, 0, 0);
}

static uint64_t off_both, off_multi, off_pp_max;

void e2_hold_demo(uint32_t periods, int qa_line)
{
    /* 每种情况先关掉规则跑一遍作对照（同样的负载），再按规则跑。
     * 第二种（v015）：Tier 2 调用者 -> 转发服务端 A -> 驱动 B，都记在调用者头上。 */
    for (int fwd = 0; fwd <= 1; fwd++) {
        sched_set_ipc_hold_rule(0);
        e2_run(periods, 0, 2, fwd);
        sched_set_ipc_hold_rule(1);
        e2_run(periods, qa_line, 1, fwd);
    }
}

static void e2_run(uint32_t periods, int qa_line, int hold, int fwd)
{
    if (hold == 2 && fwd)
        printf("\n=== 18 对照（经转发服务端）：不扣住新调用 ===\n");
    else if (hold && fwd)
        printf("\n=== 18 经转发服务端：用过余量的预算本周期新调用扣到下个周期 ===\n");
    else if (hold == 2)
        printf("\n=== 18 对照：不扣住新调用（v013 的做法）===\n");
    else if (hold)
        printf("\n=== 18 IPC 余量：用过余量的调用者本周期新调用扣到下个周期 ===\n");
    else
        printf("\n=== 13 预算令牌：驱动替谁干活就扣谁的预算 ===\n");
    struct task *drv = task_create("sim-dma-drv", TIER_SERVER, KIND_SERVER);
    task_set_entry(drv, (void (*)(void))driver_entry);
    drv->min_t = us_to_ticks(500);      /* 驱动自己的预算（这里不用） */
    drv->bench_owned = 1;
    drv_id = drv->id;
    struct task *fs = 0;
    if (fwd) {
        fs = task_create("fwd-srv", TIER_SERVER, KIND_SERVER);
        task_set_entry(fs, (void (*)(void))fwd_entry);
        fs->min_t = us_to_ticks(500);
        fs->bench_owned = 1;
        fwd_id = fs->id;
    }
    void (*ent)(void) = fwd ? app_entry_fwd : hold ? app_entry_short : app_entry;
    struct task *app = task_create("cam-app", TIER2, KIND_SPIN);
    task_set_entry(app, ent);
    app->min_t = us_to_ticks(3000);
    if (hold)
        app->sugg_t = us_to_ticks(1000);    /* 有建议值：保底轮和建议轮都可能在内核路径里到头 */
    app->bench_owned = 1;
    struct task *bg = task_create("bg-caller", TIER3, KIND_SPIN);
    task_set_entry(bg, hold ? app_entry_short : app_entry);
    bg->bench_owned = 1;
    task_start(app);
    task_start(bg);
    decl_try_ipc_t2("cam-app", 3000, 0);
    struct task *hud = 0;
    if (hold) {
        /* 第二个 Tier 2 调用者：cam-app 保底轮用完时先轮到它，cam-app 的建议轮在后面，
         * 这样同一个周期里两轮都可能在内核路径里到头。 */
        decl_try_ipc_t2("hud-app", 1500, 3000 + IPC_HEADROOM_US);
        hud = task_create("hud-app", TIER2, KIND_SPIN);
        task_set_entry(hud, ent);
        hud->min_t = us_to_ticks(1500);
        hud->sugg_t = us_to_ticks(500);
        hud->bench_owned = 1;
        task_start(hud);
    }
    if (hold) {
        printf("cam-app (tier 2, min 3000 us, suggested 1000 us), hud-app\n");
        printf("(tier 2, min 1500 us, suggested 500 us) and bg-caller (tier 3)\n");
        printf("call sim-dma-drv for 5..60 us of device work each.\n");
        if (fwd)
            printf("cam-app and hud-app call via fwd-srv (server A), which forwards\n"
                   "each call to sim-dma-drv (server B); both run on the caller's budget.\n");
    } else {
        printf("cam-app (tier 2, min 3000 us) and bg-caller (tier 3) call\n");
        printf("sim-dma-drv (passive server) for 50..400 us of device work each.\n");
    }
    struct ipc_stats s;
    sched_ipc_stats(&s, 1);
    uint64_t drv_used0 = drv->used_t + (fs ? fs->used_t : 0);
    sched_bench(periods);
    sched_ipc_stats(&s, 0);
    uint64_t drv_change = drv->used_t + (fs ? fs->used_t : 0) - drv_used0;
    printf("calls %lu; measured by the kernel during the calls:\n", s.calls);
    printf("  driver running %lu ticks + IPC kernel path %lu ticks\n", s.run_sum,
           s.meas_sum - s.run_sum);
    printf("  = %lu ticks; caller budgets dropped by %lu ticks\n", s.meas_sum,
           s.drop_sum);
    printf("  longest call %lu ticks; max |drop - measured| per call: %lu ticks\n",
           s.meas_max, s.diff_max);
    printf("  IPC kernel path charged to callers in total (none to the system\n");
    printf("  reserve): %lu ticks\n", s.kern_sum);
    printf("    = in-call window %lu + entry and queueing %lu\n", s.kern_in, s.kern_entry);
    printf("    + reply return %lu (sum %lu)\n", s.kern_reply,
           s.kern_in + s.kern_entry + s.kern_reply);
    uint64_t wdrop = s.w_used - s.w_own;
    uint64_t wsum = s.w_run + s.w_kin + s.w_kent + s.w_krep;
    uint64_t werr = wdrop > wsum ? wdrop - wsum : wsum - wdrop;
    printf("  whole window (per-period budgets): caller budget drops %lu\n", wdrop);
    printf("    = driver run %lu + in-call %lu\n", s.w_run, s.w_kin);
    printf("    + entry %lu + reply %lu\n", s.w_kent, s.w_krep);
    printf("    = %lu; error %lu ticks\n", wsum, werr);
    /* v013：两种口径对账。累计 = 窗口前 + 窗口（按周期结算的 w_*）+ 窗口后，逐项精确相等。 */
    printf("  counting windows (ticks):\n");
    printf("    cumulative: from stats reset %lu to read %lu\n", s.t_reset, s.t_read);
    printf("    per-period: periods %lu..%lu, ticks %lu..%lu\n", s.p_w0, s.p_w1,
           s.t_w0, s.t_w1);
    printf("  part         before     window      after  cumulative  err\n");
    static const char *pn[4] = {"driver run", "in-call", "entry", "reply"};
    uint64_t wv[4] = {s.w_run, s.w_kin, s.w_kent, s.w_krep};
    uint64_t cv[4] = {s.run_cum, s.kern_in, s.kern_entry, s.kern_reply};
    uint64_t rerr = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t before = s.snap0[i], after = cv[i] - s.snap1[i];
        uint64_t sum = before + wv[i] + after;
        uint64_t e = sum > cv[i] ? sum - cv[i] : cv[i] - sum;
        rerr += e;
        printf("  ");
        put_padded(pn[i], 10);
        printf(" %8lu %10lu %10lu %11lu %4lu\n", before, wv[i], after, cv[i], e);
    }
    int64_t rdiff = (int64_t)s.run_sum - (int64_t)(s.run_cum + s.infl_reset_run - s.infl_end_run);
    uint64_t rd = rdiff < 0 ? (uint64_t)-rdiff : (uint64_t)rdiff;
    printf("  driver run per replied call %lu = charged %lu\n", s.run_sum, s.run_cum);
    printf("    + ran before reset in calls open then %lu - ran in %lu calls\n",
           s.infl_reset_run, s.infl_end_calls);
    printf("    still open at the end %lu; error %lu ticks\n", s.infl_end_run, rd);
    rerr += rd;
    printf("  IPC kernel path past the end of a tier-2 caller's budget, taken\n");
    printf("  from its admitted IPC headroom (%u us = %lu ticks per period):\n",
           IPC_HEADROOM_US, us_to_ticks(IPC_HEADROOM_US));
    printf("    %lu times, total %lu, max %lu ticks; most in one period %lu;\n",
           s.hr_n, s.hr_sum, s.hr_max, s.hr_pp_max);
    printf("    charged to the system reserve: 0\n");
    printf("  caller-periods with a draw %lu, with draws in both the guaranteed\n",
           s.hr_periods);
    printf("  and the suggested pass %lu; new calls held to the next period %lu,\n",
           s.hr_both_periods, s.held_n);
    printf("  released at the next period start %lu\n", s.held_resumed);
    printf("  caller-periods with 2 or more draws %lu; forwarded calls %lu,\n",
           s.hr_multi_periods, s.fwd_calls);
    printf("  replied %lu\n", s.fwd_replies);
    printf("  driver's own budget used during the window: %lu ticks\n", drv_change);
    printf("  driver's own wall-clock timing incl. preemption (reference only):\n");
    printf("  total %lu ticks\n", s.self_sum);
    sched_print_window();
    sched_print_kernel();
    uint64_t ma, mb;
    sched_window_misses(&ma, &mb);
    if (hold == 2) {
        off_both = s.hr_both_periods;
        off_multi = s.hr_multi_periods;
        off_pp_max = s.hr_pp_max;
    }
    if (qa_line && hold == 1) {
        const char *tag = fwd ? "E2 headroom fwd" : "E2 headroom";
        printf("%s: most drawn by one owner in one period %lu <= %lu\n", tag, s.hr_pp_max,
               us_to_ticks(IPC_HEADROOM_US));
        printf("  admitted; owner-periods with 2+ draws %lu (both passes %lu),\n",
               s.hr_multi_periods, s.hr_both_periods);
        printf("  held calls %lu, tier-1 misses %lu; without the rule (same load):\n",
               s.held_n, ma + mb);
        printf("  2+ draws %lu (both passes %lu), most in one period %lu\n", off_multi,
               off_both, off_pp_max);
        printf("%s %s\n", tag,
               s.hr_pp_max <= us_to_ticks(IPC_HEADROOM_US) && s.hr_multi_periods == 0 &&
               s.hr_both_periods == 0 && off_multi > 0 && s.held_n > 0 && ma + mb == 0 &&
               werr == 0 && rerr == 0 && s.diff_max <= 1 && drv_change == 0 &&
               (!fwd || s.fwd_calls > 0) ? "PASS" : "FAIL");
    }
    if (qa_line)
        printf("E2 item8 %s\n",
               s.calls > 0 && s.diff_max <= 1 && drv_change == 0 &&
               s.kern_in + s.kern_entry + s.kern_reply == s.kern_sum && werr == 0 &&
               rerr == 0 && s.hr_pp_max <= us_to_ticks(IPC_HEADROOM_US) ? "PASS" : "FAIL");
}

/* ---------- E3：设备闸门、超时降级、中断风暴 ---------- */

static uint64_t sys_dev_submit(uint64_t dev, uint64_t us)
{
    register uint64_t x0 asm("x0") = dev;
    register uint64_t x1 asm("x1") = us;
    asm volatile("svc #3" : "+r"(x0) : "r"(x1) : "memory");
    return x0;
}

/* 驱动：把调用者的请求交给设备（经过闸门），把结果（实测 tick 或错误码）回给调用者。 */
static void dev_driver_entry(uint64_t req)
{
    for (;;) {
        uint64_t r = sys_dev_submit(0, req);
        req = sys_reply_wait(r);
    }
}

static volatile uint64_t e3_ok[2], e3_err[2];

static void e3_loop(int k)
{
    uint32_t x = 777u + (uint32_t)k;
    for (;;) {
        x = x * 1103515245u + 12345u;
        uint64_t r = sys_call((uint64_t)drv_id, 50 + (x >> 16) % 351);
        if (r >> 62)
            e3_err[k]++;
        else
            e3_ok[k]++;
    }
}

static void e3_cam(void) { e3_loop(0); }
static void e3_bg(void) { e3_loop(1); }

static struct task *make_driver(void)
{
    struct task *drv = task_create("sim-dma-drv", TIER_SERVER, KIND_SERVER);
    task_set_entry(drv, (void (*)(void))dev_driver_entry);
    drv->min_t = us_to_ticks(300);      /* 驱动自己的预算：没有作业时的中断处理记在这里 */
    drv->bench_owned = 1;
    drv_id = drv->id;
    return drv;
}

void e3_demo_timeout(uint32_t periods, int qa_line)
{
    printf("\n=== 14 设备超时：强制中止，设备降到 Tier 3 ===\n");
    struct sim_dev *d = decl_dev(0);
    decl_reset_device(0);
    klog_flush();
    printf("sim-dma reset to its factory state and re-verified: %s.\n",
           d->state == DEV_OK ? "verified" : "NOT verified");
    printf("display-vsync holds sim-dma Tier 1 (policy degrade).\n");
    decl_add_t1_app("hud-overlay", 0, 500, 1);
    d->under_report = 1;
    printf("sim-dma now under-reports: declares %u us per job, really\n", d->max_job_us);
    printf("takes 1.5x the request. cam-app (tier 2, device budget 3000 us)\n");
    printf("and bg-caller (tier 3) send 50..400 us jobs through the gate.\n");
    struct task *drv = make_driver();
    struct task *app = task_create("cam-app", TIER2, KIND_SPIN);
    task_set_entry(app, e3_cam);
    app->min_t = us_to_ticks(3000);
    app->dev_budget_t = us_to_ticks(3000);
    app->bench_owned = 1;
    struct task *bg = task_create("bg-caller", TIER3, KIND_SPIN);
    task_set_entry(bg, e3_bg);
    bg->bench_owned = 1;
    e3_ok[0] = e3_ok[1] = e3_err[0] = e3_err[1] = 0;
    dev_bind(0, drv);
    struct dev_stats s;
    dev_stats_get(&s, 1);
    task_start(app);
    task_start(bg);
    sched_bench(periods);
    dev_stats_get(&s, 0);
    static const char *k7[] = {"[health] sim-dma timeout", "measured ", "declared ", "period "};
    int line7 = klog_find_all(k7, 4);
    int revoked = klog_count_lines("Tier 1 revoked (实测超出声明)");
    klog_flush();
    d->under_report = 0;
    printf("device jobs: submitted %lu, completed %lu, timed out %lu\n", s.submits,
           s.completed, s.timeouts);
    printf("  (timed-out jobs are aborted and discarded); first timeout in\n");
    printf("  period %lu\n", s.first_timeout_period);
    printf("gate refused: device budget %lu, would reach the next tier-1\n", s.refused[1]);
    printf("  guard %lu, device in Tier 3 and caller not Tier 3 %lu\n", s.refused[2],
           s.refused[3]);
    printf("abort -> stopped: %lu aborts, max %lu us (declared bound %u us,\n", s.stop_n,
           up_us(s.stop_max_t), d->stop_us);
    printf("  CPU guard %u us); over the bound %lu, over the guard %lu\n", GUARD_US,
           s.stop_over_bound, s.stop_over_guard);
    printf("device jobs or aborts ending after the next tier-1 guard: %lu\n", s.abort_late);
    printf("device busy when a tier-1 guard began: %lu\n", s.t1_busy);
    printf("device time charged to callers: %lu ticks\n", s.dev_time_t);
    printf("cam-app calls ok %lu / refused %lu; bg-caller ok %lu / refused %lu\n",
           e3_ok[0], e3_err[0], e3_ok[1], e3_err[1]);
    sched_print_window();
    sched_print_kernel();
    printf("Tier 1 declarations left on sim-dma: %d\n", decl_t1_left_on(0));
    int named = decl_try_t1("cam-app", 0, 1000, "实测超出声明");
    if (!qa_line)
        return;
    uint64_t ma, mb;
    sched_window_misses(&ma, &mb);
    int verify_ok = 1;
    for (int i = 0; i < 3; i++) {
        struct sim_dev *v = decl_dev(i);
        if (v->preemptible && (v->stop_max_t > us_to_ticks(v->stop_us) ||
                               v->stop_max_t > us_to_ticks(GUARD_US)))
            verify_ok = 0;
    }
    printf("E3 item1 under-reporting preemptible device: tier-1 misses %lu,\n", ma + mb);
    printf("  device busy at a guard %lu, ends after a guard %lu, timeouts %lu\n",
           s.t1_busy, s.abort_late, s.timeouts);
    printf("E3 item1 %s\n",
           ma + mb == 0 && s.t1_busy == 0 && s.abort_late == 0 && s.timeouts > 0 ? "PASS"
                                                                              : "FAIL");
    printf("E3 item3 timed-out device in Tier 3: %s; later Tier 1 refused\n",
           d->state == DEV_DEMOTED ? "yes" : "no");
    printf("  naming 实测超出声明: %s\n", named ? "yes" : "no");
    printf("E3 item3 %s\n", d->state == DEV_DEMOTED && named ? "PASS" : "FAIL");
    printf("E3 item6 aborts %lu (+ verification aborts of preemptible devices):\n",
           s.stop_n);
    printf("  over the declared bound %lu, over the guard %lu, verification %s\n",
           s.stop_over_bound, s.stop_over_guard, verify_ok ? "ok" : "over");
    printf("E3 item6 %s\n",
           s.stop_n > 0 && !s.stop_over_bound && !s.stop_over_guard && verify_ok ? "PASS"
                                                                              : "FAIL");
    printf("E3 item7 log line with device, measured, declared, period: %s\n",
           line7 ? "yes" : "no");
    printf("E3 item7 %s\n", line7 ? "PASS" : "FAIL");
    int left = decl_t1_left_on(0);
    printf("E3 policy: tier-1 apps on sim-dma handled together %d (expected 2),\n", revoked);
    printf("  still Tier 1 on it %d\n", left);
    printf("E3 policy %s\n", revoked == 2 && left == 0 ? "PASS" : "FAIL");
}

void e3_demo_storm(uint32_t periods, int qa_line)
{
    printf("\n=== 15 中断风暴：多出来的中断被限流并计数 ===\n");
    struct sim_dev *d = decl_dev(0);
    printf("sim-dma raises its IRQ line every 16 us (62500 per second).\n");
    printf("Declared limit %u IRQs per period; the line is masked inside\n", d->irq_limit);
    printf("tier-1 guards and slots, and past the limit or the driver's own\n");
    printf("300 us budget until the next period.\n");
    struct task *drv = make_driver();
    struct task *app = task_create("cam-app", TIER2, KIND_SPIN);
    app->min_t = us_to_ticks(3000);
    app->bench_owned = 1;
    struct task *bg = task_create("bg-load", TIER3, KIND_SPIN);
    bg->bench_owned = 1;
    dev_bind(0, drv);
    struct dev_stats s;
    dev_stats_get(&s, 1);
    task_start(app);
    task_start(bg);
    dev_storm(16);
    sched_bench(periods);
    dev_stats_get(&s, 0);
    klog_flush();
    uint64_t sup = s.raised > s.irq_handled ? s.raised - s.irq_handled : 0;
    printf("IRQs raised by the device (its own counter) %lu, handled %lu,\n", s.raised,
           s.irq_handled);
    printf("  not delivered (limited) %lu; most handled in one period %lu\n", sup,
           s.irq_period_max);
    printf("  (limit %u); line masked for the rest of a period %lu times\n", d->irq_limit,
           s.lim_masks);
    printf("device IRQs taken inside a tier-1 guard or slot: %lu\n", s.irq_in_t1);
    printf("cam-app (tier 2, min 3000 us): lowest per period %lu us\n",
           ticks_to_us(app->w.used_min == ~0ull ? 0 : app->w.used_min));
    sched_print_window();
    sched_print_kernel();
    if (!qa_line)
        return;
    uint64_t ma, mb;
    sched_window_misses(&ma, &mb);
    int ok = ma + mb == 0 && s.irq_period_max <= d->irq_limit && sup > 0 && s.irq_in_t1 == 0 &&
             s.raised >= 50000ull * periods / 100;
    printf("E3 item5 storm: tier-1 misses %lu, limited %lu, max per period %lu\n", ma + mb,
           sup, s.irq_period_max);
    printf("E3 item5 %s\n", ok ? "PASS" : "FAIL");
}
