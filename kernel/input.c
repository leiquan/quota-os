/*
 * virtio-mmio input (device id 18), legacy or modern. Polled from the
 * shell task. Events go into a fixed queue. A full queue drops the new
 * event. There is no lock and this file does not mask IRQs.
 * LVGL is not included here.
 */
#include "quotaos.h"

#define VIRTIO_ID_INPUT 18u
#define VIRTQ_DESC_F_WRITE 2u
#define EV_SYN 0u
#define EV_KEY 1u
#define EV_REL 2u
#define EV_ABS 3u
#define REL_X 0u
#define REL_Y 1u
#define ABS_X 0u
#define ABS_Y 1u
#define BTN_LEFT 0x110u
#define BTN_TOUCH 0x14au
#define NDEV 2
#define QCAP 32

struct vdesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct evbuf {
    uint16_t type;
    uint16_t code;
    uint32_t value;
};

struct idev {
    uint64_t base;
    uint32_t qsz;
    uint32_t qmask;
    uint32_t used_off;
    uint8_t *vring;
    struct evbuf *events;
    uint16_t last_used;
    uint16_t avail_count;
    int kind; /* QUOTA_INPUT_KEY or QUOTA_INPUT_POINTER */
    int abs;
    int32_t min_x, max_x, min_y, max_y;
    int x, y, btn;
    int ready;
};

static struct idev devs[NDEV];
static int ndev;
static struct quota_input queue[QCAP];
static uint16_t qhead, qcount;
static uint32_t qdrop;
static uint32_t span_w = 640, span_h = 480;

static void dsb(void)
{
    asm volatile("dsb sy" ::: "memory");
}

static int contains(const char *s, const char *n)
{
    for (; *s; s++) {
        const char *a = s;
        const char *b = n;
        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (*b == 0)
            return 1;
    }
    return 0;
}

static unsigned vring_used_off(unsigned num, unsigned align)
{
    unsigned bytes = 16u * num + 2u * (3u + num);
    return (bytes + align - 1u) & ~(align - 1u);
}

static int setup_legacy(struct idev *d)
{
    mmio_wr(d->base + 0x28, 4096);
    mmio_wr(d->base + 0x30, 0);
    uint32_t max = mmio_rd(d->base + 0x34);
    if (max < 2)
        return -1;
    d->qsz = 8;
    if (d->qsz > max)
        d->qsz = max;
    d->qmask = d->qsz - 1;
    mmio_wr(d->base + 0x38, d->qsz);
    mmio_wr(d->base + 0x3c, 4096);
    mmio_wr(d->base + 0x40, (uint32_t)((uint64_t)d->vring / 4096u));
    d->used_off = vring_used_off(d->qsz, 4096);
    return 0;
}

static int setup_modern(struct idev *d)
{
    mmio_wr(d->base + 0x14, 1);
    uint32_t hi = mmio_rd(d->base + 0x10);
    mmio_wr(d->base + 0x24, 1);
    mmio_wr(d->base + 0x20, hi & 1u);
    mmio_wr(d->base + 0x14, 0);
    mmio_wr(d->base + 0x24, 0);
    mmio_wr(d->base + 0x20, 0);
    mmio_wr(d->base + 0x70, 1u | 2u | 8u);
    dsb();
    if (mmio_rd(d->base + 0x70) & 0x80u)
        return -1;
    mmio_wr(d->base + 0x30, 0);
    uint32_t max = mmio_rd(d->base + 0x34);
    if (max < 2)
        return -1;
    d->qsz = 8;
    if (d->qsz > max)
        d->qsz = max;
    d->qmask = d->qsz - 1;
    mmio_wr(d->base + 0x38, d->qsz);
    uint64_t desc = (uint64_t)d->vring;
    uint64_t avail = desc + 16u * d->qsz;
    uint64_t used = desc + d->used_off;
    mmio_wr(d->base + 0x80, (uint32_t)desc);
    mmio_wr(d->base + 0x84, (uint32_t)(desc >> 32));
    mmio_wr(d->base + 0x90, (uint32_t)avail);
    mmio_wr(d->base + 0x94, (uint32_t)(avail >> 32));
    mmio_wr(d->base + 0xa0, (uint32_t)used);
    mmio_wr(d->base + 0xa4, (uint32_t)(used >> 32));
    mmio_wr(d->base + 0x44, 1);
    return 0;
}

static void publish_all(struct idev *d)
{
    struct vdesc *desc = (struct vdesc *)d->vring;
    uint16_t *avail = (uint16_t *)(d->vring + 16u * d->qsz);
    for (uint32_t i = 0; i < d->qsz; i++) {
        desc[i].addr = (uint64_t)&d->events[i];
        desc[i].len = sizeof d->events[i];
        desc[i].flags = VIRTQ_DESC_F_WRITE;
        desc[i].next = 0;
        avail[2 + i] = (uint16_t)i;
    }
    avail[0] = 1;
    d->avail_count = (uint16_t)d->qsz;
    dsb();
    avail[1] = d->avail_count;
    dsb();
}

static void recycle(struct idev *d, uint16_t id)
{
    uint16_t *avail = (uint16_t *)(d->vring + 16u * d->qsz);
    avail[2 + (d->avail_count & d->qmask)] = id;
    d->avail_count++;
    dsb();
    avail[1] = d->avail_count;
    dsb();
    mmio_wr(d->base + 0x50, 0);
}

static void enqueue(uint8_t kind, uint16_t code, uint16_t value, int x, int y)
{
    if (qcount >= QCAP) {
        qdrop++;
        return;
    }
    struct quota_input *ev = &queue[(qhead + qcount) % QCAP];
    ev->kind = kind;
    ev->code = code;
    ev->value = value;
    ev->x = (int16_t)x;
    ev->y = (int16_t)y;
    qcount++;
}

static int clamp_span(int v, int limit)
{
    if (v < 0)
        return 0;
    if (v >= limit)
        return limit - 1;
    return v;
}

static int scale_abs(int32_t v, int32_t lo, int32_t hi, int span)
{
    if (hi <= lo || span <= 1)
        return 0;
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    return (int)(((int64_t)(v - lo) * (span - 1)) / (hi - lo));
}

static void read_name(struct idev *d, char *name, uint32_t cap)
{
    mmio_wr(d->base + 0x100, 0x01u);
    dsb();
    uint32_t sz = (mmio_rd(d->base + 0x100) >> 16) & 0xffu;
    if (sz > cap - 1)
        sz = cap - 1;
    memset(name, 0, cap);
    for (uint32_t i = 0; i < sz; i += 4) {
        uint32_t w = mmio_rd(d->base + 0x108 + i);
        memcpy(name + i, &w, 4);
    }
    name[sz] = 0;
}

static void read_abs(struct idev *d)
{
    uint8_t sels[2] = {ABS_X, ABS_Y};
    int32_t *minv[2] = {&d->min_x, &d->min_y};
    int32_t *maxv[2] = {&d->max_x, &d->max_y};
    d->abs = 0;
    for (int a = 0; a < 2; a++) {
        uint32_t sel = 0x12u | ((uint32_t)sels[a] << 8);
        mmio_wr(d->base + 0x100, sel);
        dsb();
        uint32_t sz = (mmio_rd(d->base + 0x100) >> 16) & 0xffu;
        if (sz < 8)
            return;
        int32_t lo = (int32_t)mmio_rd(d->base + 0x108);
        int32_t hi = (int32_t)mmio_rd(d->base + 0x10c);
        *minv[a] = lo;
        *maxv[a] = hi;
    }
    d->abs = 1;
}

static uint32_t raw_prints;

static void take_event(struct idev *d, struct evbuf *ev)
{
    if (raw_prints < 12) {
        raw_prints++;
        printf("[input] raw %s type %u code %u value %u\n",
               d->kind == QUOTA_INPUT_KEY ? "key" : "ptr", ev->type, ev->code, ev->value);
    }
    if (d->kind == QUOTA_INPUT_KEY) {
        if (ev->type == EV_KEY && ev->value != 2)
            enqueue(QUOTA_INPUT_KEY, ev->code, (uint16_t)ev->value, 0, 0);
        return;
    }
    if (ev->type == EV_ABS && d->abs) {
        if (ev->code == ABS_X)
            d->x = scale_abs((int32_t)ev->value, d->min_x, d->max_x, (int)span_w);
        else if (ev->code == ABS_Y)
            d->y = scale_abs((int32_t)ev->value, d->min_y, d->max_y, (int)span_h);
    } else if (ev->type == EV_REL) {
        if (ev->code == REL_X)
            d->x = clamp_span(d->x + (int)ev->value, (int)span_w);
        else if (ev->code == REL_Y)
            d->y = clamp_span(d->y + (int)ev->value, (int)span_h);
    } else if (ev->type == EV_KEY && (ev->code == BTN_LEFT || ev->code == BTN_TOUCH)) {
        d->btn = ev->value != 0;
    } else if (ev->type == EV_SYN && ev->code == 0) {
        enqueue(QUOTA_INPUT_POINTER, 0, (uint16_t)d->btn, d->x, d->y);
    }
}

static void drain(struct idev *d)
{
    if (!d->ready)
        return;
    uint32_t is = mmio_rd(d->base + 0x60);
    if (is)
        mmio_wr(d->base + 0x64, is);
    volatile uint16_t *uidx = (volatile uint16_t *)(d->vring + d->used_off + 2);
    dsb();
    uint16_t idx = *uidx;
    while (d->last_used != idx) {
        uint32_t slot = d->last_used & d->qmask;
        uint32_t *elem = (uint32_t *)(d->vring + d->used_off + 4 + slot * 8);
        uint32_t id = elem[0];
        d->last_used++;
        if (id < d->qsz)
            take_event(d, &d->events[id]);
        recycle(d, (uint16_t)id);
        dsb();
        idx = *uidx;
    }
}

void input_set_span(uint32_t w, uint32_t h)
{
    if (w)
        span_w = w;
    if (h)
        span_h = h;
}

uint32_t input_drop_count(void)
{
    return qdrop;
}

int input_service_poll(void)
{
    for (int i = 0; i < ndev; i++)
        drain(&devs[i]);
    return (int)qcount;
}

int input_pop(struct quota_input *ev)
{
    if (!qcount)
        return 0;
    *ev = queue[qhead];
    qhead = (uint16_t)((qhead + 1) % QCAP);
    qcount--;
    return 1;
}

int input_poll(uint16_t *code, uint16_t *value)
{
    input_service_poll();
    struct quota_input ev;
    while (input_pop(&ev)) {
        if (ev.kind != QUOTA_INPUT_KEY)
            continue;
        if (code)
            *code = ev.code;
        if (value)
            *value = ev.value;
        return 1;
    }
    return 0;
}

static int bringup(struct idev *d, uint32_t version)
{
    uint64_t ring_pa, ev_pa;
    if (kernel_reserve_alloc(8192, 4096, &ring_pa) != 0 ||
        kernel_reserve_alloc(4096, 4096, &ev_pa) != 0) {
        printf("[input] system reserve has no room\n");
        return -1;
    }
    d->vring = (uint8_t *)ring_pa;
    d->events = (struct evbuf *)ev_pa;
    memset(d->vring, 0, 8192);
    memset(d->events, 0, 4096);
    d->used_off = vring_used_off(8, 4096);
    mmio_wr(d->base + 0x70, 0);
    dsb();
    mmio_wr(d->base + 0x70, 1);
    mmio_wr(d->base + 0x70, 1u | 2u);
    int rc = (version == 2) ? setup_modern(d) : (version == 1) ? setup_legacy(d) : -1;
    if (rc != 0) {
        printf("[input] queue setup failed at 0x%lx version %u\n", d->base, version);
        return -1;
    }
    publish_all(d);
    uint32_t st = mmio_rd(d->base + 0x70);
    mmio_wr(d->base + 0x70, st | 4u);
    dsb();
    mmio_wr(d->base + 0x50, 0);
    char name[56];
    read_name(d, name, sizeof name);
    if (contains(name, "eyboard"))
        d->kind = QUOTA_INPUT_KEY;
    else
        d->kind = QUOTA_INPUT_POINTER;
    if (d->kind == QUOTA_INPUT_POINTER)
        read_abs(d);
    d->ready = 1;
    d->last_used = 0;
    printf("[input] %s \"%s\" base 0x%lx queue %u abs %d range x %d..%d y %d..%d\n",
           d->kind == QUOTA_INPUT_KEY ? "keyboard" : "pointer",
           name[0] ? name : "?", d->base, d->qsz, d->abs, d->min_x, d->max_x, d->min_y, d->max_y);
    return 0;
}

int input_init(void)
{
    uint64_t bases[32];
    int n = dtb_find_bases("virtio,mmio", bases, 32);
    printf("[input] virtio-mmio nodes %d queue cap %d\n", n, QCAP);
    int keys = 0, ptrs = 0;
    for (int i = 0; i < n && ndev < NDEV; i++) {
        if (mmio_rd(bases[i]) != 0x74726976u)
            continue;
        if (mmio_rd(bases[i] + 8) != VIRTIO_ID_INPUT)
            continue;
        uint32_t version = mmio_rd(bases[i] + 4);
        struct idev *d = &devs[ndev];
        memset(d, 0, sizeof *d);
        d->base = bases[i];
        if (bringup(d, version) != 0)
            continue;
        if (d->kind == QUOTA_INPUT_KEY) {
            if (keys) {
                d->ready = 0;
                continue;
            }
            keys = 1;
        } else {
            if (ptrs) {
                d->ready = 0;
                continue;
            }
            ptrs = 1;
        }
        ndev++;
    }
    if (!keys)
        printf("[input] no virtio keyboard\n");
    if (!ptrs)
        printf("[input] no virtio pointer\n");
    if (!ndev) {
        printf("[input] no virtio-input (id 18)\n");
        return -1;
    }
    return 0;
}
