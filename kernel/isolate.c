/*
 * EL0 isolation exercise. Two user programs share the display service.
 * Kill and fault are each repeated EL0_*_ROUNDS times. The 20 ms figure is
 * the goal (two 10 ms periods). A late round is a failed round; pages that
 * the queue still holds are not freed to make the number.
 * Tier-1 deadlines are not accepted here.
 */
#include "quotaos.h"

#define AX 48
#define AY 96
#define BX 400
#define BY 96
#define VW 32
#define VH 32
#define WAIT_MAX 4000

static void yld(void)
{
    asm volatile("svc #0" ::: "memory");
}

static void pump(uint64_t us, int drain)
{
    if (clock_is_icount())
        us = us > 2000 ? 2000 : us;
    uint64_t t = now_us();
    while (now_us() - t < us) {
        yld();
        if (drain)
            disp_drain(8);
    }
}

static int wait_dead(int64_t h, int max)
{
    int n = 0;
    while (n < max && proc_alive(h)) {
        yld();
        n++;
    }
    proc_finish_pending();
    return n;
}

static int wait_queue(int64_t h, int max)
{
    int n = 0;
    while (n < max && proc_alive(h) && proc_queue_for(h) < 1) {
        yld();
        n++;
    }
    return n;
}

static int wait_period(void)
{
    uint64_t p = sched_period_now();
    int n = 0;
    while (n < 20000 && sched_period_now() == p) {
        yld();
        n++;
    }
    return n;
}

static int a_gone(int64_t handle, uint32_t gen)
{
    int slot = (int)(handle & 0xff) - 1;
    return proc_pages_used() == 3 && resource_mem_min_sum_kb() == 16 &&
           resource_running_count() == 1 && proc_queued_slot(slot, gen) == 0 &&
           proc_unmap_refusals() == 0;
}

static int baseline_empty(void)
{
    return proc_pages_used() == 0 && proc_pages_free() == 16 && proc_queue_depth() == 0 &&
           proc_holds() == 0 && resource_mem_min_sum_kb() == 0 && resource_running_count() == 0 &&
           proc_unmap_refusals() == 0;
}

static int late_of(const struct el0_reap *r)
{
    /* CNTVCT microseconds. The scheduler period counter does not move while IRQs are off. */
    return r->us > EL0_REAP_TARGET_US;
}

static uint32_t peek(int64_t h)
{
    uint64_t pa = proc_draw_pa(h);
    if (!pa)
        return 0;
    return *(volatile uint32_t *)(uintptr_t)pa;
}

static void rect(uint32_t *fb, uint32_t fw, int x, int y, int w, int h, uint32_t color)
{
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            fb[(uint32_t)(y + row) * fw + (uint32_t)(x + col)] = color;
}

static void note_us(uint64_t us, uint64_t periods, uint64_t *mn, uint64_t *mx, uint64_t *pmax, int *have)
{
    if (!*have || us < *mn)
        *mn = us;
    if (us > *mx)
        *mx = us;
    if (periods > *pmax)
        *pmax = periods;
    *have = 1;
}

static void report(const char *kind, int i, const struct el0_reap *r, int stale, int fresh,
                   int ledger, uint32_t bsub)
{
    printf("[isolate] %s %d pid=%d gen %u->%u us=%lu ticks=%lu cntpct=%lu late=%d hold %d->%d cancel=%d free %d->%d draw0=0x%x refused=%d stale=%d fresh=%d ledger=%d b_sub=%u\n",
           kind, i, r->pid, r->gen_before, r->gen_after, r->us, r->ticks, r->cntpct, late_of(r),
           r->hold_before, r->hold_after, r->cancelled, r->free_before, r->free_after, r->draw0,
           r->unmap_refused, stale, fresh, ledger, bsub);
}

static int64_t spawn_a(uint64_t role, uint64_t limit, uint64_t foreign, uint64_t fb, uint64_t kern)
{
    return proc_spawn("el0-a", role, limit, foreign, fb, kern, AX, AY, VW, VH);
}

static void steps(const struct el0_reap *r)
{
    printf("[reap] pid=%d why=%s step=1 revoke submit gen %u -> %u\n",
           r->pid, r->why, r->gen_before, r->gen_after);
    printf("[reap] pid=%d step=2 cancel inflight hold_before=%d cancelled=%d\n",
           r->pid, r->hold_before, r->cancelled);
    printf("[reap] pid=%d step=3 unmap refused=%d hold_after=%d free %d -> %d draw0=0x%x\n",
           r->pid, r->unmap_refused, r->hold_after, r->free_before, r->free_after, r->draw0);
    printf("[reap] pid=%d measured us=%lu ticks=%lu periods=%lu target_us=%u late=%d\n",
           r->pid, r->us, r->ticks, r->periods, EL0_REAP_TARGET_US, late_of(r));
}

void isolate_run(void)
{
    uint64_t fb = 0, fbn = 0, kern = (uint64_t)kmain;
    uint32_t fw = 0, fh = 0;
    int64_t b, a, third;
    struct app_manifest big = {"too-big", "probe", 0xffffffffu, 0xffffffffu, 0, 0};
    int over;
    int kill_ok = 0, kill_late = 0, kill_stale_n = 0, kill_have = 0;
    int fault_ok = 0, fault_late = 0, fault_stale_n = 0, fault_have = 0;
    uint64_t kill_min = 0, kill_max = 0, kill_pmax = 0;
    uint64_t fault_min = 0, fault_max = 0, fault_pmax = 0;
    uint32_t b_at_kill = 0, b_after_kill = 0, b_after_fault = 0;
    struct task *vsync, *tsrv;
    uint32_t v0 = 0, t0j = 0;

    printf("[isolate] rounds kill=%d fault=%d target_us=%u (2 periods). Target is not a prior result.\n",
           EL0_KILL_ROUNDS, EL0_FAULT_ROUNDS, EL0_REAP_TARGET_US);
    printf("[isolate] queue rejects when full. Drain runs on this shell, not in display-vsync. Tier-1 is not accepted here.\n");
    if (fb_present() != 0) {
        printf("[isolate] framebuffer was not published\n");
        return;
    }
    if (fb_reserved_range(&fb, &fbn) != 0)
        return;
    fb_geometry(&fw, &fh);
    rect((uint32_t *)(uintptr_t)fb, fw, AX - 4, AY - 4, VW + 8, VH + 8, 0x00ffffffu);
    rect((uint32_t *)(uintptr_t)fb, fw, BX - 4, BY - 4, VW + 8, VH + 8, 0x00ffffffu);
    proc_ledger("baseline");
    if (!baseline_empty())
        printf("[isolate] baseline was not empty\n");

    b = proc_spawn("el0-b", 0, 0x7fffffff, 0, fb, kern, BX, BY, VW, VH);
    a = spawn_a(0, 0x7fffffff, 0, fb, kern);
    third = proc_spawn("el0-c", 0, 1, 0, fb, kern, AX, AY, VW, VH);
    printf("[isolate] spawn b=0x%lx a=0x%lx third=%ld\n", (unsigned long)b, (unsigned long)a, (long)third);
    over = resource_check(&big);
    printf("[isolate] oversize admit %d\n%s", over, resource_reason());
    proc_ledger("both-live");

    printf("[isolate] shot both\n");
    {
        uint32_t a0 = peek(a), b0 = peek(b);
        pump(2000000, 1);
        printf("[isolate] pixels A 0x%x -> 0x%x B 0x%x -> 0x%x b_sub %u -> %u\n",
               a0, peek(a), b0, peek(b), 0u, proc_submits(b));
    }

    vsync = sched_find("display-vsync");
    tsrv = sched_find("timer-server");
    if (vsync)
        v0 = vsync->jobs;
    if (tsrv)
        t0j = tsrv->jobs;
    b_at_kill = proc_submits(b);

    for (int i = 0; i < EL0_KILL_ROUNDS; i++) {
        struct el0_reap r;
        int64_t old;
        int stale, fresh, led, y;
        if (!proc_alive(a))
            a = spawn_a(0, 0x7fffffff, 0, fb, kern);
        y = wait_queue(a, WAIT_MAX);
        if (proc_queue_for(a) < 1)
            printf("[isolate] kill %d inflight missing yields=%d\n", i, y);
        if (proc_kill(a) != 0)
            printf("[isolate] kill %d proc_kill rejected a live handle\n", i);
        proc_last_reap(&r);
        led = a_gone(a, r.gen_before) && r.hold_before >= 1 && r.hold_after == 0 &&
              r.cancelled >= 1 && r.unmap_refused == 0 && proc_alive(a) == 0;
        if (i == 0) {
            steps(&r);
            printf("[isolate] shot after\n");
            {
                uint32_t b0 = peek(b);
                pump(2000000, 1);
                printf("[isolate] after kill pixels B 0x%x -> 0x%x b_sub=%u\n", b0, peek(b), proc_submits(b));
            }
        }
        old = a;
        a = spawn_a(0, 0x7fffffff, 0, fb, kern);
        stale = proc_submit_handle(old);
        fresh = proc_alive(a) ? proc_submit_handle(a) : -2;
        if (stale == -1)
            kill_stale_n++;
        if (late_of(&r))
            kill_late++;
        note_us(r.us, r.periods, &kill_min, &kill_max, &kill_pmax, &kill_have);
        if (led && !late_of(&r) && stale == -1 && fresh != -1 && proc_alive(b))
            kill_ok++;
        report("kill", i, &r, stale, fresh, led, proc_submits(b));
    }
    b_after_kill = proc_submits(b);

    if (proc_alive(a))
        proc_kill(a);
    disp_drain(8);

    for (int i = 0; i < EL0_FAULT_ROUNDS; i++) {
        struct el0_reap r;
        struct el0_fault ft;
        int64_t h, h2, old;
        int stale, fresh, led, y, kind_ok;
        uint32_t gen2;
        h = spawn_a(2, 1, proc_draw_pa(b), fb, kern);
        y = wait_dead(h, WAIT_MAX);
        proc_last_reap(&r);
        proc_last_fault(&ft);
        if (proc_alive(h))
            printf("[isolate] fault %d still alive yields=%d\n", i, y);
        kind_ok = strcmp(ft.kind, "kernel") == 0 && ft.ec == 0x24 && (ft.spsr & 0xfu) == 0 &&
                  ft.far == kern && ft.ap_el0 == 0;
        led = a_gone(h, r.gen_before) && r.hold_before >= 1 && r.hold_after == 0 &&
              r.cancelled >= 1 && r.unmap_refused == 0 && r.why[0] == 'f';
        old = h;
        h2 = spawn_a(0, 0x7fffffff, 0, fb, kern);
        stale = proc_submit_handle(old);
        fresh = proc_alive(h2) ? proc_submit_handle(h2) : -2;
        if (stale == -1)
            fault_stale_n++;
        if (late_of(&r))
            fault_late++;
        note_us(r.us, r.periods, &fault_min, &fault_max, &fault_pmax, &fault_have);
        if (led && kind_ok && !late_of(&r) && stale == -1 && fresh != -1 && proc_alive(b))
            fault_ok++;
        report("fault", i, &r, stale, fresh, led && kind_ok, proc_submits(b));
        gen2 = 0;
        if (proc_alive(h2)) {
            struct el0_reap c;
            proc_kill(h2);
            proc_last_reap(&c);
            gen2 = c.gen_before;
            if (c.unmap_refused || proc_queued_gen(gen2))
                printf("[isolate] fault %d cleanup refused=%d queued=%d\n",
                       i, c.unmap_refused, proc_queued_gen(gen2));
        }
    }
    b_after_fault = proc_submits(b);

    {
        struct spec {
            const char *name;
            uint64_t role;
            uint64_t far_expect;
            int check_far;
            uint32_t ec;
        } specs[5];
        specs[0].name = "device";
        specs[0].role = 3;
        specs[0].far_expect = 0x09000000ull;
        specs[0].check_far = 1;
        specs[0].ec = 0x24;
        specs[1].name = "other-app";
        specs[1].role = 4;
        specs[1].far_expect = proc_draw_pa(b);
        specs[1].check_far = 1;
        specs[1].ec = 0x24;
        specs[2].name = "framebuffer";
        specs[2].role = 5;
        specs[2].far_expect = fb;
        specs[2].check_far = 1;
        specs[2].ec = 0x24;
        specs[3].name = "unmapped";
        specs[3].role = 8;
        specs[3].far_expect = 0x3000ull;
        specs[3].check_far = 1;
        specs[3].ec = 0x24;
        specs[4].name = "exec-kernel";
        specs[4].role = 9;
        specs[4].far_expect = kern;
        specs[4].check_far = 1;
        specs[4].ec = 0x20;
        for (int i = 0; i < 5; i++) {
            struct el0_reap r;
            struct el0_fault ft;
            int64_t h = spawn_a(specs[i].role, 1, proc_draw_pa(b), fb, kern);
            int y = wait_dead(h, WAIT_MAX);
            int64_t h2;
            int stale, led;
            proc_last_reap(&r);
            proc_last_fault(&ft);
            led = a_gone(h, r.gen_before);
            h2 = spawn_a(0, 1, 0, fb, kern);
            stale = proc_submit_handle(h);
            printf("[isolate] class %s yields=%d from_el0=%d ec=0x%x esr=0x%lx elr=0x%lx far=0x%lx class=%s ap_el0=%d spsr=0x%lx gen %u->%u h=0x%lx h2=0x%lx same=%d stale=%d alive=%d\n",
                   specs[i].name, y, (ft.spsr & 0xfu) == 0, ft.ec, ft.esr, ft.elr, ft.far, ft.kind,
                   ft.ap_el0, ft.spsr, r.gen_before, r.gen_after, (unsigned long)h, (unsigned long)h2,
                   h == h2, stale, proc_alive(h));
            printf("[isolate] class %s match_ec=%d match_far=%d match_name=%d ledger=%d\n",
                   specs[i].name, ft.ec == specs[i].ec,
                   !specs[i].check_far || ft.far == specs[i].far_expect,
                   strcmp(ft.kind, specs[i].name) == 0 ||
                       (specs[i].role == 9 && strcmp(ft.kind, "kernel") == 0),
                   led);
            if (proc_alive(h2))
                proc_kill(h2);
            disp_drain(8);
        }
    }

    {
        struct el0_reap r;
        int64_t h = spawn_a(7, 1, 0, fb, kern);
        int y = wait_dead(h, WAIT_MAX);
        proc_last_reap(&r);
        printf("[isolate] range yields=%d submits=%u rej_range=%u rej_freq=%u cancel=%d us=%lu why=%s\n",
               y, r.submits, r.rej_range, r.rej_freq, r.cancelled, r.us, r.why);
    }

    {
        int64_t h = spawn_a(14, 0, 0, fb, kern);
        uint32_t s0, f0, d0, s1, f1, d1;
        int y;
        struct el0_reap r;
        uint64_t flag;
        if (h <= 0 || !proc_alive(h)) {
            printf("[isolate] scripted spawn failed\n");
        } else {
        disp_drain(8);
        s0 = proc_submits(h);
        flag = proc_stack_pa(h);
        if (!flag) {
            printf("[isolate] scripted flag page missing\n");
        } else {
        *(volatile uint32_t *)(uintptr_t)flag = 3;
        y = 0;
        while (y < WAIT_MAX && proc_submits(h) + proc_rej_freq(h) + proc_rej_depth(h) < s0 + 3) {
            yld();
            y++;
        }
        s1 = proc_submits(h);
        f1 = proc_rej_freq(h);
        d1 = proc_rej_depth(h);
        printf("[isolate] freq burst yields=%d submits %u->%u freq=%u depth=%u\n",
               y, s0, s1, f1, d1);
        y = wait_period();
        s0 = proc_submits(h);
        f0 = proc_rej_freq(h);
        d0 = proc_rej_depth(h);
        *(volatile uint32_t *)(uintptr_t)proc_stack_pa(h) = 1;
        {
            int n = 0;
            while (n < WAIT_MAX &&
                   proc_submits(h) + proc_rej_freq(h) + proc_rej_depth(h) < s0 + f0 + d0 + 1) {
                yld();
                n++;
            }
            printf("[isolate] depth after period yields=%d period_yields=%d submits=%u freq=%u depth=%u q=%d\n",
                   n, y, proc_submits(h), proc_rej_freq(h), proc_rej_depth(h), proc_queue_for(h));
        }
        s0 = proc_submits(h);
        d0 = proc_rej_depth(h);
        *(volatile uint32_t *)(uintptr_t)proc_stack_pa(h) = 1;
        {
            int n = 0;
            while (n < WAIT_MAX && proc_rej_depth(h) == d0 && proc_alive(h)) {
                yld();
                n++;
            }
            printf("[isolate] depth overflow yields=%d submits=%u freq=%u depth %u->%u q=%d\n",
                   n, proc_submits(h), proc_rej_freq(h), d0, proc_rej_depth(h), proc_queue_for(h));
            (void)s0;
        }
        *(volatile uint32_t *)(uintptr_t)proc_stack_pa(h) = 2;
        y = wait_dead(h, WAIT_MAX);
        proc_last_reap(&r);
        printf("[isolate] scripted exit yields=%d us=%lu hold_before=%d cancel=%d\n",
               y, r.us, r.hold_before, r.cancelled);
        disp_drain(8);
        }
        }
    }

    {
        int bad = 0;
        for (int i = 0; i < 16; i++) {
            struct el0_fault ft;
            struct el0_reap r;
            int64_t h = spawn_a(9, 1, 0, fb, kern);
            int y = wait_dead(h, WAIT_MAX);
            int64_t h2;
            int st, al;
            proc_last_fault(&ft);
            proc_last_reap(&r);
            h2 = spawn_a(0, 1, 0, fb, kern);
            st = proc_submit_handle(h);
            al = proc_alive(h);
            printf("[exec-re] %d yields=%d from_el0=%d ec=0x%x esr=0x%lx elr=0x%lx far=0x%lx spsr=0x%lx gen %u->%u h=0x%lx h2=0x%lx same=%d stale=%d alive=%d\n",
                   i, y, (ft.spsr & 0xfu) == 0, ft.ec, ft.esr, ft.elr, ft.far, ft.spsr,
                   r.gen_before, r.gen_after, (unsigned long)h, (unsigned long)h2, h == h2, st, al);
            if (st != -1 || al || h == h2 || (ft.spsr & 0xfu) != 0 || ft.ec != 0x20)
                bad++;
            if (proc_alive(h2))
                proc_kill(h2);
        }
        printf("[exec-re] bad=%d of 16\n", bad);
    }

    {
        struct task *vs = sched_find("display-vsync");
        uint32_t m0 = vs ? vs->misses : 0;
        uint64_t deadline_us = vs ? ticks_to_us(vs->deadline_t) : 0;
        for (int i = 0; i < 20; i++) {
            int64_t h = spawn_a(0, 0x7fffffff, 0, fb, kern);
            int y = 0;
            uint64_t guard;
            while (y < 200 && proc_alive(h) && proc_queue_for(h) < 1) {
                yld();
                y++;
            }
            guard = sched_next_guard(ticks_now());
            while (ticks_now() + us_to_ticks(30) < guard)
                yld();
            proc_kill(h);
        }
        printf("[t1] display-vsync deadline_us=%lu misses %u -> %u irq_off_max_us=%lu\n",
               deadline_us, m0, vs ? vs->misses : 0, proc_irq_off_max_us());
        if (vs && vs->start_ts >= vs->release_ts && vs->release_ts)
            printf("[t1] last release-to-start us=%lu\n", ticks_to_us(vs->start_ts - vs->release_ts));
    }

    {
        int y = wait_queue(b, WAIT_MAX);
        struct el0_reap r;
        uint32_t before = proc_submits(b);
        proc_kill(b);
        proc_last_reap(&r);
        steps(&r);
        printf("[isolate] survivor exit yields=%d b_sub=%u us=%lu ticks=%lu periods=%lu\n",
               y, before, r.us, r.ticks, r.periods);
    }
    disp_drain(8);
    proc_ledger("final");
    printf("[isolate] empty=%d\n", baseline_empty());
    printf("[isolate] kill_rounds %d ok %d late %d stale_reject %d us_min %lu us_max %lu period_max %lu\n",
           EL0_KILL_ROUNDS, kill_ok, kill_late, kill_stale_n,
           kill_have ? kill_min : 0, kill_max, kill_pmax);
    printf("[isolate] fault_rounds %d ok %d late %d stale_reject %d us_min %lu us_max %lu period_max %lu\n",
           EL0_FAULT_ROUNDS, fault_ok, fault_late, fault_stale_n,
           fault_have ? fault_min : 0, fault_max, fault_pmax);
    printf("[isolate] survivor submits at_kill=%u after_kill=%u after_fault=%u\n",
           b_at_kill, b_after_kill, b_after_fault);
    if (vsync)
        printf("[isolate] display-vsync jobs %u -> %u (not a deadline result)\n", v0, vsync->jobs);
    if (tsrv)
        printf("[isolate] timer-server jobs %u -> %u (not a deadline result)\n", t0j, tsrv->jobs);
    printf("[isolate] target_us %u late_kill %d late_fault %d. Pass/fail uses CNTVCT us, not the scheduler period counter.\n",
           EL0_REAP_TARGET_US, kill_late, fault_late);
    printf("[isolate] irq_off_max_us=%lu. That is the longest continuous DAIF.I stretch measured with CNTVCT.\n",
           proc_irq_off_max_us());
    printf("[isolate] DONE\n");
}
