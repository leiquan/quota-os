/*
 * 严格时间分区调度。
 *
 * 每个周期（PERIOD_US）的时间线由开机时算好的槽表决定：
 *   - Tier 1 槽：[周期起点 + 槽偏移, + 槽长)。槽前 GUARD_US 把窗口任务停下，
 *     内核在出口原地等到槽起点再 eret，所以槽主人每个周期在同一个 tick 开始；
 *     槽尾用绝对比较值掐断。槽主人提前做完、或者槽根本没有主人，剩下的时间空转，
 *     不借给任何人。
 *   - 窗口：槽之外的时间。先给 Tier 2 补足最低值，再给建议值（有压力时跳过），
 *     再让 Tier 3 按 T3_QUANTUM_US 轮转，最后空转。
 * 所有期限都是绝对的：开机零点 + k * 周期 + 偏移，不从“现在”往后推，所以不会漂。
 *
 * 记账一律用 CNTVCT 原始 tick。任务一次运行 = [出口 eret 前的时间戳, 下一次异常入口的时间戳)，
 * 内核一次停留 = [入口, 出口)。两段首尾相接，整条时间线没有漏记。
 */
#include "quotaos.h"

#define TIMER_PPI   27
#define MISS_LOG    64
#define JUMP_LOG    64
/* 出口尾巴从读计数器到 eret 是 10 条指令：剩余预算比这还短，比较值会在 eret 之前就到，
 * 中断要等 eret 之后才进来（v012 前实测进入晚到 5–11 tick）。这种情况把期限推到
 * 读计数器后 MIN_REL_T，换成预算最多多跑几 tick（计入 tier-2 overrun max）。 */
#define MIN_REL_T 12
#define EXIT_MARGIN_T 700   /* 挑选到 eret 最多要这么多 tick，期限更近就先等；v015 经转发服务端 icount 实测 545，原 500 不够 */

extern volatile uint64_t k_entry_ts, k_eret_ts, k_release_at, k_spin_late;
extern volatile uint64_t k_arm_abs, k_arm_rel, k_cval;

static struct task tasks[MAX_TASKS];
static uint8_t stacks[MAX_TASKS][STACK_SIZE] __attribute__((aligned(16)));
static struct task *cur;
static struct task *idle_task;
static struct task *shell_task;
static int rr2, rr3;
/* 就绪队列不按状态分，只按级别分：每次挑选只扫这一级的任务。 */
static struct task *lst2[MAX_TASKS], *lst3[MAX_TASKS];
static int n2, n3;

static void rebuild_lists(void)
{
    n2 = n3 = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_EMPTY || t->state == TASK_DEAD)
            continue;
        if (t->tier == TIER2)
            lst2[n2++] = t;
        else if (t->tier == TIER3)
            lst3[n3++] = t;
    }
    rr2 = rr3 = 0;
}

static uint64_t P, G, Q3, SR_T, JUMP_T;
static uint64_t epoch, pk, pstart;
static int nslots;
static uint64_t slot_off[MAX_SLOTS], slot_len[MAX_SLOTS];
static const char *slot_name[MAX_SLOTS];
static struct task *slot_owner[MAX_SLOTS];

/* 上一段内核停留 */
static uint64_t prev_entry;
static int prev_guard;          /* 上一段停留以等待槽起点结束 */
static uint64_t prev_release;

/* 全局统计。[0] 开机以来，[1] 当前演示窗口。 */
struct kstat {
    uint64_t periods;
    uint64_t k_max;             /* 单周期内核 tick 最多 */
    uint64_t k_sum;
    uint64_t k_over;            /* 超过系统预留的周期数 */
    uint64_t foreign_ticks;     /* Tier 2/3 落进 Tier 1 槽的 tick */
    uint64_t foreign_runs;
    uint64_t foreign_net;       /* 去掉时钟跳动覆盖的部分 */
    uint64_t slot_idle[MAX_SLOTS];
    uint64_t slot_owner_used[MAX_SLOTS];
    uint64_t slot_foreign[MAX_SLOTS];
    char slot_who[MAX_SLOTS][NAME_LEN];   /* 窗口开始时的槽主人 */
    uint64_t k_max_period;
    uint64_t irq_lat_min, irq_lat_max;  /* 定时器中断：入口 - 比较值 */
    uint64_t irq_n;
    uint64_t sw_min, sw_max;            /* 不含等待的内核停留：入口到 eret */
    uint64_t minrel_n;                  /* 剩余预算短于出口尾巴、期限推到 MIN_REL_T 的次数 */
    uint64_t guard_path_max;            /* 守卫中断入口到开始等待 */
    uint64_t guard_slack_min;           /* 开始等待时离槽起点还有多少 */
    uint64_t guard_n;
    uint64_t spin_hist[4];              /* 等待循环退出晚 0/1/2/>2 tick */
    uint64_t jumps, jump_ticks;
    uint64_t miss_a, miss_b;
    uint64_t skipped;
    uint64_t past_n, past_max;          /* 出口写进去的比较值已经过了：期限在内核里错过 */
    uint64_t exit_max;                  /* 挑选用的 now 到 eret */
    uint64_t t2_over_max;               /* Tier 2 预算超出上限的最大 tick */
    uint64_t t2_over_n;
};
static struct kstat ks[2];
static int rec;                 /* 1 = 正在记演示窗口 */
static uint64_t k_this_period;

static struct {
    uint64_t a, b;
} jlog[JUMP_LOG];
static uint32_t jn;

struct miss_rec {
    int task;
    uint64_t period;
    int reason;                 /* 0 late 1 unfinished 2 not_started 3 skipped */
    int started;
    uint64_t start_t, lat_t, deadline_t, j_t, unc_t;
    int covered;
    char cls;
};
static struct miss_rec mlog[MISS_LOG];
static uint32_t mn;
/* 前 MISS_LOG 条之后的 class b 另存，保证每条疑似内核问题的原始字段都能打出来 */
#define MISS_LOG_B 16
static struct miss_rec blog[MISS_LOG_B];
static uint32_t bn;

static int bench_state;
static uint32_t bench_left;

struct task *sched_current(void) { return cur; }
uint64_t sched_kernel_max_all(void) { return ks[0].k_max; }
uint64_t sched_kernel_max_period(void) { return ks[0].k_max_period; }

/* v015：内核每周期用量按进入原因拆开，记下开机以来最大那个周期的明细。 */
static int cur_label;
static int prev_kind = 4;
static uint64_t kc_t[KC_N], kc_n[KC_N], kb_t, kspin_t, pend_b, pend_spin;
static struct kattr kworst;
void sched_set_label(int l) { cur_label = l; }
void sched_kernel_worst(struct kattr *a) { *a = kworst; }
uint64_t sched_irq_lat_max(void) { return ks[1].irq_lat_max; }
uint64_t sched_periods(void) { return ks[0].periods; }
uint64_t sched_foreign_window(void) { return ks[1].foreign_ticks; }

static void kstat_reset(struct kstat *s)
{
    memset(s, 0, sizeof *s);
    s->irq_lat_min = ~0ull;
    s->sw_min = ~0ull;
    s->guard_slack_min = ~0ull;
}

/* ---------- 时钟跳动 ---------- */

static void jump_add(uint64_t a, uint64_t b)
{
    if (b <= a)
        return;
    jlog[jn % JUMP_LOG].a = a;
    jlog[jn % JUMP_LOG].b = b;
    jn++;
    ks[0].jumps++;
    ks[0].jump_ticks += b - a;
    if (rec) {
        ks[1].jumps++;
        ks[1].jump_ticks += b - a;
    }
}

/* [t0, t1) 里被时钟跳动覆盖的总长。 */
static uint64_t jump_total(uint64_t t0, uint64_t t1)
{
    uint64_t sum = 0;
    uint32_t first = jn > JUMP_LOG ? jn - JUMP_LOG : 0;
    for (uint32_t i = first; i < jn; i++) {
        uint64_t a = jlog[i % JUMP_LOG].a, b = jlog[i % JUMP_LOG].b;
        if (a < t0)
            a = t0;
        if (b > t1)
            b = t1;
        if (b > a)
            sum += b - a;
    }
    return sum;
}

/* ---------- 错过记录 ---------- */

static void miss(struct task *t, int reason, uint64_t done)
{
    t->misses++;
    t->w.misses += rec;
    if (t->tier != TIER1)
        return;
    /* J = 从上一个周期起点到这次作业完成（或判定错过）这段里时钟跳动的总长。 */
    uint64_t ps = epoch + t->job_period * P;
    uint64_t lat = done > t->release_ts ? done - t->release_ts : 0;
    uint64_t j = jump_total(ps >= epoch + P ? ps - P : epoch, done);
    int covered = 0;
    uint64_t unc = 0;
    char cls;
    if (reason == 3) {
        uint64_t cov = jump_total(ps, ps + P);
        unc = cov < P ? P - cov : 0;   /* 这个周期里没被跳动盖住的长度，原始数据，照打 */
        covered = !unc;
        cls = covered ? 'a' : 'b';
    } else {
        cls = (lat > j && lat - j > t->deadline_t) ? 'b' : 'a';
    }
    if (cls == 'a') {
        ks[0].miss_a++;
        ks[1].miss_a += rec;
    } else {
        ks[0].miss_b++;
        ks[1].miss_b += rec;
    }
    if (!rec)
        return;
    struct miss_rec *m = 0;
    if (mn < MISS_LOG)
        m = &mlog[mn];
    else if (cls == 'b') {
        if (bn < MISS_LOG_B)
            m = &blog[bn];
        bn++;
    }
    mn++;
    if (!m)
        return;
    m->task = t->id;
    m->period = t->job_period;
    m->reason = reason;
    m->started = t->job_started;
    m->start_t = t->job_started && t->start_ts ? t->start_ts - t->release_ts : 0;
    m->lat_t = lat;
    m->deadline_t = t->deadline_t;
    m->j_t = j;
    m->covered = covered;
    m->unc_t = unc;
    m->cls = cls;
}

/* ---------- 任务 ---------- */

struct task *sched_task_at(int index)
{
    if (index < 0 || index >= MAX_TASKS || tasks[index].state == TASK_EMPTY)
        return 0;
    return &tasks[index];
}

struct task *sched_find(const char *name)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state != TASK_EMPTY && tasks[i].state != TASK_DEAD &&
            strcmp(tasks[i].name, name) == 0)
            return &tasks[i];
    return 0;
}

static void spin_entry(void)
{
    for (;;)
        asm volatile("" ::: "memory");
}

/*
 * QEMU 8.2.2 -icount sleep=off 在 WFI 里不把虚拟时间推到下一个定时器。
 * 空槽进了 idle 就再也不会有定时器中断，设备验证停在第一条作业前。
 * icount 下改成空转，计数器仍按指令走，比较值到了中断照常进来。
 * 普通时钟仍用 WFI。槽尾、保护区和准入都没改。
 */
static void idle_entry(void)
{
    if (clock_is_icount()) {
        for (;;)
            asm volatile("" ::: "memory");
    }
    for (;;)
        asm volatile("wfi");
}

static void win_reset(struct task *t)
{
    memset(&t->w, 0, sizeof t->w);
    t->bw_w_periods = t->bw_w_stalled = t->bw_w_over_max = t->bw_w_after = 0;
    t->bw_w_used_max = 0;
    t->bw_w_used_min = ~0ull;
    t->bw_w_restore_min = ~0ull;
    t->w.used_min = ~0ull;
    t->w.start_min = ~0ull;
}

struct task *task_create(const char *name, int tier, int kind)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_EMPTY && tasks[i].state != TASK_DEAD)
            continue;
        struct task *t = &tasks[i];
        memset(t, 0, sizeof *t);
        t->id = i;
        t->stack = stacks[i];
        t->tier = tier;
        t->kind = kind;
        t->slot = -1;
        str_copy(t->name, name, NAME_LEN);
        uintptr_t top = ((uintptr_t)t->stack + STACK_SIZE) & ~(uintptr_t)15u;
        struct irq_frame *f = (struct irq_frame *)(top - sizeof(struct irq_frame));
        memset(f, 0, sizeof *f);
        f->elr = kind == KIND_JOB ? (uint64_t)job_resume :
                 kind == KIND_IDLE ? (uint64_t)idle_entry : (uint64_t)spin_entry;
        f->spsr = 0x345; /* EL1h，只开 IRQ */
        t->sp = (uint64_t)f;
        win_reset(t);
        t->state = TASK_BLOCKED; /* 参数填好后由 task_start 放行 */
        return t;
    }
    printf("PANIC: out of tasks\n");
    for (;;)
        asm volatile("wfi");
}

static void set_entry(struct task *t, void (*entry)(void))
{
    ((struct irq_frame *)t->sp)->elr = (uint64_t)entry;
}

void task_set_entry(struct task *t, void (*entry)(void))
{
    set_entry(t, entry);
}

void task_arm_el0(struct task *t, uint64_t elr, uint64_t user_sp,
                  uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5)
{
    struct irq_frame *f = (struct irq_frame *)t->sp;
    f->elr = elr;
    f->spsr = 0x340;
    f->pad = user_sp;
    f->r[0] = a0;
    f->r[1] = a1;
    f->r[2] = a2;
    f->r[3] = a3;
    f->r[4] = a4;
    f->r[5] = a5;
}

static void kill_locked(struct task *t);

void task_dead(struct task *t)
{
    if (!t)
        return;
    kill_locked(t);
    rebuild_lists();
}

/* IRQs stay as the caller left them. */
void task_ready(struct task *t)
{
    if (t->kind != KIND_JOB)
        t->state = TASK_READY;
    rebuild_lists();
}

/* 死循环型立刻就绪；周期作业等下一次释放。 */
void task_start(struct task *t)
{
    asm volatile("msr daifset, #2" ::: "memory");
    task_ready(t);
    asm volatile("msr daifclr, #2" ::: "memory");
}

static void kill_locked(struct task *t)
{
    t->state = TASK_DEAD;
    t->job_open = 0;
    if (t->slot >= 0 && slot_owner[t->slot] == t)
        slot_owner[t->slot] = 0;
    t->slot = -1;
}

/* 只在任务上下文里调用（shell）。 */
void task_kill(struct task *t)
{
    if (!t)
        return;
    asm volatile("msr daifset, #2" ::: "memory");
    kill_locked(t);
    rebuild_lists();
    asm volatile("msr daifclr, #2" ::: "memory");
}

void sched_bind_slot(struct task *t, int slot)
{
    t->slot = slot;
    slot_owner[slot] = t;
    t->deadline_t = slot_len[slot];
}

struct task *sched_slot_owner(int slot)
{
    return slot >= 0 && slot < nslots ? slot_owner[slot] : 0;
}

/* ---------- 记账 ---------- */

static uint64_t ovl(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    uint64_t lo = a > c ? a : c, hi = b < d ? b : d;
    return hi > lo ? hi - lo : 0;
}

/* 一次运行 [a, b) 和槽的重叠。只查 a 所在周期和下一个周期。 */
static void slot_overlap(struct task *t, uint64_t a, uint64_t b)
{
    if (a < epoch)
        return;
    uint64_t ps = epoch + ((a - epoch) / P) * P;
    uint64_t hit = 0;
    for (int k = 0; k < 2; k++, ps += P)
        for (int i = 0; i < nslots; i++) {
            uint64_t s = ps + slot_off[i];
            uint64_t o = ovl(a, b, s, s + slot_len[i]);
            if (!o)
                continue;
            if (t == idle_task) {
                ks[0].slot_idle[i] += o;
                ks[1].slot_idle[i] += rec ? o : 0;
            } else if (t->tier == TIER1 && t->slot == i) {
                ks[0].slot_owner_used[i] += o;
                ks[1].slot_owner_used[i] += rec ? o : 0;
            } else {
                hit += o;
                ks[0].slot_foreign[i] += o;
                ks[1].slot_foreign[i] += rec ? o : 0;
                uint64_t lo = a > s ? a : s;
                uint64_t net = o - jump_total(lo, lo + o);
                ks[0].foreign_net += net;
                ks[1].foreign_net += rec ? net : 0;
            }
        }
    if (hit) {
        ks[0].foreign_ticks += hit;
        ks[0].foreign_runs++;
        if (rec) {
            ks[1].foreign_ticks += hit;
            ks[1].foreign_runs++;
        }
    }
}

static struct ipc_stats ipc;

static void over_check(struct task *x)
{
    if (x->tier != TIER2 || x->used_t <= x->run_cap)
        return;
    uint64_t o = x->used_t - x->run_cap;
    for (int i = 0; i <= rec; i++) {
        ks[i].t2_over_n++;
        if (o > ks[i].t2_over_max)
            ks[i].t2_over_max = o;
    }
}

static void charge(struct task *t, uint64_t a, uint64_t b)
{
    if (!t || b <= a)
        return;
    slot_overlap(t, a, b);
    if (t == idle_task)
        return;
    struct task *x = t->sc ? t->sc : t;
    /* 服务端在替调用者干活：记在调用者的预算上，服务端自己的不动 */
    x->used_t += b - a;
    x->used_total += b - a;
    if (t->sc) {
        t->call_ran_t += b - a;
        x->p_run += b - a;
        ipc.run_cum += b - a;
    } else {
        x->p_own += b - a;
    }
    over_check(x);
    if (t->bw_rate) {
        /* 模拟访存计数：这段运行的 tick * 访存密度，记在预算主人头上 */
        if (x->bw_hit)
            x->bw_w_after += rec;           /* 停住之后同一周期又跑了：不该发生 */
        uint64_t acc = (b - a) * t->bw_rate + x->bw_frac;
        x->bw_used += acc / 1000;
        x->bw_frac = acc % 1000;
        if (x->bw_budget && x->bw_used >= x->bw_budget)
            x->bw_hit = 1;
    }
}

/* 带宽预算还能跑多少 tick；用完返回 0（停到下个周期）；不限返回 ~0。 */
static uint64_t bw_room(struct task *t)
{
    if (!t->bw_budget || !t->bw_rate)
        return ~0ull;
    if (t->bw_used >= t->bw_budget)
        return 0;
    uint64_t need = (t->bw_budget - t->bw_used) * 1000;
    need = need > t->bw_frac ? need - t->bw_frac : 0;
    return (need + t->bw_rate - 1) / t->bw_rate;
}

static uint64_t inflight_run(uint64_t *calls)
{
    uint64_t sum = 0, n = 0;
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].kind == KIND_SERVER && tasks[i].caller && tasks[i].state != TASK_EMPTY &&
            tasks[i].state != TASK_DEAD) {
            sum += tasks[i].call_ran_t + tasks[i].call_sub_ran;
            n++;
        }
    if (calls)
        *calls = n;
    return sum;
}

static void ipc_snap(uint64_t *v)
{
    v[0] = ipc.run_cum;
    v[1] = ipc.kern_in;
    v[2] = ipc.kern_entry;
    v[3] = ipc.kern_reply;
}

void sched_ipc_stats(struct ipc_stats *s, int reset)
{
    asm volatile("msr daifset, #2" ::: "memory");
    *s = ipc;
    s->t_read = ticks_now();
    if (reset) {
        memset(&ipc, 0, sizeof ipc);
        ipc.t_reset = ticks_now();
        ipc.infl_reset_run = inflight_run(0);
    }
    asm volatile("msr daifclr, #2" ::: "memory");
}

static void kernel_stint(uint64_t a, uint64_t b, int guard)
{
    if (b <= a)
        return;
    k_this_period += b - a;
    int k = guard ? KC_GUARD : prev_kind;
    kc_t[k] += b - a;
    kc_n[k]++;
    kb_t += pend_b;
    kspin_t += pend_spin;
    pend_b = pend_spin = 0;
    if (!guard) {
        uint64_t d = b - a;
        for (int i = 0; i <= rec; i++) {
            if (d < ks[i].sw_min)
                ks[i].sw_min = d;
            if (d > ks[i].sw_max)
                ks[i].sw_max = d;
        }
    }
}

/* ---------- 周期边界 ---------- */

static void job_done(struct task *t, uint64_t at)
{
    if (!t->job_open)
        return;
    t->job_open = 0;
    t->jobs++;
    t->w.jobs += rec;
    uint64_t lat = at - t->release_ts;
    if (rec && lat > t->w.lat_max)
        t->w.lat_max = lat;
    if (lat > t->deadline_t)
        miss(t, 0, at);
}

/* 槽尾已过、作业还开着：这一刻就判错过，完成时刻记为内核看到它的这一刻。 */
static void check_slot_ends(uint64_t seen, uint64_t now)
{
    for (int i = 0; i < nslots; i++) {
        struct task *o = slot_owner[i];
        if (!o || o->kind != KIND_JOB || !o->job_open)
            continue;
        if (now < pstart + slot_off[i] + slot_len[i])
            continue;
        o->jobs++;
        o->w.jobs += rec;
        miss(o, o->job_started ? 1 : 2, seen);
        o->job_open = 0;
    }
}

static void end_period(uint64_t now, int skipped)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_EMPTY || t->state == TASK_DEAD || t == idle_task)
            continue;
        if (t->kind == KIND_JOB && t->job_open) {
            t->jobs++;
            t->w.jobs += rec;
            int reason = skipped ? 3 : t->job_started ? 1 : 2;
            if (skipped) {
                ks[0].skipped++;
                ks[1].skipped += rec;
            }
            miss(t, reason, now);
            t->job_open = 0;
        }
        if (t->used_t > t->used_max_all)
            t->used_max_all = t->used_t;
        if (rec) {
            t->w.periods++;
            t->w.used_sum += t->used_t;
            if (t->used_t < t->w.used_min)
                t->w.used_min = t->used_t;
            if (t->used_t > t->w.used_max)
                t->w.used_max = t->used_t;
            if (t->tier == TIER2 && t->kind == KIND_SPIN && t->used_t < t->min_t)
                t->w.short_periods++;
        }
        if (rec) {
            ipc.w_used += t->used_t;
            ipc.w_own += t->p_own;
            ipc.w_run += t->p_run;
            ipc.w_kin += t->p_kin;
            ipc.w_kent += t->p_kent;
            ipc.w_krep += t->p_krep;
        }
        if (rec && t->p_hr > ipc.hr_pp_max)
            ipc.hr_pp_max = t->p_hr;
        if (rec && t->p_hr) {
            ipc.hr_periods++;
            if (t->p_hr_pass == 3)
                ipc.hr_both_periods++;
            if (t->p_hr_n >= 2)
                ipc.hr_multi_periods++;
        }
        t->p_own = t->p_run = t->p_kin = t->p_kent = t->p_krep = t->p_hr = 0;
        t->p_hr_pass = 0;
        t->p_hr_n = 0;
        t->ipc_hold = 0;
        t->used_t = 0;
        t->dev_used_t = 0;
        if (t->bw_budget) {
            if (rec) {
                t->bw_w_periods++;
                if (t->bw_used > t->bw_w_used_max)
                    t->bw_w_used_max = t->bw_used;
                if (t->bw_used < t->bw_w_used_min)
                    t->bw_w_used_min = t->bw_used;
                if (t->bw_hit) {
                    t->bw_w_stalled++;
                    if (t->bw_used - t->bw_budget > t->bw_w_over_max)
                        t->bw_w_over_max = t->bw_used - t->bw_budget;
                }
                if (t->bw_prev_hit && t->bw_used < t->bw_w_restore_min)
                    t->bw_w_restore_min = t->bw_used;
            }
            t->bw_prev_hit = t->bw_hit;
        }
        /* 不欠账：超出的部分不带到下个周期 */
        t->bw_used = 0;
        t->bw_frac = 0;
        t->bw_hit = 0;
    }
    for (int i = 0; i <= rec; i++) {
        ks[i].periods++;
        ks[i].k_sum += k_this_period;
        if (k_this_period > ks[i].k_max) {
            ks[i].k_max = k_this_period;
            ks[i].k_max_period = pk;
            if (i == 0) {
                kworst.period = pk;
                kworst.label = cur_label;
                kworst.total = k_this_period;
                for (int j = 0; j < KC_N; j++) {
                    kworst.t[j] = kc_t[j];
                    kworst.n[j] = kc_n[j];
                }
                kworst.boundary = kb_t;
                kworst.spin = kspin_t;
            }
        }
        if (k_this_period > SR_T)
            ks[i].k_over++;
    }
    k_this_period = 0;
    for (int j = 0; j < KC_N; j++)
        kc_t[j] = kc_n[j] = 0;
    kb_t = kspin_t = 0;
}

static void finish_bench(void)
{
    ipc.infl_end_run = inflight_run(&ipc.infl_end_calls);
    dev_unbind_locked();
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].bench_owned && tasks[i].state != TASK_EMPTY)
            kill_locked(&tasks[i]);
    rebuild_lists();
    /* The serial menu parks the shell. A GUI window does not. */
    if (shell_task->wait_bench) {
        shell_task->wait_bench = 0;
        shell_task->state = TASK_READY;
    }
}

static void ipc_release_held(void);

static void begin_period(void)
{
    dev_period();
    ipc_release_held();
    if (bench_state == 1) {
        rec = 1;
        kstat_reset(&ks[1]);
        for (int i = 0; i < nslots; i++)
            str_copy(ks[1].slot_who[i], slot_owner[i] ? slot_owner[i]->name : "(empty)", NAME_LEN);
        mn = 0;
        bn = 0;
        /* v015：空槽/已退出的任务不用清（task_create 会清），省掉窗口起点那一周期
           一大段边界工作（v014 qa 开机以来最大内核周期就落在这里） */
        for (int i = 0; i < MAX_TASKS; i++)
            if (tasks[i].state != TASK_EMPTY && tasks[i].state != TASK_DEAD)
                win_reset(&tasks[i]);
        bench_state = 2;
        ipc_snap(ipc.snap0);
        ipc.t_w0 = pstart;
        ipc.p_w0 = pk;
    } else if (bench_state == 2) {
        if (--bench_left == 0) {
            ipc_snap(ipc.snap1);
            ipc.t_w1 = pstart;
            ipc.p_w1 = pk - 1;
            rec = 0;
            bench_state = 0;
            finish_bench();
        }
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_EMPTY || t->state == TASK_DEAD || t->kind != KIND_JOB)
            continue;
        if (t->tier == TIER1 && t->slot < 0)
            continue;
        t->job_open = 1;
        t->job_started = 0;
        t->job_period = pk;
        t->release_ts = pstart + (t->tier == TIER1 ? slot_off[t->slot] : 0);
        if (t->state == TASK_BLOCKED)
            t->state = TASK_READY;
    }
}

/* ---------- 挑选 ---------- */

/* 调用者在等 IPC 回复时，它的预算由服务端来用。 */
/* 沿调用链走到真正要跑的那个任务（v015 起可以是 调用者 -> 服务端 A -> 服务端 B）。 */
static struct task *chain_end(struct task *t)
{
    for (int n = 0; n < 8 && t->state == TASK_IPC; n++) {
        struct task *s = t->server;
        if (!s || s->caller != t)
            return 0;
        t = s;
    }
    return t->state == TASK_READY ? t : 0;
}

static int runnable(struct task *t)
{
    return chain_end(t) != 0;
}

static struct task *runner(struct task *t)
{
    return chain_end(t);
}

static int k_ipc_op;
static uint64_t disp_now, cur_entry;
/* 上一段内核停留是 IPC 系统调用：记在这个预算上（MCS：调用路径用调用者的预算），不算系统预留 */
static struct task *prev_stint_sc, *prev_stint_srv;
static int prev_stint_reply;      /* 这一段是回复返回（svc #2 之后） */

static struct task *pick_t2(int sugg)
{
    if (sugg && resource_pressure())
        return 0;
    for (int n = 1; n <= n2; n++) {
        int i = rr2 + n;
        if (i >= n2)
            i -= n2;
        struct task *t = lst2[i];
        if (!runnable(t))
            continue;
        uint64_t cap = t->min_t + (sugg ? t->sugg_t : 0);
        /* 这一段 IPC 内核停留稍后记在 t 头上：按最多到 eret 的长度先扣掉 */
        uint64_t used = t->used_t;
        if (prev_stint_sc == t)
            used += disp_now + EXIT_MARGIN_T - cur_entry;
        uint64_t room = bw_room(t);
        if (used < cap && room) {
            rr2 = i;
            t->run_cap = cap;
            k_arm_rel = cap - used;
            if (room < k_arm_rel)
                k_arm_rel = room;
            return runner(t);
        }
    }
    return 0;
}

static struct task *pick_t3(void)
{
    for (int n = 1; n <= n3; n++) {
        int i = rr3 + n;
        if (i >= n3)
            i -= n3;
        struct task *t = lst3[i];
        uint64_t room = bw_room(t);
        if (runnable(t) && room) {
            rr3 = i;
            k_arm_rel = Q3 < room ? Q3 : room;
            return runner(t);
        }
    }
    return 0;
}

/* now 之后本周期内最近的期限：周期边界、各槽保护区起点和槽尾。 */
static uint64_t next_event(uint64_t now)
{
    uint64_t ev = pstart + P;
    for (int i = 0; i < nslots; i++) {
        uint64_t g = pstart + slot_off[i] - G;
        uint64_t e = pstart + slot_off[i] + slot_len[i];
        if (now < g && g < ev)
            ev = g;
        if (now < e && e < ev)
            ev = e;
    }
    uint64_t dd = dev_deadline();
    if (now < dd && dd < ev)
        ev = dd;
    return ev;
}

static struct task *dispatch(uint64_t now)
{
    k_arm_rel = 0;
    k_release_at = 0;
    prev_guard = 0;
    for (int i = 0; i < nslots; i++) {
        uint64_t s = pstart + slot_off[i];
        uint64_t e = s + slot_len[i];
        if (now + G < s || now >= e)
            continue;
        /* 守卫或槽内：只有槽主人能上，否则空转到槽尾。设备中断线在这里屏蔽。 */
        k_arm_abs = e;
        dev_slot(1);
        if (now < s && dev_busy())
            dev_t1_busy();
        struct task *o = slot_owner[i];
        if (!o || o->state != TASK_READY)
            return idle_task;
        if (now < s) {
            k_release_at = s;
            prev_release = s;
            prev_guard = 1;
            uint64_t t = ticks_now();
            for (int r = 0; r <= rec; r++) {
                ks[r].guard_n++;
                if (t - k_entry_ts > ks[r].guard_path_max)
                    ks[r].guard_path_max = t - k_entry_ts;
                uint64_t slack = s > t ? s - t : 0;
                if (slack < ks[r].guard_slack_min)
                    ks[r].guard_slack_min = slack;
            }
        }
        return o;
    }
    uint64_t hard = pstart + P;
    for (int i = 0; i < nslots; i++) {
        uint64_t g = pstart + slot_off[i] - G;
        if (now < g && g < hard)
            hard = g;
    }
    uint64_t dd = dev_deadline();
    if (now < dd && dd < hard)
        hard = dd;
    k_arm_abs = hard;
    dev_slot(0);
    struct task *t = pick_t2(0);
    if (!t)
        t = pick_t2(1);
    if (!t)
        t = pick_t3();
    if (!t) {
        k_arm_rel = 0;
        t = idle_task;
    } else if (k_arm_rel && k_arm_rel < MIN_REL_T) {
        k_arm_rel = MIN_REL_T;
        for (int r = 0; r <= rec; r++)
            ks[r].minrel_n++;
    }
    return t;
}


/* 服务端开始替调用者 c 干活：从这一刻起它用 c 的预算。 */
static void ipc_begin(struct task *s, struct task *c)
{
    s->caller = c;
    s->sc = c->sc ? c->sc : c;
    s->call_base = s->sc->used_total;   /* 预算主人的累计用量：调用结束时相减就是这次调用扣掉的 */
    s->call_ran_t = 0;
    s->call_kern_t = 0;
    s->call_sub_ran = 0;
    s->call_sub_kern = 0;
    ((struct irq_frame *)s->sp)->r[0] = ((struct irq_frame *)c->sp)->r[1];
    s->state = TASK_READY;
}

/* svc #1：调用（x0 = 服务端任务号，x1 = 请求）；svc #2：服务端回复（x0 = 回复）并等下一次。
 * 服务端忙时调用者排队（状态 TASK_IPC，但不可运行），回复后按任务号轮转接下一个。 */
static int ipc_hold_rule = 1;
static int held_count;           /* 扣住的调用数：0 时周期开头不用扫 */

/* 只给演示 18 的对照用：关掉 v014 的扣住规则 */
void sched_set_ipc_hold_rule(int on)
{
    ipc_hold_rule = on;
}

/* 调用者 t 向服务端 s 发起调用：服务端空闲就接手（返回 1），否则排队。 */
static int ipc_call(struct task *t, struct task *s)
{
    t->state = TASK_IPC;
    t->server = s;
    if (!s->caller && s->state == TASK_BLOCKED) {
        ipc_begin(s, t);
        return 1;
    }
    if (s->ipc_qt - s->ipc_qh < sizeof s->ipc_q) {
        s->ipc_q[s->ipc_qt++ % sizeof s->ipc_q] = (uint8_t)t->id;
        t->ipc_queued = 1;
    } else {
        s->ipc_q_over = 1;
    }
    return 0;
}

/* v014：周期开头放出上个周期扣住的调用 */
static void ipc_release_held(void)
{
    if (!held_count)
        return;
    held_count = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (!t->ipc_held)
            continue;
        t->ipc_held = 0;
        if (t->state != TASK_BLOCKED)
            continue;
        struct irq_frame *f = (struct irq_frame *)t->sp;
        uint64_t id = f->r[0];
        struct task *s = id < MAX_TASKS ? &tasks[id] : 0;
        if (!s || s->kind != KIND_SERVER || s->state == TASK_EMPTY || s->state == TASK_DEAD) {
            f->r[0] = ~0ull;
            t->state = TASK_READY;
            continue;
        }
        ipc.held_resumed++;
        ipc_call(t, s);
    }
}

static void ipc_op(struct task *t, int op)
{
    struct irq_frame *f = (struct irq_frame *)t->sp;
    if (op == 1) {
        uint64_t id = f->r[0];
        struct task *s = id < MAX_TASKS ? &tasks[id] : 0;
        if (!s || s->kind != KIND_SERVER || s->state == TASK_EMPTY || s->state == TASK_DEAD) {
            f->r[0] = ~0ull;    /* 没有这个服务端：调用失败 */
            return;
        }
        prev_stint_sc = t->sc ? t->sc : t;
        /* v014：这个预算本周期已经用过 IPC 余量，新调用扣到下个周期（已经在路上的调用照常做完） */
        /* v015：扣不扣看预算主人（sc），服务端替调用者转发的调用也一样 */
        if (t->kind == KIND_SERVER && t->sc) {
            prev_stint_srv = t;     /* 这段内核路径属于 t 正在处理的那次调用 */
            ipc.fwd_calls++;
        }
        if (ipc_hold_rule && prev_stint_sc->ipc_hold) {
            t->ipc_held = 1;
            held_count++;
            t->state = TASK_BLOCKED;
            ipc.held_n++;
            return;
        }
        if (ipc_call(t, s))
            prev_stint_srv = s;
    } else if (op == 3 && t->kind == KIND_SERVER) {
        /* svc #3：驱动向设备提交作业，经过闸门。放行就等完成中断（或超时）。 */
        prev_stint_sc = t->sc ? t->sc : t;
        if (t->sc)
            prev_stint_srv = t;
        int code = dev_submit(t, f->r[0], f->r[1], ticks_now());
        if (code == 0)
            t->state = TASK_BLOCKED;
        else
            f->r[0] = (1ull << 62) | (uint64_t)code;
    } else if (op == 2 && t->kind == KIND_SERVER) {
        struct task *c = t->caller;
        if (c) {
            struct task *owner = t->sc;
            prev_stint_sc = owner;
            uint64_t drop = owner->used_total - t->call_base;
            uint64_t ran = t->call_ran_t + t->call_sub_ran;
            uint64_t m = ran + t->call_kern_t + t->call_sub_kern;
            uint64_t d = drop > m ? drop - m : m - drop;
            if (d > ipc.diff_max)
                ipc.diff_max = d;
            if (c->kind == KIND_SERVER && c->sc == owner) {
                /* 转发的调用：算进外层那次调用，回复这段内核路径也属于外层调用 */
                c->call_sub_ran += ran;
                c->call_sub_kern += t->call_kern_t + t->call_sub_kern;
                prev_stint_srv = c;
                ipc.fwd_replies++;
            } else {
                prev_stint_reply = 1;
                ipc.run_sum += ran;
                ipc.calls++;
                ipc.drop_sum += drop;
                ipc.meas_sum += m;
                ipc.self_sum += f->r[0];
                if (m > ipc.meas_max)
                    ipc.meas_max = m;
            }
            ((struct irq_frame *)c->sp)->r[0] = f->r[0];
            c->server = 0;
            c->state = TASK_READY;
            t->caller = 0;
            t->sc = 0;
        }
        t->state = TASK_BLOCKED;
        /* 下一个排队的调用者。内核路径要短：槽前保护区要盖住进内核前已开始的最长路径。 */
        while (t->ipc_qh != t->ipc_qt) {
            struct task *q = &tasks[t->ipc_q[t->ipc_qh++ % sizeof t->ipc_q]];
            if (q->ipc_queued && q->state == TASK_IPC && q->server == t) {
                q->ipc_queued = 0;
                ipc_begin(t, q);
                return;
            }
        }
        if (t->ipc_q_over) {
            t->ipc_q_over = 0;
            int from = c ? c->id : 0;
            for (int n = 1; n <= MAX_TASKS; n++) {
                struct task *q = &tasks[(from + n) % MAX_TASKS];
                if (q->state == TASK_IPC && q->server == t && !q->server->caller) {
                    q->ipc_queued = 0;
                    ipc_begin(t, q);
                    break;
                }
            }
        }
    }
}

static uint64_t schedule(uint64_t sp, int timer)
{
    uint64_t entry = k_entry_ts;
    uint64_t eret = k_eret_ts;
    struct task *prev = cur;

    if (prev_stint_sc && !prev_guard && eret > prev_entry) {
        uint64_t d = eret - prev_entry;
        pend_b = pend_spin = 0;     /* 这段算调用者的，不算内核每周期 */
        struct task *x = prev_stint_sc;
        /* 全部记在调用者头上。Tier 2 超出预算上限的部分（调用者预算在内核路径里用完）
         * 从它准入时多留的 IPC 余量里出（v013 起；v012 及以前改记系统预留）。 */
        uint64_t c = d;
        if (x->tier == TIER2) {
            uint64_t room = x->run_cap > x->used_t ? x->run_cap - x->used_t : 0;
            if (d > room) {
                uint64_t o = d - room;
                ipc.hr_n++;
                ipc.hr_sum += o;
                if (o > ipc.hr_max)
                    ipc.hr_max = o;
                x->p_hr += o;
                x->p_hr_n++;
                x->p_hr_pass |= x->run_cap > x->min_t ? 2 : 1;
                x->ipc_hold = 1;    /* v014：本周期不再接新调用 */
            }
        }
        x->used_t += c;
        x->used_total += c;
        ipc.kern_sum += c;
        if (prev_stint_srv && prev_stint_srv->sc == x) {
            prev_stint_srv->call_kern_t += c;
            ipc.kern_in += c;
            x->p_kin += c;
        } else if (prev_stint_reply) {
            ipc.kern_reply += c;
            x->p_krep += c;
        } else {
            ipc.kern_entry += c;
            x->p_kent += c;
        }
    } else {
        kernel_stint(prev_entry, eret, prev_guard);
    }
    prev_stint_sc = 0;
    prev_stint_srv = 0;
    prev_stint_reply = 0;
    if (prev_guard) {
        uint64_t d = k_spin_late;
        ks[0].spin_hist[d > 2 ? 3 : d]++;
        if (rec)
            ks[1].spin_hist[d > 2 ? 3 : d]++;
        if (d > JUMP_T)
            jump_add(prev_release, prev_release + d); /* 等待期间被宿主机停住 */
    }
    if (k_cval < eret) {
        for (int r = 0; r <= rec; r++) {
            ks[r].past_n++;
            if (eret - k_cval > ks[r].past_max)
                ks[r].past_max = eret - k_cval;
        }
    }
    if (!prev_guard && eret >= disp_now)
        for (int r = 0; r <= rec; r++)
            if (eret - disp_now > ks[r].exit_max)
                ks[r].exit_max = eret - disp_now;
    if (entry >= k_cval) {
        uint64_t l = entry - k_cval;
        for (int r = 0; timer == 1 && r <= rec; r++) {
            ks[r].irq_n++;
            if (l < ks[r].irq_lat_min)
                ks[r].irq_lat_min = l;
            if (l > ks[r].irq_lat_max)
                ks[r].irq_lat_max = l;
        }
        /* 不管是定时器中断还是 svc 进来的：进内核时定时器早该响了，
         * 晚了这么多就是宿主机把时钟往前跳了（svc 抢在迟到的中断前面进来也算）。 */
        if (l > JUMP_T)
            jump_add(k_cval, entry);
    }

    prev->sp = sp;
    charge(prev, eret, entry);
    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    int op = timer ? 0 : k_ipc_op;
    k_ipc_op = 0;
    if (op)
        ipc_op(prev, op);
    if (timer == 2) {
        struct task *srv = 0, *x = dev_irq(entry, &srv);
        if (x && !prev_stint_sc) {
            prev_stint_sc = x;
            prev_stint_srv = srv;
        }
    }
    if (!timer && !op && prev->kind == KIND_JOB && prev->state == TASK_READY) {
        job_done(prev, entry);
        prev->state = TASK_BLOCKED;
    }

    cur_entry = entry;
    /* 第一个结束的周期是刚才待过的；同一次里再结束的周期一刻都没轮到，算跳过。 */
    int skipped = 0;
    for (;;) {
        uint64_t now = ticks_now();
        uint64_t b0 = now;
        int crossed = now >= pstart + P;
        while (now >= pstart + P) {
            if (!skipped)
                check_slot_ends(entry, now);
            end_period(now, skipped);
            skipped = 1;
            pk++;
            pstart += P;
            begin_period();
        }
        if (crossed)
            pend_b += ticks_now() - b0;
        check_slot_ends(entry, now);
        {
            struct task *srv = 0, *x = dev_poll(now, &srv);
            if (x && !prev_stint_sc) {
                prev_stint_sc = x;
                prev_stint_srv = srv;
            }
        }
        /* 下一个期限（槽前保护区、槽尾、周期边界）离现在不到出口所需的时间：
         * 在内核里等到它再挑，免得写进比较值时期限已经过了。 */
        disp_now = ticks_now();
        uint64_t ev = next_event(disp_now);
        if (ev > disp_now && ev - disp_now >= EXIT_MARGIN_T)
            break;
        uint64_t s0 = ticks_now();
        while (ticks_now() < ev)
            asm volatile("" ::: "memory");
        pend_spin += ticks_now() - s0;
    }
    struct task *next = dispatch(disp_now);
    if (next->kind == KIND_JOB && next->job_open && !next->job_started) {
        next->job_started = 1;
        next->start_ts = 0;
    }
    next->state = TASK_RUNNING;
    cur = next;
    prev_entry = entry;
    prev_kind = timer == 1 ? KC_TIMER : timer == 2 ? KC_DEVIRQ : op ? KC_SVC_IPC : KC_SVC_OTHER;
    proc_on_switch(next);
    return next->sp;
}

/* 作业任务 eret 后第一条指令读到的时间由 boot/vectors.S 传进来。 */
void job_body(uint64_t start)
{
    struct task *t = cur;
    /* start 早于释放时刻：这是上一次作业读到的时间（中途被掐断），不算新作业的起点。 */
    if (t->job_open && t->start_ts == 0 && start >= t->release_ts) {
        t->start_ts = start;
        uint64_t off = start - t->release_ts;
        if (rec && t->tier == TIER1) {
            if (off < t->w.start_min)
                t->w.start_min = off;
            if (off > t->w.start_max)
                t->w.start_max = off;
        }
    }
    uint64_t end = start + t->work_t;
    while (ticks_now() < end)
        asm volatile("" ::: "memory");
}

uint64_t handle_irq(uint64_t frame_sp)
{
    uint32_t iar = gic_ack();
    uint32_t id = iar & 0x3ffu;
    if (id < 1020)
        gic_eoi(iar);
    if (id == DEV_IRQ_ID)
        return schedule(frame_sp, 2);
    if (id != TIMER_PPI) {
        k_arm_rel = 0;
        k_arm_abs = k_cval;
        return frame_sp;
    }
    return schedule(frame_sp, 1);
}

uint64_t handle_sync(uint64_t frame_sp)
{
    struct irq_frame *f = (struct irq_frame *)frame_sp;
    uint64_t esr;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    uint32_t ec = (uint32_t)((esr >> 26) & 0x3f);
    if ((f->spsr & 0xfu) == 0) {
        k_ipc_op = 0;
        if (ec == 0x15)
            proc_el0_svc(f);
        else
            proc_el0_abort(f, esr);
        return schedule(frame_sp, 0);
    }
    if (ec == 0x15) {
        k_ipc_op = (int)(esr & 0xffff);
        return schedule(frame_sp, 0);
    }
    if ((dtb_fault_expected() || mmu_fault_expected()) && (ec == 0x24 || ec == 0x25)) {
        if (dtb_fault_expected())
            dtb_note_fault();
        else
            mmu_note_fault();
        f->elr += 4;
        return frame_sp;
    }
    uint64_t far = 0;
    asm volatile("mrs %0, far_el1" : "=r"(far));
    printf("\nPANIC sync ec=0x%x esr=0x%lx elr=0x%lx far=0x%lx\n", ec, esr, f->elr, far);
    for (;;)
        asm volatile("wfi");
}

void handle_unhandled(uint64_t frame_sp, uint64_t kind)
{
    struct irq_frame *f = (struct irq_frame *)frame_sp;
    printf("\nPANIC unhandled vector %lu elr=0x%lx\n", kind, f->elr);
    for (;;)
        asm volatile("wfi");
}

/* ---------- 演示窗口 ---------- */

void sched_bench(uint32_t periods)
{
    asm volatile("msr daifset, #2" ::: "memory");
    bench_left = periods;
    bench_state = 1;
    shell_task->wait_bench = 1;
    shell_task->state = TASK_BLOCKED;
    asm volatile("msr daifclr, #2" ::: "memory");
    asm volatile("svc #0" ::: "memory");
    while (shell_task->wait_bench)
        asm volatile("svc #0" ::: "memory");
}

int sched_bench_async(uint32_t periods)
{
    int rc = 0;
    asm volatile("msr daifset, #2" ::: "memory");
    if (bench_state)
        rc = -1;
    else {
        bench_left = periods;
        bench_state = 1;
    }
    asm volatile("msr daifclr, #2" ::: "memory");
    return rc;
}

int sched_bench_busy(void)
{
    return bench_state != 0;
}

void sched_bench_stop(void)
{
    asm volatile("msr daifset, #2" ::: "memory");
    if (bench_state == 2 && bench_left > 1)
        bench_left = 1;
    asm volatile("msr daifclr, #2" ::: "memory");
}

uint32_t sched_bench_left(void)
{
    return bench_left;
}

static const char *reason_name[] = {"late", "unfinished", "not_started", "skipped"};

static void print_miss(const struct miss_rec *m)
{
    if (m->reason == 3)
        printf("T1MISS task=%s period=%lu reason=skipped lat_us=-\n"
               "  deadline_us=%lu J_us=%lu covered=%s uncovered_us=%lu class=%c\n",
               tasks[m->task].name, m->period, ticks_to_us(m->deadline_t),
               ticks_to_us(m->j_t), m->covered ? "yes" : "no", ticks_to_us(m->unc_t), m->cls);
    else
        printf("T1MISS task=%s period=%lu reason=%s start_t=%ld\n"
               "  lat_t=%lu lat_us=%lu deadline_us=%lu J_us=%lu class=%c\n",
               tasks[m->task].name, m->period, reason_name[m->reason],
               m->started ? (long)m->start_t : -1L, m->lat_t,
               ticks_to_us(m->lat_t), ticks_to_us(m->deadline_t),
               ticks_to_us(m->j_t), m->cls);
}

void sched_print_window(void)
{
    printf("Tier 1 (ticks; 1 tick = 16 ns; start = first job instruction - slot start)\n");
    printf("  task           slot   jobs  miss  start_min  start_max  jitter  lat_max\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_EMPTY || t->tier != TIER1 || t->kind != KIND_JOB || !t->w.jobs)
            continue;
        uint64_t mn0 = t->w.start_min == ~0ull ? 0 : t->w.start_min;
        printf("  ");
        put_padded(t->name, 14);
        printf(" %4d %6u %5u %10lu %10lu %7lu %8lu\n", t->slot, t->w.jobs, t->w.misses,
               mn0, t->w.start_max, t->w.start_max - mn0, t->w.lat_max);
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_EMPTY || t->tier != TIER1 || t->kind != KIND_JOB || !t->w.jobs)
            continue;
        uint64_t want = epoch + t->job_period * P + slot_off[t->slot];
        printf("  %s last release: period %lu, tick %lu,\n    epoch+k*P+offset %lu\n", t->name,
               t->job_period, t->release_ts, want);
    }
    for (uint32_t i = 0; i < mn && i < MISS_LOG; i++)
        print_miss(&mlog[i]);
    if (mn > MISS_LOG)
        printf("T1MISS (%u more not shown; class b among them: %u, listed below)\n",
               mn - MISS_LOG, bn);
    for (uint32_t i = 0; i < bn && i < MISS_LOG_B; i++)
        print_miss(&blog[i]);
    if (bn > MISS_LOG_B)
        printf("T1MISS (%u more class b not shown)\n", bn - MISS_LOG_B);
    printf("misses: class a (clock jump) %lu, class b (kernel) %lu, skipped periods %lu\n",
           ks[1].miss_a, ks[1].miss_b, ks[1].skipped);
}

void sched_print_slots(void)
{
    uint64_t n = ks[1].periods ? ks[1].periods : 1;
    printf("Tier 1 slots over %lu periods (ticks per period, average)\n", ks[1].periods);
    printf("  slot owner          start    len  owner_used  idle  foreign(total)\n");
    for (int i = 0; i < nslots; i++) {
        printf("  %4d ", i);
        put_padded(ks[1].slot_who[i], 14);
        printf(" %6lu %6lu %11lu %5lu %8lu\n", slot_off[i], slot_len[i],
               ks[1].slot_owner_used[i] / n, ks[1].slot_idle[i] / n, ks[1].slot_foreign[i]);
    }
    printf("Tier 2/3 ticks inside any Tier 1 slot: %lu (in %lu runs)\n",
           ks[1].foreign_ticks, ks[1].foreign_runs);
    printf("  of which not covered by clock jumps: %lu\n", ks[1].foreign_net);
}

void sched_print_kernel(void)
{
    struct kstat *s = &ks[1];
    uint64_t n = s->periods ? s->periods : 1;
    printf("kernel per period: max %lu avg %lu ticks, reserve %lu, over %lu\n",
           s->k_max, s->k_sum / n, SR_T, s->k_over);
    printf("timer IRQ entry - CVAL: min %lu max %lu ticks (n=%lu)\n",
           s->irq_n ? s->irq_lat_min : 0, s->irq_lat_max, s->irq_n);
    printf("kernel entry -> eret (no slot wait): min %lu max %lu ticks\n",
           s->sw_min == ~0ull ? 0 : s->sw_min, s->sw_max);
    printf("slot guard: path max %lu ticks, slack min %lu ticks of %lu (n=%lu)\n",
           s->guard_path_max, s->guard_n ? s->guard_slack_min : 0, G, s->guard_n);
    printf("wait loop exit late by 0/1/2/>2 ticks: %lu/%lu/%lu/%lu\n",
           s->spin_hist[0], s->spin_hist[1], s->spin_hist[2], s->spin_hist[3]);
    printf("clock jumps: %lu, total %lu us\n", s->jumps, ticks_to_us(s->jump_ticks));
    printf("deadline already past at eret: %lu times, max %lu ticks;\n",
           s->past_n, s->past_max);
    printf("  pick time -> eret max %lu ticks (margin %lu); tier-2 budget\n",
           s->exit_max, (uint64_t)EXIT_MARGIN_T);
    printf("  overrun max %lu ticks (%lu times)\n", s->t2_over_max, s->t2_over_n);
    printf("  budget left < exit tail, deadline moved to %u ticks: %lu times\n",
           MIN_REL_T, s->minrel_n);
}

/* ---------- 启动 ---------- */

void sched_init(void)
{
    memset(tasks, 0, sizeof tasks);
    P = us_to_ticks(PERIOD_US);
    G = us_to_ticks(GUARD_US);
    Q3 = us_to_ticks(T3_QUANTUM_US);
    SR_T = us_to_ticks(SYS_RESERVE_US);
    JUMP_T = us_to_ticks(JUMP_US);
    kstat_reset(&ks[0]);
    kstat_reset(&ks[1]);
    int n = 0;
    const struct tier1_decl *d = resource_tier1(&n);
    nslots = 0;
    for (int i = 0; i < n && nslots < MAX_SLOTS; i++) {
        if (!d[i].cpu_us)
            continue;
        slot_off[nslots] = us_to_ticks(d[i].slot_off_us);
        slot_len[nslots] = us_to_ticks(d[i].cpu_us);
        slot_name[nslots] = d[i].name;
        nslots++;
    }
    (void)slot_name;
}

void services_start(void)
{
    struct task *t = task_create("display-vsync", TIER1, KIND_JOB);
    t->work_t = us_to_ticks(80);
    sched_bind_slot(t, 0);
    t = task_create("timer-server", TIER1, KIND_JOB);
    t->work_t = us_to_ticks(40);
    sched_bind_slot(t, 1);
}

void sched_start(void (*shell_entry)(void))
{
    idle_task = task_create("idle", TIER_IDLE, KIND_IDLE);
    idle_task->state = TASK_READY;
    shell_task = task_create("shell", TIER3, KIND_SHELL);
    set_entry(shell_task, shell_entry);
    shell_task->state = TASK_RUNNING;
    cur = shell_task;
    rebuild_lists();
    /* 周期零点取下一个整 tick 周期，槽表从这里起算。 */
    uint64_t now = ticks_now();
    epoch = now + P - now % P;
    pk = 0;
    pstart = epoch;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->kind == KIND_JOB && t->tier == TIER1 && t->slot >= 0) {
            t->job_open = 1;
            t->release_ts = pstart + slot_off[t->slot];
            t->state = TASK_READY;
        }
    }
    k_arm_abs = epoch + slot_off[0] - G;
    k_arm_rel = 0;
    printf("[boot] slot table from tick %lu, period %lu ticks, guard %lu ticks\n", epoch, P, G);
    prev_entry = ticks_now();
    k_eret_ts = prev_entry;
    start_first_task(shell_task->sp);
}

/* now 之后最近一个 Tier 1 保护区的起点；now 已经在保护区或槽内就返回 now。
 * 设备闸门用它判断一个作业能不能在下一个 Tier 1 窗口前做完。 */
uint64_t sched_next_guard(uint64_t now)
{
    uint64_t k = now >= epoch ? (now - epoch) / P : 0;
    for (uint64_t kk = k; kk <= k + 1; kk++)
        for (int s = 0; s < nslots; s++) {
            uint64_t g = epoch + kk * P + slot_off[s] - G;
            uint64_t e = epoch + kk * P + slot_off[s] + slot_len[s];
            if (now < g)
                return g;
            if (now < e)
                return now;
        }
    return now;
}

void sched_total_misses(uint64_t *a, uint64_t *b)
{
    *a = ks[0].miss_a;
    *b = ks[0].miss_b;
}

uint64_t sched_period_now(void)
{
    return pk;
}

/* now 在某个 Tier 1 保护区或槽内。 */
int sched_in_t1_window(uint64_t now)
{
    for (int i = 0; i < nslots; i++) {
        uint64_t s = pstart + slot_off[i];
        if (now + G >= s && now < s + slot_len[i])
            return 1;
    }
    return 0;
}

void sched_window_misses(uint64_t *a, uint64_t *b)
{
    *a = ks[1].miss_a;
    *b = ks[1].miss_b;
}
