/*
 * EL0 processes. Each live app has its own TTBR0. EL0 can touch only its
 * code, stack, and draw page. Kernel RAM, devices, the global framebuffer,
 * and other apps stay EL1-only or absent.
 *
 * A handle is generation<<16 | (slot+1). The generation bumps when the
 * kernel accepts a kill or an exception, before any page is freed. Reusing
 * the slot does not revive the old handle.
 *
 * Exit order: revoke submit and bump generation, cancel in-flight draws
 * that still name the old generation, and only then unmap and free.
 * A page still referenced by the display queue is not freed. Missing the
 * 2-period target does not skip that rule.
 *
 * LVGL is not involved. Display clients enter through SVC submit.
 */
#include "quotaos.h"

#define EL0_SLOTS 2
#define EL0_POOL 16
#define EL0_Q_PER 3
#define EL0_Q_GLOBAL 8
#define EL0_FREQ 2
#define EL0_DRAW 4096u
#define DRAW_VA 0x2000ull
#define CODE_VA 0x0ull
#define STACK_VA 0x1000ull
#define USER_SP 0x2000ull

#define PTE_VALID 1ull
#define PTE_PAGE 2ull
#define PTE_AF (1ull << 10)
#define PTE_NG (1ull << 11)
#define PTE_PXN (1ull << 53)
#define PTE_UXN (1ull << 54)
#define ATTR_NORMAL 0ull
#define ATTR_DEVICE (1ull << 2)
#define AP_EL0 (1ull << 6)
#define AP_RO (1ull << 7)
#define SH_INNER (3ull << 8)

extern char user_blob[];
extern char user_blob_end[];

struct proc {
    int slot;
    int alive;
    int draining;
    int submit_ok;
    uint32_t gen;
    int asid;
    int hold;
    int pages[3];
    uint64_t pa[3];
    uint64_t ttbr;
    uint16_t vx, vy, vw, vh;
    struct task *task;
    struct running_app *app;
    uint32_t submits, rej_range, rej_freq, rej_depth;
    uint64_t sub_period;
    uint32_t sub_n;
    int pid;
    uint64_t reap_t0;
    uint64_t reap_pct0;
};

struct qent {
    int used;
    int slot;
    uint32_t gen;
    uint64_t pa;
    uint16_t x, y, w, h;
    uint32_t bytes;
};

static struct proc procs[EL0_SLOTS];
static struct qent qq[EL0_Q_GLOBAL];
static uint64_t pt_l0[EL0_SLOTS][512] __attribute__((aligned(4096)));
static uint64_t pt_l1[EL0_SLOTS][512] __attribute__((aligned(4096)));
static uint64_t pt_l2[EL0_SLOTS][512] __attribute__((aligned(4096)));
static uint64_t pt_l3[EL0_SLOTS][512] __attribute__((aligned(4096)));
static uint8_t pool[EL0_POOL][4096] __attribute__((aligned(4096)));
static uint8_t pool_used[EL0_POOL];
static uint64_t kernel_ttbr;
static struct el0_reap last_reap;
static struct el0_fault last_fault;
static int unmap_refusals;
static int spawn_logs;
static int stale_drain;
static uint64_t irq_off_max;
extern uint64_t gate_kernel_ttbr, gate_user_ttbr, gate_saved_x0;
extern char gate_vectors[];

static uint64_t el0_l2_hi[512] __attribute__((aligned(4096)));
static uint64_t el0_l3_gate[512] __attribute__((aligned(4096)));

static uint64_t cntpct_now(void)
{
    uint64_t v;
    asm volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static void irq_off_begin(uint64_t *t0)
{
    *t0 = ticks_now();
    asm volatile("msr daifset, #2" ::: "memory");
}

static void irq_off_end(uint64_t t0)
{
    uint64_t d = ticks_now() - t0;
    if (d > irq_off_max)
        irq_off_max = d;
    asm volatile("msr daifclr, #2" ::: "memory");
}

asm(
    ".section .rodata\n"
    ".global user_blob\n"
    ".global user_blob_end\n"
    "user_blob:\n"
    ".incbin \"build/user/paint.bin\"\n"
    "user_blob_end:\n"
    ".section .text\n"
);

static int pages_free_n(void)
{
    int n = 0;
    for (int i = 0; i < EL0_POOL; i++)
        n += pool_used[i] ? 0 : 1;
    return n;
}

static int pages_used_n(void)
{
    return EL0_POOL - pages_free_n();
}

int proc_pages_free(void)
{
    return pages_free_n();
}

int proc_pages_used(void)
{
    return pages_used_n();
}

int proc_queue_depth(void)
{
    int n = 0;
    for (int i = 0; i < EL0_Q_GLOBAL; i++)
        n += qq[i].used ? 1 : 0;
    return n;
}

int proc_holds(void)
{
    int n = 0;
    for (int i = 0; i < EL0_SLOTS; i++)
        if (procs[i].alive)
            n += procs[i].hold;
    return n;
}

int proc_unmap_refusals(void)
{
    return unmap_refusals;
}

int proc_queued_gen(uint32_t gen)
{
    int n = 0;
    for (int i = 0; i < EL0_Q_GLOBAL; i++)
        if (qq[i].used && qq[i].gen == gen)
            n++;
    return n;
}

int proc_queued_slot(int slot, uint32_t gen)
{
    int n = 0;
    if (slot < 0 || slot >= EL0_SLOTS)
        return -1;
    for (int i = 0; i < EL0_Q_GLOBAL; i++)
        if (qq[i].used && qq[i].slot == slot && qq[i].gen == gen)
            n++;
    return n;
}

static int queue_for_gen(int slot, uint32_t gen)
{
    int n = 0;
    for (int i = 0; i < EL0_Q_GLOBAL; i++)
        if (qq[i].used && qq[i].slot == slot && qq[i].gen == gen)
            n++;
    return n;
}

static struct proc *from_handle(int64_t handle, int live)
{
    if (handle <= 0)
        return 0;
    int slot = (int)(handle & 0xff) - 1;
    uint32_t gen = (uint32_t)((uint64_t)handle >> 16);
    if (slot < 0 || slot >= EL0_SLOTS)
        return 0;
    struct proc *p = &procs[slot];
    if (p->gen != gen)
        return 0;
    if (live && !p->alive)
        return 0;
    return p;
}

static int64_t make_handle(const struct proc *p)
{
    return ((int64_t)p->gen << 16) | (p->slot + 1);
}

static struct proc *proc_by_task(struct task *t)
{
    if (!t)
        return 0;
    for (int i = 0; i < EL0_SLOTS; i++)
        if (procs[i].alive && procs[i].task == t)
            return &procs[i];
    return 0;
}

int proc_alive(int64_t handle)
{
    return from_handle(handle, 1) != 0;
}

static struct proc *live(int64_t handle)
{
    return from_handle(handle, 1);
}

uint32_t proc_submits(int64_t handle)
{
    struct proc *p = live(handle);
    return p ? p->submits : 0;
}

uint32_t proc_rej_range(int64_t handle)
{
    struct proc *p = live(handle);
    return p ? p->rej_range : 0;
}

uint32_t proc_rej_freq(int64_t handle)
{
    struct proc *p = live(handle);
    return p ? p->rej_freq : 0;
}

uint32_t proc_rej_depth(int64_t handle)
{
    struct proc *p = live(handle);
    return p ? p->rej_depth : 0;
}

int proc_queue_for(int64_t handle)
{
    struct proc *p = live(handle);
    if (!p)
        return -1;
    return queue_for_gen(p->slot, p->gen);
}

uint64_t proc_draw_pa(int64_t handle)
{
    struct proc *p = live(handle);
    return p ? p->pa[2] : 0;
}

uint64_t proc_stack_pa(int64_t handle)
{
    struct proc *p = live(handle);
    return p ? p->pa[1] : 0;
}

int proc_pid(int64_t handle)
{
    struct proc *p = live(handle);
    return p ? p->pid : -1;
}

void proc_last_reap(struct el0_reap *o)
{
    if (o)
        *o = last_reap;
}

void proc_last_fault(struct el0_fault *o)
{
    if (o)
        *o = last_fault;
}

static void tlb_asid(int asid)
{
    uint64_t x = (uint64_t)(uint16_t)asid << 48;
    asm volatile("dsb ish; tlbi aside1, %0; dsb ish; isb" :: "r"(x) : "memory");
}

static uint64_t walk(const struct proc *p, uint64_t va, int *level)
{
    uint64_t d0 = pt_l0[p->slot][(va >> 39) & 511];
    if ((d0 & 1) == 0)
        return 0;
    uint64_t *l1 = (uint64_t *)(uintptr_t)(d0 & ~0xfffull);
    uint64_t d1 = l1[(va >> 30) & 511];
    if ((d1 & 1) == 0)
        return 0;
    if ((d1 & 2) == 0) {
        *level = 1;
        return d1;
    }
    uint64_t *l2 = (uint64_t *)(uintptr_t)(d1 & ~0xfffull);
    uint64_t d2 = l2[(va >> 21) & 511];
    if ((d2 & 1) == 0)
        return 0;
    if ((d2 & 2) == 0) {
        *level = 2;
        return d2;
    }
    uint64_t *l3 = (uint64_t *)(uintptr_t)(d2 & ~0xfffull);
    *level = 3;
    return l3[(va >> 12) & 511];
}

static int range_ok(const struct proc *p, uint64_t va, uint64_t bytes,
                    uint64_t x, uint64_t y, uint64_t w, uint64_t h)
{
    uint64_t x1, y1;
    if (w == 0 || h == 0 || w > 32 || h > 32)
        return 0;
    if (bytes != w * h * 4 || bytes > EL0_DRAW)
        return 0;
    if (va != DRAW_VA)
        return 0;
    if (x < p->vx || y < p->vy)
        return 0;
    x1 = (uint64_t)p->vx + p->vw;
    y1 = (uint64_t)p->vy + p->vh;
    if (w > x1 - x || h > y1 - y)
        return 0;
    return 1;
}

static int submit_checked(struct proc *p, uint64_t va, uint64_t bytes,
                          uint64_t x, uint64_t y, uint64_t w, uint64_t h)
{
    int slot_q, all_q, free_q;
    uint64_t period;

    if (!p->alive || !p->submit_ok)
        return 4;
    if (!range_ok(p, va, bytes, x, y, w, h)) {
        p->rej_range++;
        return 1;
    }
    slot_q = queue_for_gen(p->slot, p->gen);
    all_q = proc_queue_depth();
    if (slot_q >= EL0_Q_PER || all_q >= EL0_Q_GLOBAL) {
        p->rej_depth++;
        return 3;
    }
    period = sched_period_now();
    if (period != p->sub_period) {
        p->sub_period = period;
        p->sub_n = 0;
    }
    if (p->sub_n >= EL0_FREQ) {
        p->rej_freq++;
        return 2;
    }
    free_q = -1;
    for (int i = 0; i < EL0_Q_GLOBAL; i++) {
        if (!qq[i].used) {
            free_q = i;
            break;
        }
    }
    if (free_q < 0) {
        p->rej_depth++;
        return 3;
    }
    qq[free_q].used = 1;
    qq[free_q].slot = p->slot;
    qq[free_q].gen = p->gen;
    qq[free_q].pa = p->pa[2];
    qq[free_q].x = (uint16_t)x;
    qq[free_q].y = (uint16_t)y;
    qq[free_q].w = (uint16_t)w;
    qq[free_q].h = (uint16_t)h;
    qq[free_q].bytes = (uint32_t)bytes;
    p->sub_n++;
    p->submits++;
    p->hold++;
    (void)va;
    return 0;
}

int proc_submit_handle(int64_t handle)
{
    struct proc *p;
    uint64_t daif;
    int rc;
    asm volatile("mrs %0, daif" : "=r"(daif));
    asm volatile("msr daifset, #2" ::: "memory");
    p = live(handle);
    rc = p ? submit_checked(p, DRAW_VA, EL0_DRAW, p->vx, p->vy, p->vw, p->vh) : -1;
    if ((daif & 0x80ull) == 0)
        asm volatile("msr daifclr, #2" ::: "memory");
    return rc;
}

static void blit(uint64_t pa, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint64_t fb, bytes;
    uint32_t fw = 0, fh = 0;
    uint32_t *dst, *src;

    if (fb_reserved_range(&fb, &bytes) != 0)
        return;
    fb_geometry(&fw, &fh);
    if (fw == 0 || (uint32_t)x + w > fw || (uint32_t)y + h > fh)
        return;
    dst = (uint32_t *)(uintptr_t)fb;
    src = (uint32_t *)(uintptr_t)pa;
    for (uint16_t row = 0; row < h; row++)
        for (uint16_t col = 0; col < w; col++)
            dst[((uint32_t)y + row) * fw + x + col] = src[(uint32_t)row * w + col];
}

int disp_drain(int max)
{
    int n = 0;
    uint64_t daif;
    if (max < 0)
        return 0;
    asm volatile("mrs %0, daif" : "=r"(daif));
    asm volatile("msr daifset, #2" ::: "memory");
    for (int i = 0; i < EL0_Q_GLOBAL && n < max; i++) {
        struct proc *p;
        if (!qq[i].used)
            continue;
        p = &procs[qq[i].slot];
        if (!p->alive || p->gen != qq[i].gen || qq[i].pa != p->pa[2]) {
            /* Drop the slot. Do not blit, and do not touch a newer generation's hold. */
            qq[i].used = 0;
            stale_drain++;
            continue;
        }
        blit(qq[i].pa, qq[i].x, qq[i].y, qq[i].w, qq[i].h);
        qq[i].used = 0;
        if (p->hold)
            p->hold--;
        n++;
    }
    if ((daif & 0x80ull) == 0)
        asm volatile("msr daifclr, #2" ::: "memory");
    return n;
}

static int slot_entries(int slot)
{
    int n = 0;
    for (int i = 0; i < EL0_Q_GLOBAL; i++)
        if (qq[i].used && qq[i].slot == slot)
            n++;
    return n;
}

static int cancel_gen(struct proc *p, uint32_t gen)
{
    int n = 0;
    for (int i = 0; i < EL0_Q_GLOBAL; i++) {
        if (!qq[i].used || qq[i].slot != p->slot || qq[i].gen != gen)
            continue;
        qq[i].used = 0;
        if (p->hold)
            p->hold--;
        n++;
    }
    /* A hold with no queue entry is not a reference. An IRQ could split
     * drain's clear from its hold update. Do not keep the page for that. */
    if (slot_entries(p->slot) == 0)
        p->hold = 0;
    return n;
}

static void mark_dead(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint64_t fb, bytes;
    uint32_t fw = 0, fh = 0;
    uint32_t *dst;
    if (fb_reserved_range(&fb, &bytes) != 0 || w == 0 || h == 0)
        return;
    fb_geometry(&fw, &fh);
    if ((uint32_t)x + w > fw || (uint32_t)y + h > fh)
        return;
    dst = (uint32_t *)(uintptr_t)fb;
    for (uint16_t row = 0; row < h; row++)
        for (uint16_t col = 0; col < w; col++)
            dst[((uint32_t)y + row) * fw + x + col] = 0x00600000u;
}

static int pa_still_queued(uint64_t pa)
{
    if (!pa)
        return 0;
    for (int i = 0; i < EL0_Q_GLOBAL; i++) {
        if (!qq[i].used)
            continue;
        if (qq[i].pa >= pa && qq[i].pa < pa + 4096)
            return 1;
    }
    return 0;
}

static void reap_timed(struct proc *p, const char *why, uint64_t t0, uint64_t p0)
{
    uint64_t off0, pct0;
    uint32_t gen_before;

    if (!p || !p->alive)
        return;
    pct0 = cntpct_now();
    irq_off_begin(&off0);

    gen_before = p->gen;
    p->submit_ok = 0;
    p->gen++;

    memset(&last_reap, 0, sizeof last_reap);
    last_reap.valid = 1;
    last_reap.pid = p->pid;
    last_reap.hold_before = p->hold;
    last_reap.free_before = pages_free_n();
    last_reap.submits = p->submits;
    last_reap.rej_range = p->rej_range;
    last_reap.rej_freq = p->rej_freq;
    last_reap.rej_depth = p->rej_depth;
    last_reap.gen_before = gen_before;
    last_reap.gen_after = p->gen;
    last_reap.vx = p->vx;
    last_reap.vy = p->vy;
    last_reap.vw = p->vw;
    last_reap.vh = p->vh;
    if (p->pa[2])
        last_reap.draw0 = *(uint32_t *)(uintptr_t)p->pa[2];
    str_copy(last_reap.why, why, (int)sizeof last_reap.why);

    last_reap.cancelled = cancel_gen(p, gen_before);
    if (p->task) {
        p->task->user_ttbr = 0;
        task_dead(p->task);
        p->task = 0;
    }
    p->ttbr = 0;
    p->reap_t0 = t0;
    p->reap_pct0 = pct0;
    p->draining = 1;
    irq_off_end(off0);
    (void)p0;
}

static void finish_one(struct proc *p)
{
    int refused = 0;
    uint64_t t1, pct1;
    if (!p->draining)
        return;
    if (p->hold != 0 || slot_entries(p->slot) != 0) {
        refused = 1;
        unmap_refusals++;
    } else {
        uint64_t pas[3];
        for (int i = 0; i < 3; i++)
            pas[i] = p->pa[i];
        for (int i = 0; i < 3; i++)
            pt_l3[p->slot][i] = 0;
        asm volatile("dsb ish" ::: "memory");
        tlb_asid(p->asid);
        for (int i = 0; i < 3; i++) {
            int pg = p->pages[i];
            if (pg < 0 || pg >= EL0_POOL)
                continue;
            if (pa_still_queued(pas[i]) || slot_entries(p->slot) != 0) {
                refused = 1;
                unmap_refusals++;
                break;
            }
            memset(pool[pg], 0, 4096);
            pool_used[pg] = 0;
            p->pages[i] = -1;
            p->pa[i] = 0;
        }
        if (!refused) {
            if (p->app) {
                resource_release(p->app);
                p->app = 0;
            }
            p->alive = 0;
            p->draining = 0;
        }
    }
    t1 = ticks_now();
    pct1 = cntpct_now();
    if (last_reap.pid == p->pid && last_reap.gen_after == p->gen) {
        last_reap.hold_after = p->hold;
        last_reap.free_after = pages_free_n();
        last_reap.unmap_refused = refused;
        last_reap.ticks = t1 >= p->reap_t0 ? t1 - p->reap_t0 : 0;
        last_reap.us = ticks_to_us(last_reap.ticks);
        last_reap.cntpct = pct1 >= p->reap_pct0 ? pct1 - p->reap_pct0 : 0;
    }
    if (!refused)
        mark_dead(p->vx, p->vy, p->vw, p->vh);
}

void proc_finish_pending(void)
{
    for (int i = 0; i < EL0_SLOTS; i++)
        if (procs[i].draining)
            finish_one(&procs[i]);
}

static void classify(struct proc *self, uint64_t far, uint64_t pte, char *out)
{
    uint64_t fb0 = 0, fbn = 0;

    if (far >= 0x08000000ull && far < 0x0c000000ull) {
        str_copy(out, "device", 16);
        return;
    }
    if (fb_reserved_range(&fb0, &fbn) == 0 && far >= fb0 && far < fb0 + fbn) {
        str_copy(out, "framebuffer", 16);
        return;
    }
    for (int i = 0; i < EL0_SLOTS; i++) {
        struct proc *o = &procs[i];
        if (o == self || !o->alive)
            continue;
        for (int k = 0; k < 3; k++) {
            if (o->pa[k] && far >= o->pa[k] && far < o->pa[k] + 4096) {
                str_copy(out, "other-app", 16);
                return;
            }
        }
    }
    if (far < 0x4000ull && (pte & 1ull) == 0) {
        str_copy(out, "unmapped", 16);
        return;
    }
    if (far >= 0x40000000ull && far < 0x48000000ull) {
        str_copy(out, "kernel", 16);
        return;
    }
    str_copy(out, "other", 16);
}

void proc_el0_abort(struct irq_frame *f, uint64_t esr)
{
    uint64_t t0 = ticks_now();
    uint64_t p0 = sched_period_now();
    uint64_t far = 0;
    struct proc *p = proc_by_task(sched_current());
    int level = 0;
    uint64_t pte = 0;

    asm volatile("mrs %0, far_el1" : "=r"(far));
    memset(&last_fault, 0, sizeof last_fault);
    last_fault.valid = 1;
    last_fault.ec = (uint32_t)((esr >> 26) & 0x3f);
    last_fault.esr = esr;
    last_fault.far = far;
    last_fault.elr = f->elr;
    last_fault.spsr = f->spsr;
    if (p) {
        pte = walk(p, far, &level);
        last_fault.pid = p->pid;
        last_fault.pte = pte;
        last_fault.level = level;
        last_fault.ap_el0 = (int)((pte >> 6) & 1ull);
        classify(p, far, pte, last_fault.kind);
        reap_timed(p, "fault", t0, p0);
    } else {
        str_copy(last_fault.kind, "no-proc", 16);
        struct task *t = sched_current();
        if (t)
            task_dead(t);
    }
    printf("[fault] from_el=%u pid=%d ec=0x%x esr=0x%lx elr=0x%lx far=0x%lx spsr=0x%lx pte=0x%lx level=%d ap_el0=%d class=%s gen %u->%u\n",
           (unsigned)((last_fault.spsr & 0xfu) == 0 ? 0 : (last_fault.spsr & 0xfu)),
           last_fault.pid, last_fault.ec, last_fault.esr, last_fault.elr, last_fault.far,
           last_fault.spsr, last_fault.pte, last_fault.level, last_fault.ap_el0, last_fault.kind,
           p ? last_reap.gen_before : 0, p ? last_reap.gen_after : 0);
    if (p)
        printf("[fault] reap us=%lu ticks=%lu periods=%lu hold_before=%d cancelled=%d free %d -> %d refused=%d target_us=%u\n",
               last_reap.us, last_reap.ticks, last_reap.periods, last_reap.hold_before, last_reap.cancelled,
               last_reap.free_before, last_reap.free_after, last_reap.unmap_refused, EL0_REAP_TARGET_US);
}

void proc_el0_svc(struct irq_frame *f)
{
    struct proc *p = proc_by_task(sched_current());
    uint64_t op;

    if (!p) {
        f->r[0] = ~0ull;
        return;
    }
    op = f->r[8];
    if (op == 0) {
        f->r[0] = 0;
        return;
    }
    if (op == 1) {
        reap_timed(p, "exit", ticks_now(), sched_period_now());
        return;
    }
    if (op == 2) {
        f->r[0] = (uint64_t)submit_checked(p, f->r[0], f->r[1], f->r[2], f->r[3], f->r[4], f->r[5]);
        return;
    }
    f->r[0] = ~0ull;
}

int proc_kill(int64_t handle)
{
    struct proc *p = live(handle);
    if (!p)
        return -1;
    reap_timed(p, "kill", ticks_now(), sched_period_now());
    proc_finish_pending();
    return 0;
}

static int alloc_page(uint64_t *pa)
{
    for (int i = 0; i < EL0_POOL; i++) {
        if (pool_used[i])
            continue;
        pool_used[i] = 1;
        memset(pool[i], 0, 4096);
        *pa = (uint64_t)(uintptr_t)pool[i];
        return i;
    }
    return -1;
}

static void free_idx(int idx)
{
    if (idx < 0 || idx >= EL0_POOL)
        return;
    memset(pool[idx], 0, 4096);
    pool_used[idx] = 0;
}

static uint64_t data_pte(uint64_t pa)
{
    return (pa & ~0xfffull) | PTE_VALID | PTE_PAGE | PTE_AF | SH_INNER | ATTR_NORMAL | PTE_NG |
           AP_EL0 | PTE_UXN | PTE_PXN;
}

static uint64_t code_pte(uint64_t pa)
{
    return (pa & ~0xfffull) | PTE_VALID | PTE_PAGE | PTE_AF | SH_INNER | ATTR_NORMAL | PTE_NG |
           AP_EL0 | AP_RO;
}

static void build_pt(struct proc *p)
{
    memset(pt_l0[p->slot], 0, 4096);
    memset(pt_l1[p->slot], 0, 4096);
    memset(pt_l2[p->slot], 0, 4096);
    memset(pt_l3[p->slot], 0, 4096);
    pt_l3[p->slot][0] = code_pte(p->pa[0]);
    pt_l3[p->slot][1] = data_pte(p->pa[1]);
    pt_l3[p->slot][2] = data_pte(p->pa[2]);
    pt_l2[p->slot][0] = (uint64_t)(uintptr_t)pt_l3[p->slot] | PTE_VALID | PTE_PAGE;
    pt_l1[p->slot][0] = (uint64_t)(uintptr_t)pt_l2[p->slot] | PTE_VALID | PTE_PAGE;
    /* RAM half is not the kernel L2. Only the shared gate page is present. */
    pt_l1[p->slot][1] = (uint64_t)(uintptr_t)el0_l2_hi | PTE_VALID | PTE_PAGE;
    pt_l0[p->slot][0] = (uint64_t)(uintptr_t)pt_l1[p->slot] | PTE_VALID | PTE_PAGE;
    p->ttbr = (uint64_t)(uintptr_t)pt_l0[p->slot] | ((uint64_t)p->asid << 48);
    for (int i = 0; i < 512; i += 8) {
        asm volatile("dc civac, %0" :: "r"(&pt_l0[p->slot][i]) : "memory");
        asm volatile("dc civac, %0" :: "r"(&pt_l1[p->slot][i]) : "memory");
        asm volatile("dc civac, %0" :: "r"(&pt_l2[p->slot][i]) : "memory");
        asm volatile("dc civac, %0" :: "r"(&pt_l3[p->slot][i]) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
}

static void clean_code(void *p, uint64_t n)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)63;
    uintptr_t end = (uintptr_t)p + n;
    for (; a < end; a += 64)
        asm volatile("dc cvau, %0" :: "r"(a) : "memory");
    asm volatile("dsb ish; ic iallu; dsb ish; isb" ::: "memory");
}

int64_t proc_spawn(const char *name, uint64_t role, uint64_t limit, uint64_t foreign_pa,
                   uint64_t fb_pa, uint64_t kern_va, uint16_t vx, uint16_t vy,
                   uint16_t vw, uint16_t vh)
{
    struct proc *p = 0;
    struct app_manifest man;
    uint64_t blen, pack;
    int pg[3];

    proc_finish_pending();
    for (int i = 0; i < EL0_SLOTS; i++) {
        if (!procs[i].alive) {
            p = &procs[i];
            p->slot = i;
            break;
        }
    }
    if (!p)
        return -1;
    if (p->gen == 0)
        p->gen = 1;
    for (int i = 0; i < EL0_Q_GLOBAL; i++) {
        if (qq[i].used && qq[i].slot == p->slot)
            return -1;
    }
    blen = (uint64_t)(user_blob_end - user_blob);
    if (blen == 0 || blen > 4096)
        return -1;
    for (int i = 0; i < 3; i++)
        pg[i] = -1;
    for (int i = 0; i < 3; i++) {
        uint64_t pa = 0;
        pg[i] = alloc_page(&pa);
        if (pg[i] < 0) {
            for (int k = 0; k < i; k++)
                free_idx(pg[k]);
            return -1;
        }
        p->pages[i] = pg[i];
        p->pa[i] = pa;
    }
    man.name = name;
    man.role = "el0-draw";
    man.mem_min_kb = 16;
    man.mem_sugg_kb = 16;
    man.cpu_min_us = 0;
    man.cpu_sugg_us = 0;
    if (resource_check(&man) != ADMIT_OK) {
        for (int i = 0; i < 3; i++)
            free_idx(p->pages[i]);
        printf("%s", resource_reason());
        return -1;
    }
    p->app = resource_commit(&man);
    if (!p->app) {
        for (int i = 0; i < 3; i++)
            free_idx(p->pages[i]);
        return -1;
    }
    p->asid = p->slot + 1;
    p->vx = vx;
    p->vy = vy;
    p->vw = vw;
    p->vh = vh;
    p->submit_ok = 1;
    p->hold = 0;
    p->submits = p->rej_range = p->rej_freq = p->rej_depth = 0;
    p->sub_period = ~0ull;
    p->sub_n = 0;
    build_pt(p);
    memcpy((void *)(uintptr_t)p->pa[0], user_blob, (size_t)blen);
    clean_code((void *)(uintptr_t)p->pa[0], blen);
    tlb_asid(p->asid);
    p->task = task_create(name, TIER3, KIND_SPIN);
    pack = (uint64_t)vx | ((uint64_t)vy << 16) | ((uint64_t)vw << 32) | ((uint64_t)vh << 48);
    task_arm_el0(p->task, 0, USER_SP, role, limit, foreign_pa, fb_pa, kern_va, pack);
    p->task->user_ttbr = p->ttbr;
    p->pid = p->task->id;
    if (spawn_logs < 2) {
        int lv = 0;
        uint64_t pc = walk(p, CODE_VA, &lv);
        uint64_t ps = walk(p, STACK_VA, &lv);
        uint64_t pd = walk(p, DRAW_VA, &lv);
        uint64_t pkern = walk(p, kern_va, &lv);
        uint64_t pdev = walk(p, 0x09000000ull, &lv);
        uint64_t pfb = walk(p, fb_pa, &lv);
        printf("[map] pid=%d handle=0x%lx gen=%u asid=%d ttbr=0x%lx draw_pa=0x%lx\n",
               p->pid, (unsigned long)make_handle(p), p->gen, p->asid, p->ttbr, p->pa[2]);
        printf("[map] pte code=0x%lx stack=0x%lx draw=0x%lx ap_el0=%d/%d/%d\n",
               pc, ps, pd, (int)((pc >> 6) & 1), (int)((ps >> 6) & 1), (int)((pd >> 6) & 1));
        printf("[map] walk kern=0x%lx pte=0x%lx ap_el0=%d dev pte=0x%lx ap_el0=%d fb pte=0x%lx ap_el0=%d\n",
               kern_va, pkern, (int)((pkern >> 6) & 1), pdev, (int)((pdev >> 6) & 1),
               pfb, (int)((pfb >> 6) & 1));
    } else {
        printf("[map] pid=%d handle=0x%lx gen=%u asid=%d draw_pa=0x%lx\n",
               p->pid, (unsigned long)make_handle(p), p->gen, p->asid, p->pa[2]);
    }
    spawn_logs++;
    {
        uint64_t off0, published;
        irq_off_begin(&off0);
        p->alive = 1;
        task_ready(p->task);
        published = make_handle(p);
        irq_off_end(off0);
        return (int64_t)published;
    }
}

void proc_ledger(const char *tag)
{
    int procs_n = 0;
    for (int i = 0; i < EL0_SLOTS; i++)
        procs_n += procs[i].alive ? 1 : 0;
    printf("[ledger] %s procs=%d pages_free=%d pages_used=%d queue=%d holds=%d mem_kb=%lu running=%d refusals=%d stale_drain=%d\n",
           tag, procs_n, pages_free_n(), pages_used_n(), proc_queue_depth(), proc_holds(),
           resource_mem_min_sum_kb(), resource_running_count(), unmap_refusals, stale_drain);
    for (int i = 0; i < EL0_SLOTS; i++) {
        struct proc *p = &procs[i];
        if (!p->alive)
            continue;
        printf("[ledger] proc pid=%d gen=%u asid=%d handle=0x%lx ttbr=0x%lx draw=0x%lx submits=%u range=%u freq=%u depth=%u hold=%d q=%d\n",
               p->pid, p->gen, p->asid, (unsigned long)make_handle(p), p->ttbr, p->pa[2],
               p->submits, p->rej_range, p->rej_freq, p->rej_depth, p->hold,
               queue_for_gen(p->slot, p->gen));
    }
}

void proc_on_switch(struct task *next)
{
    /* Do not switch TTBR0 here. Kernel code is still running at its link address.
     * gate_eret installs this value on the way back to EL0. */
    if (next && next->user_ttbr)
        gate_user_ttbr = next->user_ttbr;
    else
        gate_user_ttbr = 0;
}

static void init_gate_map(void)
{
    uint64_t gva = (uint64_t)(uintptr_t)gate_vectors & ~4095ull;
    uint64_t l2i = (gva >> 21) & 511ull;
    uint64_t l3i = (gva >> 12) & 511ull;
    memset(el0_l2_hi, 0, sizeof el0_l2_hi);
    memset(el0_l3_gate, 0, sizeof el0_l3_gate);
    el0_l3_gate[l3i] = (gva & ~4095ull) | PTE_VALID | PTE_PAGE | PTE_AF | SH_INNER | PTE_UXN;
    el0_l2_hi[l2i] = (uint64_t)(uintptr_t)el0_l3_gate | PTE_VALID | PTE_PAGE;
    printf("[proc] EL0 table maps the app pages plus one EL1-only gate page at 0x%lx. It does not map the kernel RAM block, devices, or the framebuffer.\n",
           gva);
    printf("[proc] permission-only sharing of the kernel L2 is not what this table does.\n");
}

void proc_init(void)
{
    memset(procs, 0, sizeof procs);
    memset(qq, 0, sizeof qq);
    memset(pool_used, 0, sizeof pool_used);
    for (int i = 0; i < EL0_SLOTS; i++) {
        procs[i].slot = i;
        for (int k = 0; k < 3; k++)
            procs[i].pages[k] = -1;
    }
    kernel_ttbr = mmu_kernel_ttbr();
    gate_kernel_ttbr = kernel_ttbr;
    gate_user_ttbr = 0;
    init_gate_map();
    printf("[proc] EL0 slots %d pool %d draw %u queue %d/%d freq %d/period. target %u us is a goal, not a measured bound.\n",
           EL0_SLOTS, EL0_POOL, EL0_DRAW, EL0_Q_PER, EL0_Q_GLOBAL, EL0_FREQ, EL0_REAP_TARGET_US);
    printf("[proc] kernel ttbr 0x%lx. Handle is sampled before the task can run. Reap time is CNTVCT, not the IRQ period counter.\n",
           kernel_ttbr);
}

uint64_t proc_irq_off_max_us(void)
{
    return ticks_to_us(irq_off_max);
}
