/*
 * E3：设备闸门、超时强制中止并降级、中断风暴限流。
 *
 * 模拟设备（软件模型，不是真实硬件）的「硬件」用 EL1 物理定时器 CNTP 代替：
 * 作业做完的那一刻 CNTP 到点，经 GIC 的 PPI 30 送来中断，和真设备的完成中断一样是异步的。
 * 设备状态寄存器（忙/闲）由模型按时间给出：内核下令中止后轮询它，量出从下令到停住的时间。
 *
 *   闸门：放行前检查声明的最长作业 + 切换开销放得进调用者剩下的设备预算，
 *         并且加上声明的停住上限能在下一个 Tier 1 保护区之前结束；不满足就拒绝并给出原因码。
 *   超时：开始后过了声明的最长作业时间还没做完，内核强制中止、量停住时间、作业丢弃并计数，
 *         设备降到 Tier 3，写一行日志（设备名、实测、声明、周期号）。
 *   中断：设备线在 Tier 1 保护区和槽内屏蔽；窗口里每周期最多处理声明的次数，超出就屏蔽到下个周期。
 *         处理时间记在设备当前作业的调用者头上，没有作业时记在驱动自己的预算上，不算系统预留。
 *
 * 日志在内核里只写进缓冲（klog），由 shell 任务打印，避免在关中断的内核路径里等串口。
 */
#include "quotaos.h"

#define DEV_CHUNK_US 5      /* 模型每做 5 us 检查一次中止命令（与 kernel/decl.c 一致） */
#define DEV_RESET_US 2
#define GATE_SLACK_T 500    /* 期限在调度里被处理前可能晚的 tick（出口余量） */

#define ERR_BASE (1ull << 62)

static int64_t poff;        /* CNTPCT - CNTVCT */

static struct {
    struct sim_dev *d;
    struct task *drv, *sc, *srv;
    int busy;
    uint64_t submit_t, start_t, done_t, deadline_t, guard_t;
    int storm;
    uint64_t storm_iv, storm_t0, storm_end;
    uint64_t storm_handled;
    uint32_t irq_period;
    int lim_masked, slot_masked, line_on;
    uint32_t tlogs;
} rt;

static struct dev_stats st;

/* ---------- 内核日志缓冲 ---------- */

#define KLOG_LINES 24
#define KLOG_LEN 160
static char kl[KLOG_LINES][KLOG_LEN];
static uint32_t kl_n, kl_pos, kl_lost, kl_total;

void klog(const char *s)
{
    if (kl_n >= KLOG_LINES)
        return;
    while (*s && kl_pos < KLOG_LEN - 1)
        kl[kl_n][kl_pos++] = *s++;
    kl[kl_n][kl_pos] = 0;
}

void klog_u(uint64_t v)
{
    char t[24];
    int i = 0;
    do {
        t[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    char o[24];
    int n = 0;
    while (i)
        o[n++] = t[--i];
    o[n] = 0;
    klog(o);
}

void klog_end(void)
{
    kl_total++;
    if (kl_n >= KLOG_LINES) {
        kl_lost++;
        return;
    }
    kl_n++;
    kl_pos = 0;
    if (kl_n < KLOG_LINES)
        kl[kl_n][0] = 0;
}

void klog_flush(void)
{
    asm volatile("msr daifset, #2" ::: "memory");
    uint32_t n = kl_n, lost = kl_lost;
    static char copy[KLOG_LINES][KLOG_LEN];
    for (uint32_t i = 0; i < n; i++)
        str_copy(copy[i], kl[i], KLOG_LEN);
    kl_n = 0;
    kl_pos = 0;
    kl_lost = 0;
    kl[0][0] = 0;
    asm volatile("msr daifclr, #2" ::: "memory");
    for (uint32_t i = 0; i < n; i++)
        printf("%s\n", copy[i]);
    if (lost)
        printf("[log] %u more lines not kept\n", lost);
}

uint32_t klog_count(void)
{
    return kl_total;
}

/* ---------- 是不是 icount ---------- */

static int icount;

/* icount 下计数器由指令数推动：同一段 2 条指令的循环跑 8192 圈，三次读到的 tick 完全相同，
 * 并且约等于指令数（1 条指令 = 1 tick）。普通模式下跟着宿主机时钟走，三次不会一样。 */
static uint64_t loop_ticks(void)
{
    uint64_t t0 = ticks_now();
    asm volatile("mov x9, #8192\n1: subs x9, x9, #1\n b.ne 1b" ::: "x9", "cc");
    return ticks_now() - t0;
}

void clock_detect(void)
{
    uint64_t a = loop_ticks(), b = loop_ticks(), c = loop_ticks();
    icount = a == b && b == c && a >= 16384 && a <= 16384 + 64;
    printf("[boot] clock: %s\n", icount ? "icount (counter driven by instruction count)"
                                        : "host time (not icount; timings are reference only)");
}

int clock_is_icount(void)
{
    return icount;
}

const char *clock_tag(void)
{
    return icount ? "" : " [非 icount，仅供参考]";
}

/* ---------- 设备「硬件」 ---------- */

static void cntp_arm(uint64_t vt)
{
    uint64_t p = (uint64_t)((int64_t)vt + poff);
    asm volatile("msr cntp_cval_el0, %0" :: "r"(p));
    asm volatile("msr cntp_ctl_el0, %0; isb" :: "r"(1ul));
}

static void cntp_off(void)
{
    asm volatile("msr cntp_ctl_el0, %0; isb" :: "r"(0ul));
}

static void line_update(void)
{
    int want = (rt.busy || rt.storm) && !rt.lim_masked && !rt.slot_masked;
    if (want != rt.line_on) {
        gic_ppi_enable(DEV_IRQ_ID, want);
        rt.line_on = want;
    }
}

void dev_init(void)
{
    uint64_t p0, v, p1;
    asm volatile("mrs %0, cntpct_el0" : "=r"(p0));
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    asm volatile("mrs %0, cntpct_el0" : "=r"(p1));
    poff = (int64_t)((p0 + p1) / 2) - (int64_t)v;
    cntp_off();
    memset(&rt, 0, sizeof rt);
}

uint64_t dev_real_us(const struct sim_dev *d, uint64_t req)
{
    return d->under_report ? req * 3 / 2 : req;
}

void dev_bind(int devno, struct task *drv)
{
    asm volatile("msr daifset, #2" ::: "memory");
    rt.d = decl_dev(devno);
    rt.drv = drv;
    rt.busy = 0;
    rt.storm = 0;
    rt.irq_period = 0;
    rt.lim_masked = 0;
    rt.tlogs = 0;
    line_update();
    asm volatile("msr daifclr, #2" ::: "memory");
}

/* 内核里调用（演示结束时 finish_bench 也会走到）：作业作废、关掉中断线。 */
static void unbind_locked(void)
{
    if (rt.storm) {
        uint64_t end = ticks_now();
        st.raised += (end - rt.storm_t0) / rt.storm_iv;
    }
    cntp_off();
    rt.busy = 0;
    rt.storm = 0;
    rt.drv = 0;
    rt.sc = 0;
    rt.d = 0;
    line_update();
}

void dev_unbind(void)
{
    asm volatile("msr daifset, #2" ::: "memory");
    if (rt.d)
        unbind_locked();
    asm volatile("msr daifclr, #2" ::: "memory");
}

/* 风暴模式：设备不做作业，每 interval_us 拉一次中断线（模型自己的计数器记着拉了几次）。 */
void dev_storm(uint32_t interval_us)
{
    asm volatile("msr daifset, #2" ::: "memory");
    rt.storm_iv = us_to_ticks(interval_us);
    rt.storm_t0 = ticks_now();
    rt.storm_handled = 0;
    rt.storm = 1;
    cntp_arm(rt.storm_t0 + rt.storm_iv);
    line_update();
    asm volatile("msr daifclr, #2" ::: "memory");
}

void dev_stats_get(struct dev_stats *s, int reset)
{
    asm volatile("msr daifset, #2" ::: "memory");
    *s = st;
    if (rt.storm)
        s->raised += (ticks_now() - rt.storm_t0) / rt.storm_iv;
    if (reset) {
        memset(&st, 0, sizeof st);
        if (rt.storm) {
            rt.storm_t0 = ticks_now();
        }
    }
    asm volatile("msr daifclr, #2" ::: "memory");
}

int dev_busy(void)
{
    return rt.busy;
}

uint64_t dev_deadline(void)
{
    return rt.busy ? rt.deadline_t : ~0ull;
}

void dev_slot(int in_slot)
{
    if (rt.slot_masked != in_slot) {
        rt.slot_masked = in_slot;
        line_update();
    }
}

void dev_period(void)
{
    if (rt.irq_period > st.irq_period_max)
        st.irq_period_max = rt.irq_period;
    rt.irq_period = 0;
    rt.lim_masked = 0;
    line_update();
}

/* ---------- 闸门 ---------- */

/* svc #3（驱动）：x0 = 设备号，x1 = 作业长度（us）。返回 0 表示已放行，驱动等完成中断；
 * 否则是拒绝原因：1 设备预算不够，2 会压到下一个 Tier 1 保护区，3 设备在 Tier 3、调用者不是 Tier 3，
 * 4 设备忙，5 没有这个设备。 */
int dev_submit(struct task *drv, uint64_t devno, uint64_t req_us, uint64_t now)
{
    struct sim_dev *d = rt.d;
    int code = 0;
    struct task *c = drv->sc ? drv->sc : drv;
    if (!d || drv != rt.drv || decl_dev((int)devno) != d)
        code = 5;
    else if (rt.busy || rt.storm)
        code = 4;
    else if (d->state != DEV_OK && c->tier != TIER3)
        code = 3;
    else {
        uint64_t wcet = us_to_ticks(d->max_job_us + d->switch_us);
        uint64_t g = sched_next_guard(now);
        if (c->tier == TIER2 && c->dev_used_t + wcet > c->dev_budget_t)
            code = 1;
        else if (g <= now || now + wcet + us_to_ticks(d->stop_us) + GATE_SLACK_T > g)
            code = 2;
        else {
            rt.busy = 1;
            rt.sc = c;
            rt.srv = drv;
            rt.submit_t = now;
            rt.start_t = now + us_to_ticks(d->switch_us);
            rt.done_t = rt.start_t + us_to_ticks(dev_real_us(d, req_us));
            rt.deadline_t = rt.start_t + us_to_ticks(d->max_job_us);
            rt.guard_t = g;
            drv->dev_wait = 1;
            st.submits++;
            cntp_arm(rt.done_t);
            line_update();
            return 0;
        }
    }
    st.refused[code]++;
    return code;
}

static void finish(uint64_t end, uint64_t result)
{
    uint64_t used = end - rt.submit_t;
    rt.sc->dev_used_t += used;
    st.dev_time_t += used;
    if (end > rt.guard_t)
        st.abort_late++;
    struct task *drv = rt.drv;
    ((struct irq_frame *)drv->sp)->r[0] = result;
    drv->dev_wait = 0;
    drv->state = TASK_READY;
    rt.busy = 0;
    cntp_off();
    line_update();
}

static void log_dev_line(const char *what, uint64_t meas_t, uint64_t decl_t)
{
    klog("[health] ");
    klog(rt.d->name);
    klog(" ");
    klog(what);
    klog(": measured ");
    klog_u(up_us(meas_t));
    klog(" us > declared ");
    klog_u(ticks_to_us(decl_t));
    klog(" us, period ");
    klog_u(sched_period_now());
    klog_end();
}

/* 期限到了还没做完：强制中止，轮询状态寄存器直到停住，量出停住时间。 */
static void timeout(uint64_t now)
{
    struct sim_dev *d = rt.d;
    uint64_t meas = now - rt.start_t;
    uint64_t a = ticks_now();
    uint64_t stop_at;
    if (d->preemptible) {
        uint64_t chunk = us_to_ticks(DEV_CHUNK_US);
        uint64_t el = a - rt.start_t;
        stop_at = a + (chunk - el % chunk) % chunk + us_to_ticks(DEV_RESET_US);
    } else {
        stop_at = rt.done_t;
    }
    cntp_off();
    uint64_t lim = a + 4 * us_to_ticks(d->stop_us) + 64;
    uint64_t t;
    while ((t = ticks_now()) < stop_at && t < lim)
        asm volatile("" ::: "memory");
    uint64_t stop = t - a;
    st.timeouts++;
    st.stop_n++;
    if (stop > st.stop_max_t)
        st.stop_max_t = stop;
    if (stop > us_to_ticks(d->stop_us))
        st.stop_over_bound++;
    if (stop > us_to_ticks(GUARD_US))
        st.stop_over_guard++;
    if (st.timeouts == 1)
        st.first_timeout_period = sched_period_now();
    if (rt.tlogs < 2) {
        rt.tlogs++;
        log_dev_line("timeout", meas, us_to_ticks(d->max_job_us));
        klog("  job aborted; stop took ");
        klog_u(up_us(stop));
        klog(" us (declared bound ");
        klog_u(d->stop_us);
        klog(" us)");
        klog(clock_tag());
        klog_end();
    }
    finish(t, ERR_BASE | 6);
    if (d->state != DEV_OK)
        return;
    int devno = 0;
    while (decl_dev(devno) != d)
        devno++;
    decl_health_fault(devno, "timeout", meas, us_to_ticks(d->max_job_us));
}

struct task *dev_poll(uint64_t now, struct task **srv)
{
    if (!rt.busy || now < rt.deadline_t)
        return 0;
    struct task *c = rt.sc, *s = rt.srv;
    timeout(now);
    *srv = s;
    return c;
}

/* PPI 30：作业做完，或者风暴里的一次。返回这段内核时间记在谁的预算上。 */
struct task *dev_irq(uint64_t entry, struct task **srv)
{
    struct task *who = rt.drv;
    *srv = 0;
    rt.irq_period++;
    st.irq_handled++;
    if (sched_in_t1_window(entry))
        st.irq_in_t1++;
    if (rt.storm) {
        rt.storm_handled++;
        uint64_t k = (entry - rt.storm_t0) / rt.storm_iv + 1;
        cntp_arm(rt.storm_t0 + k * rt.storm_iv);
    } else if (rt.busy && entry >= rt.done_t) {
        who = rt.sc;
        *srv = rt.srv;
        st.completed++;
        uint64_t meas = entry - rt.start_t;
        finish(entry, meas);
        if (meas > us_to_ticks(rt.d->max_job_us) && rt.d->state == DEV_OK) {
            int devno = 0;
            while (decl_dev(devno) != rt.d)
                devno++;
            decl_health_fault(devno, "job", meas, us_to_ticks(rt.d->max_job_us));
        }
    } else {
        cntp_off();
    }
    int over_budget = rt.drv && who == rt.drv && rt.drv->used_t >= rt.drv->min_t;
    if (rt.d && (rt.irq_period >= rt.d->irq_limit || over_budget) && !rt.lim_masked) {
        rt.lim_masked = 1;
        st.lim_masks++;
    }
    line_update();
    if (!who)
        return 0;
    return who;
}

void dev_unbind_locked(void)
{
    if (rt.d)
        unbind_locked();
}

void dev_t1_busy(void)
{
    st.t1_busy++;
}

/* 日志缓冲里有没有一行同时含有全部关键字（在 klog_flush 之前查）。 */
int klog_find_all(const char *const *keys, int n)
{
    for (uint32_t i = 0; i < kl_n; i++) {
        int all = 1;
        for (int k = 0; k < n && all; k++) {
            const char *s = keys[k];
            int hit = 0;
            for (int p = 0; kl[i][p] && !hit; p++) {
                int j = 0;
                while (s[j] && kl[i][p + j] == s[j])
                    j++;
                hit = !s[j];
            }
            all = hit;
        }
        if (all)
            return 1;
    }
    return 0;
}

/* 同一行里关键字出现的次数（每行最多算一次）。 */
int klog_count_lines(const char *key)
{
    int n = 0;
    for (uint32_t i = 0; i < kl_n; i++) {
        for (int p = 0; kl[i][p]; p++) {
            int j = 0;
            while (key[j] && kl[i][p + j] == key[j])
                j++;
            if (!key[j]) {
                n++;
                break;
            }
        }
    }
    return n;
}
