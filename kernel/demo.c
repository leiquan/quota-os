/*
 * 串口菜单。十个演示，每个对应资源模型里的一条承诺，输出直接给结论。
 *
 *  1 资源总览              6 Tier 1 超时在槽尾被掐断
 *  2 安装应用：允许/拒绝   7 Tier 1 槽从不外借
 *  3 资源不够：拒绝并说关谁 8 Tier 2 最低值有保证，建议值在压力下收回
 *  4 旧设备：超过整机容量  9 Tier 3 过载不影响上层
 *  5 Tier 1 准时           10 任何一级的失控任务只拖慢自己
 *
 * 启动参数（QEMU -append）为 "qa" 时不进菜单，按固定顺序把 1、3–10 跑一遍；
 * "qa-long" 只跑演示 9，周期数取 QA_LONG_PERIODS。两种都以 "QA DONE" 结束。
 */
#include "quotaos.h"

#define QA_PERIODS       1000u
#define QA_LONG_PERIODS  100000u

static uint32_t np = 200;     /* 每个演示窗口的周期数 */
static int qa;

static int parse_int(const char *s)
{
    if (!s || !s[0])
        return -1;
    int v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        v = v * 10 + (*p - '0');
    }
    return v;
}

static void set_name(char *dst, const char *prefix, int n)
{
    int i = 0;
    for (; prefix[i] && i < 12; i++)
        dst[i] = prefix[i];
    dst[i++] = (char)('0' + (n / 10) % 10);
    dst[i++] = (char)('0' + n % 10);
    dst[i] = 0;
}

static int slot_of(const char *name)
{
    int n = 0, s = 0;
    const struct tier1_decl *d = resource_tier1(&n);
    for (int i = 0; i < n; i++) {
        if (!d[i].cpu_us)
            continue;
        if (strcmp(d[i].name, name) == 0)
            return s;
        s++;
    }
    return -1;
}

static uint64_t us(uint64_t t)
{
    return ticks_to_us(t);
}

static int menu_button_yes;

static int ask(const char *prompt)
{
    printf("%s", prompt);
    if (qa || menu_button_yes) {
        printf("y (button)\n");
        return 1;
    }
    char ans[16];
    readline(ans, sizeof ans);
    return ans[0] == 'y' || ans[0] == 'Y';
}

/* ---------- 演示用的任务 ---------- */

static struct task *hog(const char *prefix, int i)
{
    static char names[40][16];
    set_name(names[i], prefix, i);
    struct task *t = task_create(names[i], TIER3, KIND_SPIN);
    t->bench_owned = 1;
    task_start(t);
    return t;
}

static void hogs(int n)
{
    for (int i = 0; i < n; i++)
        hog("bg-", i);
}

/* 演示自己的 Tier 2 应用：先过准入，再建任务。 */
static struct running_app *demo_app(const char *name, uint32_t min_us, uint32_t sugg_us,
                                    int kind, uint32_t work_us, uint32_t deadline_us)
{
    struct app_manifest m = {name, "demo", 512, 512, min_us, sugg_us};
    struct running_app *a = resource_commit(&m);
    if (!a) {
        printf("%s", resource_reason());
        return 0;
    }
    struct task *t = task_create(name, TIER2, kind);
    t->min_t = us_to_ticks(min_us);
    t->sugg_t = us_to_ticks(sugg_us);
    t->work_t = us_to_ticks(work_us);
    t->deadline_t = us_to_ticks(deadline_us);
    a->task = t;
    task_start(t);
    return a;
}

static void app_stop(struct running_app *a)
{
    if (!a)
        return;
    if (a->task && a->task->state != TASK_DEAD)
        task_kill(a->task);
    resource_release(a);
}

static void print_t2_head(void)
{
    printf("  app           min_us sugg_us  recv_min recv_avg recv_max  short\n");
}

static void print_t2(struct task *t)
{
    uint32_t n = t->w.periods ? t->w.periods : 1;
    printf("  ");
    put_padded(t->name, 13);
    printf(" %6lu %7lu %9lu %8lu %8lu %6u\n", us(t->min_t), us(t->sugg_t),
           us(t->w.used_min == ~0ull ? 0 : t->w.used_min), us(t->w.used_sum / n),
           us(t->w.used_max), t->w.short_periods);
}

static void print_t2_job(struct task *t)
{
    printf("  %s: jobs %u, misses %u, worst response %lu us (deadline %lu us)\n",
           t->name, t->w.jobs, t->w.misses, us(t->w.lat_max), us(t->deadline_t));
}

/* 窗口里 Tier 1 作业的错过数和最大抖动。 */
static void t1_summary(uint32_t *miss, uint64_t *jitter)
{
    *miss = 0;
    *jitter = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = sched_task_at(i);
        if (!t || t->tier != TIER1 || t->kind != KIND_JOB || t->state == TASK_DEAD)
            continue;
        *miss += t->w.misses;
        if (t->w.jobs && t->w.start_min != ~0ull && t->w.start_max - t->w.start_min > *jitter)
            *jitter = t->w.start_max - t->w.start_min;
    }
}

static void verdict_t1(void)
{
    uint32_t m;
    uint64_t j;
    t1_summary(&m, &j);
    printf("结果: Tier 1 错过 %u 次，起点抖动 %lu tick，\n  槽内 Tier 2/3 共 %lu tick。\n",
           m, j, sched_foreign_window());
}

/* ---------- 1 资源总览 ---------- */

static void cmd_overview(void)
{
    printf("\n=== 1 资源总览 / resource overview ===\n");
    printf("RAM %lu KB. CPU period %u us. 1 tick = %lu ns.\n", resource_ram_kb(), PERIOD_US,
           1000000000ull / timer_freq());
    decl_notice();
    printf("\n[系统预留] kernel, fixed, never lent\n");
    printf("  CPU %u us/period = %lu ticks\n", SYS_RESERVE_US, us_to_ticks(SYS_RESERVE_US));
    printf("  measured since boot: max %lu ticks/period (period %lu of %lu)\n",
           sched_kernel_max_all(), sched_kernel_max_period(), sched_periods());
    {
        /* v015：这个最大周期是在跑哪个菜单、内核时间花在哪类进入上 */
        struct kattr a;
        sched_kernel_worst(&a);
        printf("  that period ran menu %d; kernel ticks by entry (count):\n", a.label);
        printf("    timer IRQ %lu (%lu), slot guard wait %lu (%lu),\n", a.t[KC_TIMER],
               a.n[KC_TIMER], a.t[KC_GUARD], a.n[KC_GUARD]);
        printf("    device IRQ %lu (%lu), svc IPC/dev %lu (%lu), svc other %lu (%lu);\n",
               a.t[KC_DEVIRQ], a.n[KC_DEVIRQ], a.t[KC_SVC_IPC], a.n[KC_SVC_IPC],
               a.t[KC_SVC_OTHER], a.n[KC_SVC_OTHER]);
        printf("    of which period-boundary work %lu, waits for a deadline %lu\n",
               a.boundary, a.spin);
        printf("    reserve %lu, margin %ld\n", us_to_ticks(SYS_RESERVE_US),
               (long)us_to_ticks(SYS_RESERVE_US) - (long)a.total);
    }
    printf("  memory %u KB\n", KERNEL_RESERVE_KB);

    printf("\n[Tier 1] one fixed slot per task; unused time stays idle\n");
    printf("  task           slot offset_us len_us max_used_us waste_us mem_kb\n");
    int n = 0, s = 0;
    const struct tier1_decl *d = resource_tier1(&n);
    for (int i = 0; i < n; i++) {
        printf("  ");
        put_padded(d[i].name, 14);
        if (!d[i].cpu_us) {
            printf("    -         -      -           -        - %6u\n", d[i].mem_kb);
            continue;
        }
        struct task *o = sched_slot_owner(s);
        uint64_t used = o ? us(o->used_max_all) : 0;
        printf(" %4d %9u %6u %11lu %8lu %6u\n", s, d[i].slot_off_us, d[i].cpu_us, used,
               used < d[i].cpu_us ? d[i].cpu_us - used : 0, d[i].mem_kb);
        s++;
    }
    printf("  total CPU %u us/period, memory %u KB\n", resource_t1_cpu_us(), resource_t1_mem_kb());

    printf("\n[Tier 2] admitted apps (minimum is guaranteed, suggested is spare only)\n");
    int na = resource_running_count();
    if (!na)
        printf("  (none)\n");
    for (int i = 0; i < na; i++) {
        struct running_app *a = resource_running(i);
        printf("  ");
        put_padded(a->name, 14);
        printf(" CPU min %u + sugg %u us, mem min %u KB, granted %u KB\n", a->cpu_min_us,
               a->cpu_sugg_us, a->mem_min_kb, a->mem_granted_kb);
    }
    printf("  minimum sum: CPU %u / %u us, memory %lu / %lu KB\n", resource_cpu_min_sum_us(),
           resource_app_cpu_capacity_us(), resource_mem_min_sum_kb(),
           resource_app_mem_capacity_kb());
    printf("  admission: reserve %u + tier-1 %u + tier-2 min %u\n    + guard %u = %u <= %u us\n",
           SYS_RESERVE_US, resource_t1_cpu_us(), resource_cpu_min_sum_us(), resource_guard_us(),
           SYS_RESERVE_US + resource_t1_cpu_us() + resource_cpu_min_sum_us() + resource_guard_us(),
           PERIOD_US);

    printf("\n[Tier 3] everyone else, round robin %u us, leftover only\n", T3_QUANTUM_US);
    printf("  CPU not promised to anyone: %u us/period\n",
           resource_app_cpu_capacity_us() - resource_cpu_min_sum_us());
    printf("pressure: %s\n", resource_pressure() ? "ON" : "off");
}

/* ---------- 2 安装 / 停止 ---------- */

static void install(const struct app_manifest *m)
{
    printf("\n%s: %s\n", m->name, m->role);
    printf("  CPU minimum %u us, suggested +%u us per %u us\n", m->cpu_min_us, m->cpu_sugg_us,
           PERIOD_US);
    printf("  memory minimum %u KB, suggested %u KB\n", m->mem_min_kb, m->mem_sugg_kb);
    if (resource_check(m) != ADMIT_OK) {
        printf("%s", resource_reason());
        printf("not started.\n");
        return;
    }
    if (!ask("allow this app to reserve its minimum? (y/n) allow> ")) {
        printf("CANCELLED: user denied permission. Nothing was reserved.\n");
        return;
    }
    struct running_app *a = resource_commit(m);
    if (!a) {
        printf("%s", resource_reason());
        return;
    }
    struct task *t = task_create(m->name, TIER2, KIND_SPIN);
    t->min_t = us_to_ticks(m->cpu_min_us);
    t->sugg_t = us_to_ticks(m->cpu_sugg_us);
    a->task = t;
    task_start(t);
    printf("started %s: CPU %u us/period guaranteed,\n  memory %u KB", a->name, a->cpu_min_us,
           a->mem_min_kb);
    printf(" (+%u KB suggested now)\n", a->mem_granted_kb - a->mem_min_kb);
}

static void stop(struct running_app *a)
{
    printf("stopped %s: released CPU %u us/period and %u KB.\n", a->name, a->cpu_min_us,
           a->mem_granted_kb);
    app_stop(a);
}

static void cmd_install(void)
{
    printf("\n=== 2 安装应用：允许/拒绝 / install app: allow or deny ===\n");
    int n = 0;
    const struct app_manifest *cat = resource_catalog(&n);
    for (int i = 0; i < n; i++) {
        printf("  %d) ", i + 1);
        put_padded(cat[i].name, 14);
        printf(" CPU %u+%u us  memory %u..%u KB\n", cat[i].cpu_min_us, cat[i].cpu_sugg_us,
               cat[i].mem_min_kb, cat[i].mem_sugg_kb);
    }
    int nr = resource_running_count();
    for (int i = 0; i < nr; i++)
        printf("  s%d) stop %s\n", i + 1, resource_running(i)->name);
    printf("number to install, s<number> to stop, empty to go back. app> ");
    char line[16];
    readline(line, sizeof line);
    if (line[0] == 's' || line[0] == 'S') {
        int k = parse_int(line + 1);
        if (k < 1 || k > nr) {
            printf("no such running app.\n");
            return;
        }
        stop(resource_running(k - 1));
        return;
    }
    int k = parse_int(line);
    if (k < 1 || k > n) {
        if (line[0])
            printf("no such app.\n");
        return;
    }
    install(&cat[k - 1]);
}

/* ---------- 3 资源不够 ---------- */

static void cmd_full(void)
{
    printf("\n=== 3 资源不够：拒绝并说关谁 / not enough: which app to close ===\n");
    int n = 0;
    const struct app_manifest *cat = resource_catalog(&n);
    printf("The demo starts Pixel Racer, Cinema Play and Studio Cut in turn\n");
    printf("(3000 us minimum each) until admission says no.\n");
    if (!ask("allow the demo to start them? (y/n) allow> ")) {
        printf("CANCELLED: user denied permission.\n");
        return;
    }
    struct running_app *mine[3] = {0, 0, 0};
    int refused = -1;
    for (int i = 0; i < 3; i++) {
        if (resource_check(&cat[i]) == ADMIT_DUP)
            continue;
        struct running_app *a = resource_commit(&cat[i]);
        if (!a) {
            printf("start %s:\n%s", cat[i].name, resource_reason());
            refused = i;
            break;
        }
        printf("start %s: OK, %u us/period minimum.\n", a->name, a->cpu_min_us);
        mine[i] = a;
    }
    if (refused >= 0) {
        /* 照提示关掉那个应用，再试一次，证明提示是对的。 */
        int victim = -1;
        for (int i = 0; i < 3; i++)
            if (mine[i] && victim < 0)
                victim = i;
        if (victim >= 0) {
            printf("following the advice: closing %s and retrying.\n", mine[victim]->name);
            resource_release(mine[victim]);
            mine[victim] = 0;
            struct running_app *a = resource_commit(&cat[refused]);
            printf("start %s: %s\n", cat[refused].name, a ? "OK" : "still refused");
            mine[refused] = a;
        }
    }
    for (int i = 0; i < 3; i++)
        if (mine[i])
            resource_release(mine[i]);
    printf("demo apps closed; their minimums are back in the pool.\n");
}

/* ---------- 4 旧设备 ---------- */

static void cmd_old_device(void)
{
    printf("\n=== 4 旧设备：超过整机容量 / old device: request too big ===\n");
    printf("apps get at most CPU %u us/period and memory %lu KB on this device.\n",
           resource_app_cpu_capacity_us(), resource_app_mem_capacity_kb());
    int n = 0;
    const struct app_manifest *cat = resource_catalog(&n);
    for (int i = 3; i < n; i++) {
        printf("\n%s (CPU min %u us, memory min %u KB):\n", cat[i].name, cat[i].cpu_min_us,
               cat[i].mem_min_kb);
        int rc = resource_check(&cat[i]);
        if (rc == ADMIT_OK)
            printf("  fits this device (not started; use menu 2 to install).\n");
        else
            printf("%s", resource_reason());
    }
}

/* ---------- 5 Tier 1 准时 ---------- */

static void cmd_on_time(void)
{
    printf("\n=== 5 Tier 1 准时 / tier-1 on time ===\n");
    printf("8 tier-3 busy loops run for %u periods. Each tier-1 job must start\n", np);
    printf("at the same tick of its slot every period and finish inside it.\n");
    hogs(8);
    sched_bench(np);
    sched_print_window();
    sched_print_kernel();
    verdict_t1();
}

/* ---------- 6 Tier 1 超时掐断 ---------- */

static void cmd_overrun(void)
{
    printf("\n=== 6 Tier 1 超时在槽尾被掐断 / tier-1 overrun cut at slot end ===\n");
    int s = slot_of("blk-driver");
    if (sched_slot_owner(s)) {
        printf("slot %d is busy.\n", s);
        return;
    }
    struct task *bad = task_create("bad-driver", TIER1, KIND_SPIN);
    sched_bind_slot(bad, s);
    bad->bench_owned = 1;
    task_start(bad);
    hogs(4);
    uint64_t len = us_to_ticks(400);
    printf("bad-driver loops forever in the blk-driver slot (%lu ticks).\n", len);
    sched_bench(np);
    printf("bad-driver per period: min %lu max %lu ticks, slot %lu ticks\n",
           bad->w.used_min, bad->w.used_max, len);
    printf("cut by the slot-end timer: IRQ entry at most %lu ticks after slot end\n",
           sched_irq_lat_max());
    sched_print_window();
    sched_print_slots();
    printf("结果: bad-driver 每周期最多 %lu tick（槽长 %lu）；\n  其他 Tier 1 错过 %lu 次。\n",
           bad->w.used_max, len, (uint64_t)(sched_find("display-vsync")->w.misses +
                                             sched_find("timer-server")->w.misses));
}

/* ---------- 7 Tier 1 槽不外借 ---------- */

static void cmd_never_lent(void)
{
    printf("\n=== 7 Tier 1 槽从不外借 / tier-1 slots never lent ===\n");
    printf("display-vsync uses ~80 of 500 us, timer-server ~40 of 300 us,\n");
    printf("gic-server and blk-driver slots have no task. A greedy tier-2 app\n");
    printf("and 8 tier-3 loops want every tick they can get.\n");
    struct running_app *a = demo_app("greedy-app", 1000, 6000, KIND_SPIN, 0, 0);
    if (!a)
        return;
    a->task->bench_owned = 1;
    hogs(8);
    sched_bench(np);
    sched_print_slots();
    print_t2_head();
    print_t2(a->task);
    resource_release(a);
    printf("结果: 空出来的槽时间全部空转，Tier 2/3 在槽内 %lu tick。\n", sched_foreign_window());
}

/* ---------- 8 Tier 2 最低值与建议值 ---------- */

static void cmd_t2(void)
{
    printf("\n=== 8 Tier 2 最低值有保证，建议值在压力下收回 ===\n");
    struct running_app *a = demo_app("app-A", 2000, 1000, KIND_SPIN, 0, 0);
    struct running_app *b = a ? demo_app("app-B", 2000, 2000, KIND_SPIN, 0, 0) : 0;
    if (!b) {
        app_stop(a);
        return;
    }
    hogs(8);
    printf("app-A min 2000 + sugg 1000 us, app-B min 2000 + sugg 2000 us,\n");
    printf("8 tier-3 loops. Phase 1: no pressure.\n");
    sched_bench(np);
    print_t2_head();
    print_t2(a->task);
    print_t2(b->task);
    uint64_t a1 = a->task->w.used_min, b1 = b->task->w.used_min;
    printf("Phase 2: memory/CPU pressure ON, suggested is reclaimed.\n");
    resource_set_pressure(1);
    hogs(8);
    sched_bench(np);
    print_t2_head();
    print_t2(a->task);
    print_t2(b->task);
    resource_set_pressure(0);
    uint32_t short_n = a->task->w.short_periods + b->task->w.short_periods;
    printf("结果: 无压力时最少拿到 %lu/%lu us，有压力时 %lu/%lu us；\n  低于最低值的周期 %u 个。\n",
           us(a1), us(b1), us(a->task->w.used_min), us(b->task->w.used_min), short_n);
    app_stop(a);
    app_stop(b);
}

/* ---------- 9 Tier 3 过载 ---------- */

static void run_overload(uint32_t periods)
{
    struct running_app *a = demo_app("game", 2000, 1000, KIND_SPIN, 0, 0);
    struct running_app *m = a ? demo_app("audio-mix", 1000, 0, KIND_JOB, 400, 4000) : 0;
    if (!m) {
        app_stop(a);
        return;
    }
    a->task->bench_owned = 1;
    m->task->bench_owned = 1;
    struct task *h[32];
    for (int i = 0; i < 32; i++)
        h[i] = hog("bg-", i);
    printf("32 tier-3 busy loops for %u periods, with a tier-2 game and audio-mix.\n",
           periods);
    sched_bench(periods);
    sched_print_window();
    print_t2_head();
    print_t2(a->task);
    print_t2_job(m->task);
    uint64_t lo = ~0ull, hi = 0, sum = 0;
    for (int i = 0; i < 32; i++) {
        uint64_t avg = h[i]->w.periods ? h[i]->w.used_sum / h[i]->w.periods : 0;
        sum += avg;
        lo = avg < lo ? avg : lo;
        hi = avg > hi ? avg : hi;
    }
    printf("tier-3 per loop: avg %lu..%lu us/period, all 32 together %lu us\n", us(lo), us(hi),
           us(sum));
    sched_print_slots();
    sched_print_kernel();
    verdict_t1();
    printf("结果: Tier 2 低于最低值 %u 个周期，audio-mix 错过 %u 次。\n",
           a->task->w.short_periods, m->task->w.misses);
    resource_release(a);
    resource_release(m);
}

static void cmd_overload(void)
{
    printf("\n=== 9 Tier 3 过载不影响上层 / tier-3 overload ===\n");
    run_overload(np);
}

/* ---------- 10 失控只拖慢自己 ---------- */

static void cmd_runaway(void)
{
    printf("\n=== 10 任何一级的失控任务只拖慢自己 / runaway only slows itself ===\n");
    int s = slot_of("blk-driver");
    if (sched_slot_owner(s)) {
        printf("slot %d is busy.\n", s);
        return;
    }
    struct running_app *enc = demo_app("bad-encoder", 1000, 0, KIND_SPIN, 0, 0);
    struct running_app *mix = enc ? demo_app("audio-mix", 1000, 0, KIND_JOB, 400, 4000) : 0;
    if (!mix) {
        app_stop(enc);
        return;
    }
    enc->task->bench_owned = 1;
    mix->task->bench_owned = 1;
    struct task *drv = task_create("bad-driver", TIER1, KIND_SPIN);
    sched_bind_slot(drv, s);
    drv->bench_owned = 1;
    task_start(drv);
    struct task *run3 = hog("bad-t3-", 0);
    struct task *ok3 = hog("polite-t3-", 1);
    printf("runaways: bad-driver (tier 1, blk slot), bad-encoder (tier 2, min 1000),\n");
    printf("bad-t3 (tier 3). Well behaved: vsync, timer-server, audio-mix, polite-t3.\n");
    sched_bench(np);
    uint32_t n = drv->w.periods ? drv->w.periods : 1;
    printf("bad-driver  avg %lu max %lu us/period (slot 400)\n", us(drv->w.used_sum / n),
           us(drv->w.used_max));
    printf("bad-encoder avg %lu max %lu us/period (min 1000, no suggested)\n",
           us(enc->task->w.used_sum / n), us(enc->task->w.used_max));
    printf("bad-t3 avg %lu us/period, polite-t3 avg %lu us/period\n",
           us(run3->w.used_sum / n), us(ok3->w.used_sum / n));
    print_t2_job(mix->task);
    sched_print_window();
    printf("Tier 2/3 ticks inside tier-1 slots: %lu\n", sched_foreign_window());
    uint32_t m;
    uint64_t j;
    t1_summary(&m, &j);
    printf("结果: 守规矩的 Tier 1 错过 %u 次，audio-mix 错过 %u 次；\n  失控者都停在自己的额度。\n",
           m, mix->task->w.misses);
    resource_release(enc);
    resource_release(mix);
}

static void cmd_help(void)
{
    printf("\n命令（输入数字后回车）:\n");
    printf("  1  资源总览：系统预留、Tier 1 槽和浪费、Tier 2、Tier 3\n");
    printf("  2  安装应用：允许/拒绝（也可以停止应用）\n");
    printf("  3  资源不够：拒绝并说明先关哪个应用\n");
    printf("  4  旧设备：超过整机容量的请求被拒绝\n");
    printf("  5  Tier 1 准时：抖动和错过都是 0\n");
    printf("  6  Tier 1 超时在槽尾被掐断\n");
    printf("  7  Tier 1 槽从不外借\n");
    printf("  8  Tier 2 最低值有保证，建议值在压力下收回\n");
    printf("  9  Tier 3 过载不影响上层\n");
    printf("  10 任何一级的失控任务只拖慢自己\n");
    printf("  11 设备申请 Tier 1：不能打断的设备被拒（说明缺哪一条）\n");
    printf("  12 超额申请被拒（说明是哪种资源）\n");
    printf("  13 预算令牌：驱动替谁干活就扣谁的预算\n");
    printf("  14 设备超时：强制中止，设备降到 Tier 3（少报耗时的设备）\n");
    printf("  15 中断风暴：多出来的中断被限流并计数\n");
    printf("  16 带宽限流：超额就停到下个周期，不欠账\n");
    printf("  17 缓存着色：每个 Tier 1 任务独占自己的缓存颜色\n");
    printf("  18 IPC 余量：用过余量的调用者本周期新调用扣到下个周期\n");
    printf("  h  帮助    q  提示如何退出 QEMU\n");
}

static void run_qa(int long_run)
{
    qa = 1;
    np = QA_PERIODS;
    if (long_run) {
        printf("\n=== QA long run ===\n");
        sched_set_label(9);
        run_overload(QA_LONG_PERIODS);
    } else {
        sched_set_label(1); cmd_overview();
        sched_set_label(3); cmd_full();
        sched_set_label(4); cmd_old_device();
        sched_set_label(5); cmd_on_time();
        sched_set_label(6); cmd_overrun();
        sched_set_label(7); cmd_never_lent();
        sched_set_label(8); cmd_t2();
        sched_set_label(9); cmd_overload();
        sched_set_label(10); cmd_runaway();
        sched_set_label(1); cmd_overview();
    }
    printf("QA DONE\n");
    for (;;)
        asm volatile("wfi");
}

static const char *menu_title[18] = {
    "1 overview",
    "2 install Pixel Racer",
    "3 admit until full",
    "4 reject too big",
    "5 tier-1 on time",
    "6 tier-1 slot cut",
    "7 slots never lent",
    "8 tier-2 minimum",
    "9 tier-3 overload",
    "10 runaway",
    "11 device tier-1",
    "12 overcommit",
    "13 budget token",
    "14 device timeout",
    "15 interrupt storm",
    "16 bandwidth cap",
    "17 cache colors",
    "18 IPC hold",
};

static const char *menu_block(int id)
{
    switch (id) {
    case 6:
    case 10:
        return "binds a spinning task into a tier-1 slot";
    case 13:
    case 14:
    case 15:
    case 16:
        return "sched_bench blocks the shell for the whole window";
    case 18:
        return "four sched_bench windows block the shell";
    default:
        return 0;
    }
}

static char menu_note[120];
static int menu_id;
static int menu_phase;
static int menu_stop;
static struct running_app *menu_a;
static struct running_app *menu_b;
static uint64_t menu_a1, menu_b1;

static void menu_copy(char *dst, int cap, const char *src)
{
    int i = 0;
    if (cap <= 0)
        return;
    for (; src[i] && i < cap - 1; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static void menu_note_set(const char *s)
{
    menu_copy(menu_note, (int)sizeof menu_note, s);
}

static void menu_release(void)
{
    if (menu_a && menu_a->task && menu_a->task->state != TASK_DEAD)
        task_kill(menu_a->task);
    if (menu_b && menu_b->task && menu_b->task->state != TASK_DEAD)
        task_kill(menu_b->task);
    if (menu_a)
        resource_release(menu_a);
    if (menu_b)
        resource_release(menu_b);
    menu_a = 0;
    menu_b = 0;
}

static void menu_shell_line(void)
{
    struct task *sh = sched_find("shell");
    if (sh && sh->w.periods) {
        printf("[menu] shell avg %lu us/period over %u. LVGL draw and input on this tier-3 task.\n",
               ticks_to_us(sh->w.used_sum / sh->w.periods), sh->w.periods);
        printf("[menu] the serial bench blocks the shell, so it does not include this cost.\n");
    }
    printf("[menu] host clock unless boot says icount. An average is not a real-time bound.\n");
    printf("[menu] p99 not measured. No latency histogram is collected.\n");
}

static void menu_finish_window(void)
{
    if (menu_id == 8 && menu_phase == 1 && !menu_stop) {
        menu_a1 = menu_a && menu_a->task ? menu_a->task->w.used_min : 0;
        menu_b1 = menu_b && menu_b->task ? menu_b->task->w.used_min : 0;
        print_t2_head();
        if (menu_a && menu_a->task)
            print_t2(menu_a->task);
        if (menu_b && menu_b->task)
            print_t2(menu_b->task);
        printf("Phase 2: memory/CPU pressure ON, suggested is reclaimed.\n");
        resource_set_pressure(1);
        hogs(8);
        menu_phase = 2;
        if (sched_bench_async(np) != 0) {
            resource_set_pressure(0);
            menu_release();
            menu_phase = 0;
            menu_note_set("phase 2 bench busy");
            printf("[menu] done 8\n");
        }
        return;
    }
    if (menu_id == 5) {
        sched_print_window();
        sched_print_kernel();
        verdict_t1();
    } else if (menu_id == 7) {
        sched_print_slots();
        print_t2_head();
        if (menu_a && menu_a->task)
            print_t2(menu_a->task);
        printf("结果: 空出来的槽时间全部空转，Tier 2/3 在槽内 %lu tick。\n", sched_foreign_window());
        menu_release();
    } else if (menu_id == 8) {
        print_t2_head();
        if (menu_a && menu_a->task)
            print_t2(menu_a->task);
        if (menu_b && menu_b->task)
            print_t2(menu_b->task);
        resource_set_pressure(0);
        {
            uint32_t short_n = 0;
            if (menu_a && menu_a->task)
                short_n += menu_a->task->w.short_periods;
            if (menu_b && menu_b->task)
                short_n += menu_b->task->w.short_periods;
            printf("结果: 无压力时最少拿到 %lu/%lu us，有压力时 %lu/%lu us；\n  低于最低值的周期 %u 个。\n",
                   ticks_to_us(menu_a1), ticks_to_us(menu_b1),
                   ticks_to_us(menu_a && menu_a->task ? menu_a->task->w.used_min : 0),
                   ticks_to_us(menu_b && menu_b->task ? menu_b->task->w.used_min : 0), short_n);
        }
        menu_release();
    } else if (menu_id == 9) {
        sched_print_window();
        print_t2_head();
        if (menu_a && menu_a->task)
            print_t2(menu_a->task);
        if (menu_b && menu_b->task)
            print_t2_job(menu_b->task);
        verdict_t1();
        if (menu_a && menu_a->task && menu_b && menu_b->task)
            printf("结果: Tier 2 低于最低值 %u 个周期，audio-mix 错过 %u 次。\n",
                   menu_a->task->w.short_periods, menu_b->task->w.misses);
        menu_release();
    }
    menu_shell_line();
    if (menu_id == 8 && menu_stop) {
        resource_set_pressure(0);
        menu_release();
    }
    menu_phase = 0;
    menu_stop = 0;
    menu_note_set("window done");
    printf("[menu] done %d\n", menu_id);
}

void demo_menu_stop(void)
{
    if (!menu_phase && !sched_bench_busy()) {
        menu_note_set("idle");
        printf("[menu] stop ignored, idle\n");
        return;
    }
    menu_stop = 1;
    sched_bench_stop();
    printf("[menu] stop: bounded window ends next period\n");
}

void demo_menu_legend(char *buf, int cap)
{
    char tmp[160];
    uint32_t t2 = resource_cpu_min_sum_us();
    uint32_t capu = resource_app_cpu_capacity_us();
    uint32_t left = capu > t2 ? capu - t2 : 0;
    /* Quotas are the admission numbers. Color is only a legend. */
    int n = 0;
    const char *s;
    tmp[0] = 0;
    s = "T1 quota ";
    while (*s && n < (int)sizeof tmp - 1)
        tmp[n++] = *s++;
    /* decimal helpers inline */
    {
        char num[16];
        uint32_t v;
        int k;
        const uint32_t vals[4] = {resource_t1_cpu_us(), resource_t1_mem_kb(), t2, left};
        const char *mid[4] = {"us ", "KB  T2 min ", "us  T3 left ", "us"};
        for (k = 0; k < 4; k++) {
            int ni = 0;
            v = vals[k];
            if (!v)
                num[ni++] = '0';
            else {
                char rev[16];
                int r = 0;
                while (v && r < 15) {
                    rev[r++] = (char)('0' + v % 10);
                    v /= 10;
                }
                while (r)
                    num[ni++] = rev[--r];
            }
            num[ni] = 0;
            s = num;
            while (*s && n < (int)sizeof tmp - 1)
                tmp[n++] = *s++;
            s = mid[k];
            while (*s && n < (int)sizeof tmp - 1)
                tmp[n++] = *s++;
        }
    }
    tmp[n] = 0;
    menu_copy(buf, cap, tmp);
}

static int menu_arm(int id)
{
    printf("\n=== GUI menu %d, same counters as the serial item, %u periods ===\n", id, np);
    if (id == 5) {
        printf("8 tier-3 busy loops. Each tier-1 job must start in its slot.\n");
        hogs(8);
    } else if (id == 7) {
        menu_a = demo_app("greedy-app", 1000, 6000, KIND_SPIN, 0, 0);
        if (!menu_a)
            return -1;
        menu_a->task->bench_owned = 1;
        hogs(8);
    } else if (id == 8) {
        menu_a = demo_app("app-A", 2000, 1000, KIND_SPIN, 0, 0);
        menu_b = menu_a ? demo_app("app-B", 2000, 2000, KIND_SPIN, 0, 0) : 0;
        if (!menu_b) {
            menu_release();
            return -1;
        }
        hogs(8);
        printf("Phase 1: no pressure.\n");
    } else if (id == 9) {
        menu_a = demo_app("game", 2000, 1000, KIND_SPIN, 0, 0);
        menu_b = menu_a ? demo_app("audio-mix", 1000, 0, KIND_JOB, 400, 4000) : 0;
        if (!menu_b) {
            menu_release();
            return -1;
        }
        menu_a->task->bench_owned = 1;
        menu_b->task->bench_owned = 1;
        hogs(32);
        printf("32 tier-3 busy loops, with a tier-2 game and audio-mix.\n");
    }
    if (sched_bench_async(np) != 0) {
        menu_release();
        menu_note_set("bench busy");
        return -1;
    }
    menu_id = id;
    menu_phase = 1;
    printf("[menu] running %d. The shell stays scheduled.\n", id);
    return 0;
}

static void menu_pixel(void)
{
    int n = 0, i;
    const struct app_manifest *cat = resource_catalog(&n);
    for (i = 0; i < resource_running_count(); i++) {
        struct running_app *a = resource_running(i);
        if (a && strcmp(a->name, "Pixel Racer") == 0) {
            stop(a);
            menu_note_set("Pixel Racer stopped");
            printf("[menu] done 2\n");
            return;
        }
    }
    if (n < 1)
        return;
    menu_button_yes = 1;
    install(&cat[0]);
    menu_button_yes = 0;
    menu_note_set("Pixel Racer install attempted");
    printf("[menu] done 2\n");
}

int demo_menu_count(void)
{
    return 18;
}

void demo_menu_line(int index, char *buf, int cap)
{
    int id = index + 1;
    const char *why = menu_block(id);
    const char *title = (index >= 0 && index < 18) ? menu_title[index] : "?";
    if (why)
        menu_copy(buf, cap, title);
    else
        menu_copy(buf, cap, title);
    if (why && cap > 20) {
        int n = 0;
        while (buf[n])
            n++;
        if (n < cap - 16) {
            const char *tag = "  -- not connected";
            int k = 0;
            while (tag[k] && n + k < cap - 1) {
                buf[n + k] = tag[k];
                k++;
            }
            buf[n + k] = 0;
        }
    }
    (void)why;
}

int demo_menu_start(int id)
{
    const char *why;
    if (id < 1 || id > 18)
        return -1;
    if (menu_phase || sched_bench_busy()) {
        menu_note_set("busy");
        printf("[menu] busy\n");
        return -1;
    }
    why = menu_block(id);
    if (why) {
        printf("[menu] %d not connected: %s\n", id, why);
        menu_note_set(why);
        printf("[menu] done %d\n", id);
        return -2;
    }
    menu_id = id;
    if (id == 1) {
        cmd_overview();
        menu_note_set("overview printed");
        printf("[menu] done 1\n");
        return 0;
    }
    if (id == 2) {
        menu_pixel();
        return 0;
    }
    if (id == 3) {
        menu_button_yes = 1;
        cmd_full();
        menu_button_yes = 0;
        menu_note_set("admit cycle done");
        printf("[menu] done 3\n");
        return 0;
    }
    if (id == 4) {
        cmd_old_device();
        menu_note_set("oversize checks printed");
        printf("[menu] done 4\n");
        return 0;
    }
    if (id == 11) {
        decl_demo_device_tier1();
        menu_note_set("device checks printed");
        printf("[menu] done 11\n");
        return 0;
    }
    if (id == 12) {
        decl_demo_overcommit();
        menu_note_set("overcommit checks printed");
        printf("[menu] done 12\n");
        return 0;
    }
    if (id == 17) {
        e4_demo_color(0);
        menu_note_set("color report printed");
        printf("[menu] done 17\n");
        return 0;
    }
    return menu_arm(id);
}

void demo_menu_status(char *buf, int cap)
{
    if (menu_phase && !sched_bench_busy())
        menu_finish_window();
    if (menu_phase) {
        menu_copy(buf, cap, "running");
        return;
    }
    if (menu_note[0])
        menu_copy(buf, cap, menu_note);
    else
        menu_copy(buf, cap, "idle");
}

void shell_main(void)
{
    printf("[boot] shell task running\n");
    decl_init();
    decl_verify_devices();
    const char *args = dtb_bootargs();
    if (strcmp(args, "webkit") == 0) {
        webkit_autostart();
        for (;;)
            asm volatile("wfi");
    }
    if (strcmp(args, "e1") == 0) {
        decl_demo_device_tier1();
        decl_demo_overcommit();
        decl_qa_e1(0);
        printf("QA DONE\n");
        for (;;)
            asm volatile("wfi");
    }
    if (strcmp(args, "e3") == 0) {
        qa = 1;
        sched_set_label(14); e3_demo_timeout(QA_PERIODS, 1);
        sched_set_label(1); cmd_overview();
        sched_set_label(15); e3_demo_storm(QA_PERIODS, 1);
        printf("QA DONE\n");
        for (;;)
            asm volatile("wfi");
    }
    if (strcmp(args, "e2h") == 0) {
        qa = 1;
        sched_set_label(18); e2_hold_demo(QA_PERIODS, 1);
        printf("QA DONE\n");
        for (;;)
            asm volatile("wfi");
    }
    if (strcmp(args, "e4") == 0) {
        qa = 1;
        sched_set_label(16); e4_demo_bandwidth(QA_PERIODS, 1);
        sched_set_label(17); e4_demo_color(1);
        printf("QA DONE\n");
        for (;;)
            asm volatile("wfi");
    }
    if (strcmp(args, "e2") == 0) {
        qa = 1;
        sched_set_label(13); e2_demo(QA_PERIODS, 1);
        printf("QA DONE\n");
        for (;;)
            asm volatile("wfi");
    }
    if (strcmp(args, "isolate") == 0) {
        printf("[boot] el0 isolation\n");
        isolate_run();
        for (;;)
            asm volatile("wfi");
    }
    if (strcmp(args, "qa") == 0)
        run_qa(0);
    if (strcmp(args, "qa-long") == 0)
        run_qa(1);
    /* Empty bootargs are the owner's UTM default. That path is LVGL.
     * The serial menu is only -append serial. e4, webkit, qa stay explicit. */
    if (strcmp(args, "serial") != 0) {
        if (args[0] == 0)
            printf("[boot] no bootarg, starting LVGL\n");
        else if (strcmp(args, "lvgl") == 0)
            printf("[boot] bootarg lvgl, starting LVGL\n");
        else
            printf("[boot] bootarg %s is not serial, starting LVGL\n", args);
        lvgl_autostart();
        for (;;)
            asm volatile("wfi");
    }
    printf("[boot] serial menu\n");
    cmd_help();
    for (;;) {
        printf("select> ");
        char line[32];
        readline(line, sizeof line);
        int k = parse_int(line);
        if (k > 0)
            sched_set_label(k);
        switch (k) {
        case 1: cmd_overview(); break;
        case 2: cmd_install(); break;
        case 3: cmd_full(); break;
        case 4: cmd_old_device(); break;
        case 5: cmd_on_time(); break;
        case 6: cmd_overrun(); break;
        case 7: cmd_never_lent(); break;
        case 8: cmd_t2(); break;
        case 9: cmd_overload(); break;
        case 10: cmd_runaway(); break;
        case 11: decl_demo_device_tier1(); break;
        case 12: decl_demo_overcommit(); break;
        case 13: e2_demo(np, 0); break;
        case 14: e3_demo_timeout(np, 0); break;
        case 15: e3_demo_storm(np, 0); break;
        case 16: e4_demo_bandwidth(np, 0); break;
        case 17: e4_demo_color(0); break;
        case 18: e2_hold_demo(np, 0); break;
        default:
            if (line[0] == 'h' || line[0] == 'H' || line[0] == '?')
                cmd_help();
            else if (line[0] == 'q' || line[0] == 'Q')
                printf("在 QEMU 窗口按 Ctrl-A 然后 X 退出。\n");
            else if (line[0])
                printf("未知选项。按 h 看命令。\n");
        }
    }
}
