/* Quota OS：基于三级资源声明的微内核，保证一级任务永远不卡顿、二级任务运行不卡顿，三级任务则不保证。 */
#pragma once

/* 版本号：每个提交加 1，三位补零。启动横幅和 README 抬头都显示它。 */
#define QUOTAOS_VERSION     "016"
/* Tier 2 应用用 IPC 时准入多留的余量：调用者预算在 IPC 内核路径里用完时超出的部分从这里出 */
#define IPC_HEADROOM_US     120   /* 大于实测最长的一次内核停留（qa-long 6622 tick = 106 us） */

#include <stdint.h>
#include <stddef.h>

/*
 * 资源模型（每个 10ms 周期）：
 *   系统预留  内核自己的 CPU（实测每周期最坏值 + 余量）和内存，不外借。
 *   Tier 1    每个任务一个写死的时间槽，按槽表定时触发，到槽尾准时掐断；
 *             没用完或没人用的槽空着，不借给任何人。
 *   Tier 2    最低值 + 建议值。准入条件：系统预留 + Tier 1 + ΣTier 2 最低值 <= 100%。
 *             最低值每周期都给足；建议值只来自空闲，有压力时收回。
 *   Tier 3    分剩下的，碰不到上面三样。
 */
#define PERIOD_US           10000u
#define T3_QUANTUM_US       1000u
#define GUARD_US            20u      /* 槽前多少微秒停下窗口任务，内核原地等到槽起点；v010 起 20：IPC 的 svc 可能恰好在保护区前进内核 */
#define SYS_RESERVE_US      350u     /* 系统预留 CPU：实测每周期最坏 15290 tick（245 us）+ 余量，见 README */
#define KERNEL_RESERVE_KB   (16u * 1024u)
#define JUMP_US             20u      /* 定时器或槽起点迟到超过这么多，记一次时钟跳动 */

#define MAX_TASKS           48
#define MAX_SLOTS           8
#define STACK_SIZE          8192u
#define MAX_APPS            40
#define NAME_LEN            24
#define REASON_LEN          512

#define TASK_EMPTY          0
#define TASK_READY          1
#define TASK_RUNNING        2
#define TASK_BLOCKED        3
#define TASK_DEAD           4
#define TASK_IPC            5   /* 发出 IPC 调用、等回复；它的预算借给服务端用 */

#define TIER_IDLE           0
#define TIER1               1
#define TIER2               2
#define TIER3               3

#define KIND_SPIN           0   /* 死循环：有多少给多少 */
#define KIND_JOB            1   /* 周期作业：释放后做 work_us，svc 交还 */
#define KIND_SHELL          2
#define KIND_IDLE           3
#define KIND_SERVER         4   /* 被动服务端（驱动）：自己不被调度，只在调用者的预算上跑 */
#define TIER_SERVER         0

#define ADMIT_OK            0
#define ADMIT_DEVICE        1
#define ADMIT_OVERSUB       2
#define ADMIT_DUP           3

struct irq_frame {
    uint64_t r[31];
    uint64_t elr;
    uint64_t spsr;
    uint64_t pad;
};

/* 一个演示窗口里的统计。时间一律是 CNTVCT 原始 tick（QEMU virt 62.5 MHz，16 ns）。 */
struct task_win {
    uint32_t periods;
    uint32_t jobs;
    uint32_t misses;
    uint32_t short_periods;     /* Tier 2：本周期拿到的少于最低值 */
    uint32_t cuts;              /* 到槽尾或预算用完被掐断的次数 */
    uint64_t used_sum;
    uint64_t used_min;
    uint64_t used_max;
    uint64_t start_min;         /* Tier 1：作业第一条指令读到的时间 - 槽起点 */
    uint64_t start_max;
    uint64_t lat_max;           /* 完成时刻 - 释放时刻 */
};

struct task {
    int id;
    char name[NAME_LEN];
    int tier;
    int kind;
    int state;
    uint64_t sp;
    uint8_t *stack;

    int slot;                   /* Tier 1：槽号，-1 表示没有 */
    uint64_t min_t;             /* Tier 2：每周期最低值（tick） */
    uint64_t sugg_t;            /* Tier 2：建议值里超出最低值的部分 */
    uint64_t work_t;            /* KIND_JOB：每个作业的工作量 */
    uint64_t deadline_t;        /* KIND_JOB：相对释放时刻的截止期 */
    uint64_t used_t;            /* 本周期已用 */

    int job_open;
    int job_started;
    uint64_t release_ts;
    uint64_t start_ts;
    uint64_t job_period;

    int bench_owned;
    int wait_bench;

    uint32_t jobs;              /* 开机以来 */
    uint32_t misses;
    uint64_t used_max_all;      /* 开机以来单周期最多用了多少（Tier 1 的浪费 = 槽长 - 它） */
    struct task_win w;

    /* E2：预算令牌随 IPC 走（seL4 MCS 式）。 */
    struct task *sc;            /* 服务端正在用谁的预算；0 = 自己 */
    struct task *server;        /* 调用者：在等哪个服务端 */
    /* 服务端：排队等它的调用者（任务号，先来先服务）。v012 起代替回复时把全部任务扫一遍。 */
    uint8_t ipc_q[64];
    uint32_t ipc_qh, ipc_qt;
    int ipc_q_over;             /* 队列满过：回复时退回全扫 */
    int ipc_queued;             /* 调用者：在某个服务端的队列里 */
    uint64_t p_own, p_run, p_kin, p_kent, p_krep;   /* 本周期：自己跑 / 驱动替它跑 / 三段内核路径 */
    uint64_t p_hr;              /* 本周期用掉的 IPC 余量（Tier 2） */
    int p_hr_pass;              /* 本周期在哪一轮用了余量：1 保底轮，2 建议轮 */
    int ipc_hold;               /* 本周期用过余量：新调用扣到下个周期 */
    int ipc_held;               /* 调用者：这次调用被扣住，等下个周期 */
    struct task *caller;        /* 服务端：正在替谁干活 */
    uint64_t used_total;        /* 预算主人：累计用量，不按周期清零（v015 起用来量每次调用扣了多少） */
    uint64_t call_base;         /* 服务端：这次调用开始时预算主人的 used_total */
    uint64_t call_sub_ran, call_sub_kern;   /* 服务端：这次调用里转发出去的调用（已回复）的运行和内核路径 */
    int p_hr_n;                 /* 本周期用余量的次数 */
    uint64_t call_ran_t;        /* 服务端：本次调用里内核量到的运行时间 */
    uint64_t call_kern_t;       /* 服务端：本次调用里记在调用者头上的 IPC 内核路径时间 */
    uint64_t run_cap;           /* Tier 2：这次被挑中时的预算上限（最低值或最低值+建议值） */

    /* E3：设备时间预算（每周期清零）。驱动等设备做完时 dev_wait = 1。 */
    uint64_t dev_budget_t;      /* Tier 2 调用者：每周期可用的设备时间；0 = 不限（只给 Tier 3） */
    uint64_t dev_used_t;
    int dev_wait;

    /* E4：内存带宽（MemGuard 式）。访存计数是模拟的：运行 tick * 访存密度。 */
    uint64_t bw_rate;           /* 每 1000 tick 的访存次数（任务代码的访存密度） */
    uint64_t bw_budget;         /* 每周期最多访存次数；0 = 不限 */
    uint64_t bw_used, bw_frac;
    int bw_hit, bw_prev_hit;    /* 本周期 / 上周期用满并被停住 */
    uint64_t bw_w_periods, bw_w_stalled, bw_w_over_max, bw_w_after, bw_w_restore_min;
    uint64_t bw_w_used_max, bw_w_used_min;
    /* 0：内核任务，用内核 TTBR0。非 0：EL0 进程的 TTBR0（含 ASID）。 */
    uint64_t user_ttbr;
};

/* 模拟设备（软件模型，不是真实硬件）。出厂描述写死；状态由系统维护。 */
#define DEV_NEW             0   /* 新设备：没验证过，只能在 Tier 3 */
#define DEV_OK              1   /* 系统实测没超过声明 */
#define DEV_DEMOTED         2   /* 健康监测发现实测超出声明：降到 Tier 3 */
struct sim_dev {
    const char *name;
    uint32_t max_job_us, switch_us, stop_us;    /* 声明：单次作业最长、切换开销、中止到停住上限 */
    int preemptible, divisible;
    uint32_t real_job_us;       /* 模型验证时用的作业长度 */
    uint32_t irq_limit;         /* 声明：每周期最多多少次中断 */
    int measured;
    uint64_t job_max_t, stop_max_t;
    int state;
    int under_report;           /* 少报模式：真实耗时 = 请求 * 3 / 2，声明不变 */
    uint64_t fault_meas_t, fault_decl_t, fault_period;
    const char *fault_what;
};

struct tier1_decl {
    const char *name;
    const char *role;
    uint32_t cpu_us;            /* 槽长；0 = 只占内存 */
    uint32_t slot_off_us;       /* 槽在周期里的起点 */
    uint32_t mem_kb;
};

struct app_manifest {
    const char *name;
    const char *role;
    uint32_t mem_min_kb;
    uint32_t mem_sugg_kb;
    uint32_t cpu_min_us;
    uint32_t cpu_sugg_us;
};

struct running_app {
    char name[32];
    uint32_t mem_min_kb;
    uint32_t mem_sugg_kb;
    uint32_t mem_granted_kb;
    uint32_t cpu_min_us;
    uint32_t cpu_sugg_us;
    struct task *task;
    int alive;
};

static inline void mmio_wr(uint64_t a, uint32_t v)
{
    *(volatile uint32_t *)a = v;
}

static inline uint32_t mmio_rd(uint64_t a)
{
    return *(volatile uint32_t *)a;
}

#ifdef __cplusplus
extern "C" {
#endif

void uart_init(void);
void uart_putc(char c);
void printf(const char *fmt, ...);
void readline(char *buf, int cap);
void put_padded(const char *s, int cols);

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int strcmp(const char *a, const char *b);
void str_copy(char *dst, const char *src, int cap);

uint64_t dtb_ram_bytes(uint64_t hinted);
int dtb_find_compatible(const char *compat, uint64_t *out, int max);
/* 每个匹配节点的第一段 reg 基址。返回个数。 */
int dtb_find_bases(const char *compat, uint64_t *bases, int max);
const char *dtb_bootargs(void);

void gic_init(void);
uint32_t gic_ack(void);
void gic_eoi(uint32_t iar);

void timer_init(void);
uint64_t ticks_now(void);
uint64_t timer_freq(void);
uint64_t us_to_ticks(uint64_t us);
uint64_t ticks_to_us(uint64_t t);
uint64_t now_us(void);
void busy_ticks(uint64_t t);

int dtb_fault_expected(void);
void dtb_note_fault(void);
int mmu_fault_expected(void);
void mmu_note_fault(void);

void resource_init(uint64_t ram_bytes);
const struct tier1_decl *resource_tier1(int *count);
uint32_t resource_t1_cpu_us(void);
uint32_t resource_t1_mem_kb(void);
uint32_t resource_guard_us(void);
uint32_t resource_app_cpu_capacity_us(void);
uint64_t resource_app_mem_capacity_kb(void);
uint32_t resource_cpu_min_sum_us(void);
uint64_t resource_mem_min_sum_kb(void);
uint64_t resource_ram_kb(void);
int resource_pressure(void);
void resource_set_pressure(int on);
int resource_check(const struct app_manifest *m);
const char *resource_reason(void);
struct running_app *resource_commit(const struct app_manifest *m);
int resource_release(struct running_app *a);
int resource_running_count(void);
struct running_app *resource_running(int index);
const struct app_manifest *resource_catalog(int *count);

void sched_init(void);
struct task *sched_current(void);
struct task *task_create(const char *name, int tier, int kind);
void task_start(struct task *t);
void task_ready(struct task *t);
void task_kill(struct task *t);
void sched_bind_slot(struct task *t, int slot);
struct task *sched_slot_owner(int slot);
struct task *sched_find(const char *name);
struct task *sched_task_at(int index);
void sched_start(void (*shell_entry)(void));
void sched_bench(uint32_t periods);
int sched_bench_async(uint32_t periods);
int sched_bench_busy(void);
uint32_t sched_bench_left(void);
void sched_bench_stop(void);
int demo_menu_count(void);
void demo_menu_line(int index, char *buf, int cap);
int demo_menu_start(int id);
void demo_menu_status(char *buf, int cap);
void demo_menu_stop(void);
void demo_menu_legend(char *buf, int cap);
void sched_print_window(void);
void sched_print_kernel(void);
void sched_print_slots(void);
uint64_t sched_kernel_max_all(void);
uint64_t sched_kernel_max_period(void);
/* v015：开机以来内核用量最大那个周期的明细（按上一次进内核的原因归类） */
enum { KC_TIMER, KC_GUARD, KC_DEVIRQ, KC_SVC_IPC, KC_SVC_OTHER, KC_N };
struct kattr {
    uint64_t period, total, t[KC_N], n[KC_N], boundary, spin;
    int label;
};
void sched_set_label(int l);
void sched_kernel_worst(struct kattr *a);
uint64_t sched_irq_lat_max(void);
uint64_t sched_periods(void);
uint64_t sched_foreign_window(void);
uint64_t sched_next_guard(uint64_t now);
void task_set_entry(struct task *t, void (*entry)(void));
/* EL0 初始帧。user_sp 写入帧的 pad，出口时装进 SP_EL0。spsr 为 EL0t、IRQ 开。 */
void task_arm_el0(struct task *t, uint64_t elr, uint64_t user_sp,
                  uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5);
/* 把任务标成 DEAD 并刷新就绪表。不改 DAIF，调用者自己管中断屏蔽。 */
void task_dead(struct task *t);
struct ipc_stats {
    uint64_t calls, drop_sum, meas_sum, meas_max, diff_max, srv_used_change, self_sum;
    uint64_t run_sum, kern_sum;
    uint64_t excess_n, excess_sum, excess_max;   /* IPC 内核路径超出调用者预算、改记系统预留的部分 */
    /* v012：kern_sum 分三段（相加正好是 kern_sum）：调用窗口里（服务端绑着调用者时）、
     * 进入和排队（svc #1 到服务端接手之前）、回复返回（svc #2 之后回到调用者之前） */
    uint64_t kern_in, kern_entry, kern_reply;
    /* 整个窗口的核对（按周期结算，和预算同一口径）：调用者预算减少总量
     * = 各任务周期用量之和 - 任务自己跑的部分；应等于驱动替它跑的 + 三段内核路径 */
    uint64_t w_used, w_own, w_run, w_kin, w_kent, w_krep;
    /* v013：两种口径的边界。累计计数（run_cum、kern_in/entry/reply）从清零那一刻
     * 算到读出那一刻；w_* 只算记录窗口里结束了的周期。窗口起止时各拍一次累计计数的快照。 */
    uint64_t run_cum;                       /* 驱动替调用者跑的，按记账时刻累计 */
    uint64_t t_reset, t_read, t_w0, t_w1, p_w0, p_w1;
    uint64_t snap0[4], snap1[4];            /* run_cum, kern_in, kern_entry, kern_reply */
    uint64_t infl_reset_run, infl_end_run, infl_end_calls;  /* 边界上没回复的调用已跑的部分 */
    /* v013：Tier 2 调用者的 IPC 内核路径超出其预算上限的部分，记在调用者准入时多留的余量上 */
    uint64_t hr_n, hr_sum, hr_max, hr_pp_max;
    /* v014：用过余量的调用者本周期新发的调用先扣住，下个周期再放 */
    uint64_t held_n, held_resumed, hr_both_periods, hr_periods;
    uint64_t hr_multi_periods, fwd_calls, fwd_replies;   /* v015 */
};
void sched_ipc_stats(struct ipc_stats *s, int reset);
void e2_demo(uint32_t periods, int qa_line);
void e2_hold_demo(uint32_t periods, int qa_line);
void sched_set_ipc_hold_rule(int on);
void sched_total_misses(uint64_t *a, uint64_t *b);
void sched_window_misses(uint64_t *a, uint64_t *b);

void decl_init(void);
struct sim_dev *decl_dev(int i);
void decl_verify_devices(void);
int decl_health_fault(int dev, const char *what, uint64_t meas_t, uint64_t decl_t);
void decl_notice(void);
int decl_t1_left_on(int dev);
void decl_reset_device(int dev);
int decl_try_t1(const char *who, int dev, uint64_t b, const char *expect);
void decl_add_t1_app(const char *who, int dev, uint64_t b, int policy);
uint64_t dev_real_us(const struct sim_dev *d, uint64_t req);
uint64_t up_us(uint64_t t);
int clock_is_icount(void);
void clock_detect(void);
const char *clock_tag(void);
void klog(const char *s);
void klog_u(uint64_t v);
void klog_end(void);
void klog_flush(void);
uint32_t klog_count(void);
int klog_find_all(const char *const *keys, int n);
int klog_count_lines(const char *key);

/* E3：设备闸门、超时降级、中断风暴限流（kernel/dev.c） */
#define DEV_IRQ_ID          30
struct dev_stats {
    uint64_t submits, refused[6], completed, timeouts;
    uint64_t stop_n, stop_max_t, stop_over_bound, stop_over_guard, abort_late;
    uint64_t t1_busy, irq_handled, irq_in_t1, irq_period_max, lim_masks, raised;
    uint64_t dev_time_t, irq_charged_t, first_timeout_period;
};
void dev_init(void);
void dev_bind(int devno, struct task *drv);
void dev_unbind(void);
void dev_unbind_locked(void);
void dev_t1_busy(void);
void dev_storm(uint32_t interval_us);
void dev_stats_get(struct dev_stats *s, int reset);
int dev_submit(struct task *drv, uint64_t devno, uint64_t req_us, uint64_t now);
struct task *dev_irq(uint64_t entry, struct task **srv);
struct task *dev_poll(uint64_t now, struct task **srv);
uint64_t dev_deadline(void);
void dev_slot(int in_slot);
int dev_busy(void);
void dev_period(void);
uint64_t sched_period_now(void);
int sched_in_t1_window(uint64_t now);
void gic_ppi_enable(uint32_t id, int on);
void mmu_init(void);
int mmu_protect(uint64_t addr, uint64_t len, int prot);
/* 内核 TTBR0（不含后来切到用户进程的值）和共享的 RAM 二级表物理地址。 */
uint64_t mmu_kernel_ttbr(void);
uint64_t mmu_l2_ram_phys(void);
uint32_t system_reserve_kb(void);
/* webkit boot only. 0 on every other boot, including e4. */
uint64_t webkit_structure_bytes(void);
uint64_t webkit_structure_base(void);
void boot_check_reservations(uint64_t structure, uint64_t window, uint64_t arena, uint64_t arena_end);
/* End of the JSC arena. The weak default is the image end, so a kernel
 * without the engine keeps the old framebuffer floor. */
uint64_t jsc_arena_end(void);
void color_init(uint64_t ram_bytes);
void color_pool_range(uint64_t *start, uint64_t *end);
uint64_t color_padding_kb(void);
uint32_t color_alloc_shared(uint32_t n);
void color_free_shared(void);
void color_report(int qa_line);
void e4_demo_bandwidth(uint32_t periods, int qa_line);
void e4_demo_color(int qa_line);
int decl_try_bw(const char *who, int tier, uint64_t accesses);
int decl_try_ipc_t2(const char *who, uint64_t min_us, uint64_t prior_us);
uint64_t decl_bw_capacity(void);
void e3_demo_timeout(uint32_t periods, int qa_line);
void e3_demo_storm(uint32_t periods, int qa_line);
void decl_demo_device_tier1(void);
void decl_demo_overcommit(void);
void decl_qa_e1(int runs_hint);

uint64_t handle_irq(uint64_t frame_sp);
uint64_t handle_sync(uint64_t frame_sp);
void handle_unhandled(uint64_t frame_sp, uint64_t kind);

void kmain(uint64_t dtb);
void shell_main(void);
void webkit_autostart(void);
int kernel_reserve_alloc(uint64_t bytes, uint64_t align, uint64_t *pa);
int wk_reserve(uint64_t bytes, int executable, unsigned guard_pages, uint64_t *pa);
void *wk_os_reserve(uint64_t bytes, uint64_t align, int executable, unsigned guard_pages);
void wk_os_release(void *p, uint64_t bytes);
int wk_os_commit(void *p, uint64_t bytes, int executable);
void wk_os_decommit(void *p, uint64_t bytes);
int wk_os_protect(void *p, uint64_t bytes, int readable, int writable);
void wk_os_zero(void *p, uint64_t bytes);
int cxx_runtime_test(void);
int fb_device_self_test(uint32_t **pixels, uint32_t *w, uint32_t *h, uint32_t *stride);
/* 0 when the surface was reserved. Bytes are inside the 16 MB system reserve. */
int fb_reserve(void);
int fb_reserved_range(uint64_t *start, uint64_t *bytes);
/* Publish the reserved surface through ramfb. Does not draw a demo. */
int fb_present(void);
void fb_geometry(uint32_t *w, uint32_t *h);

/* Fixed budgets for the LVGL demo. The heap is LVGL's pool (no expand).
 * The draw buffer and framebuffer are separate and do not grow. */
#define LVGL_HEAP_BYTES (256u * 1024u)
#define LVGL_DRAW_BYTES (640u * 48u * 4u)
#define LVGL_FB_BYTES (640u * 480u * 4u)

#define QUOTA_INPUT_KEY 1u
#define QUOTA_INPUT_POINTER 2u
struct quota_input {
    uint8_t kind;
    uint16_t code;
    uint16_t value;
    int16_t x, y;
};
int input_init(void);
int input_poll(uint16_t *code, uint16_t *value);
void input_set_span(uint32_t w, uint32_t h);
int input_service_poll(void);
int input_pop(struct quota_input *ev);
uint32_t input_drop_count(void);
void lvgl_autostart(void);
int lvgl_demo_run(uint32_t *pixels, uint32_t w, uint32_t h, uint32_t stride);
void services_start(void);
void job_body(uint64_t start);

void start_first_task(uint64_t frame_sp);
void job_resume(void);

/*
 * EL0 进程边界。应用只通过 proc_spawn 和三条 SVC（让出、退出、提交）进来。
 * 应用不能调用 LVGL、驱动或 resource_commit，也不能映射内核、设备、
 * 全局 framebuffer 或其他应用的页。显示服务从已准入的绘图页拷进 framebuffer。
 * LVGL 仍是内核侧适配，不是 EL0 应用。实时性不在这里验收。
 */
void proc_init(void);
void proc_on_switch(struct task *next);
void proc_el0_svc(struct irq_frame *f);
void proc_el0_abort(struct irq_frame *f, uint64_t esr);

struct el0_reap {
    int valid;
    int pid;
    int cancelled;
    int hold_before;
    int hold_after;
    int free_before;
    int free_after;
    int unmap_refused;
    uint32_t draw0;
    uint32_t submits;
    uint32_t rej_range;
    uint32_t rej_freq;
    uint32_t rej_depth;
    uint32_t gen_before;
    uint32_t gen_after;
    uint64_t us;
    uint64_t ticks;
    uint64_t cntpct;
    uint64_t periods;
    uint16_t vx, vy, vw, vh;
    char why[16];
};

struct el0_fault {
    int valid;
    int pid;
    uint32_t ec;
    uint64_t esr, far, elr, spsr, pte;
    int level;
    int ap_el0;
    char kind[16];
};

/*
 * 句柄 = generation << 16 | (slot+1)。generation 在内核接到终止或异常时立刻加一，
 * 槽位重新分配后旧句柄仍然对不上。0 和负数都不是活句柄。
 * 回收目标是 2 个调度周期（2 * PERIOD_US）。这是目标，不是事先验证过的性能。
 */
#define EL0_REAP_TARGET_US (2u * PERIOD_US)
#define EL0_KILL_ROUNDS 100
#define EL0_FAULT_ROUNDS 100

int64_t proc_spawn(const char *name, uint64_t role, uint64_t limit, uint64_t foreign_pa,
                   uint64_t fb_pa, uint64_t kern_va, uint16_t vx, uint16_t vy,
                   uint16_t vw, uint16_t vh);
int proc_kill(int64_t handle);
void proc_ledger(const char *tag);
int proc_pages_free(void);
int proc_pages_used(void);
int proc_queue_depth(void);
int proc_holds(void);
int proc_alive(int64_t handle);
int proc_queued_gen(uint32_t gen);
int proc_queued_slot(int slot, uint32_t gen);
uint32_t proc_submits(int64_t handle);
uint32_t proc_rej_range(int64_t handle);
uint32_t proc_rej_freq(int64_t handle);
uint32_t proc_rej_depth(int64_t handle);
int proc_queue_for(int64_t handle);
uint64_t proc_draw_pa(int64_t handle);
uint64_t proc_stack_pa(int64_t handle);
int proc_pid(int64_t handle);
/* 用句柄走和 SVC 相同的提交检查。旧代际返回 -1，不会作用到新占用者。 */
int proc_submit_handle(int64_t handle);
int disp_drain(int max);
void proc_last_reap(struct el0_reap *o);
void proc_last_fault(struct el0_fault *o);
int proc_unmap_refusals(void);
uint64_t proc_irq_off_max_us(void);
void proc_finish_pending(void);
void isolate_run(void);

#ifdef __cplusplus
}
#endif
