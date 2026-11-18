/*
 * 中断控制器。类型和地址都从设备树的 interrupt-controller 节点读：
 *   "arm,cortex-a15-gic" / "arm,gic-400"  -> GICv2：distributor + 内存映射的 CPU interface
 *   "arm,gic-v3"                          -> GICv3：distributor + redistributor + ICC_* 系统寄存器
 * QEMU virt：gic-version=2 是 0x08000000 / 0x08010000；gic-version=3 是 0x08000000 / 0x080a0000。
 * 认不出来就打印原因并停住，不去碰不存在的寄存器（那会变成数据中止 PANIC）。
 *
 * 用两个 PPI：EL1 虚拟定时器 INTID 27（调度）；EL1 物理定时器 INTID 30，
 * v011 起当作模拟设备的中断线（kernel/dev.c），可以单独屏蔽。
 */
#include "quotaos.h"

#define TIMER_IRQ 27
#define DEV_IRQ 30
#define GICD_ICENABLER  0x0180
#define GICR_ICENABLER0 0x0180

#define GICD_CTLR       0x0000
#define GICD_ISENABLER  0x0100
#define GICD_IPRIORITY  0x0400

#define GICC_CTLR 0x000
#define GICC_PMR  0x004
#define GICC_BPR  0x008
#define GICC_IAR  0x00c
#define GICC_EOIR 0x010

#define GICD_CTLR_RWP     (1u << 31)
#define GICD_CTLR_ARE     (1u << 4)
#define GICD_CTLR_GRP1    (1u << 1)
#define GICR_WAKER        0x0014
#define GICR_TYPER        0x0008
#define GICR_STRIDE       0x20000ul
#define GICR_SGI          0x10000ul
#define GICR_IGROUPR0     0x0080
#define GICR_ISENABLER0   0x0100
#define GICR_IPRIORITYR   0x0400

static int gic_version;
static uint64_t gicd, gicc, gicr_sgi;

static void v2_init(void)
{
    mmio_wr(gicd + GICD_CTLR, 0);
    mmio_wr(gicc + GICC_CTLR, 0);
    /*
     * QEMU virt 默认 secure=off：没有安全世界，中断停在 Group 0，
     * 从 EL1 读 IGROUPR 会恒为 1，写进去并不会真的变成 Group 1。
     * 所以这里保持 Group 0，只打开转发、优先级和定时器 PPI。
     */
    for (int i = 0; i < 16; i++)
        mmio_wr(gicd + GICD_IPRIORITY + (uint64_t)i * 4, 0xa0a0a0a0u);
    mmio_wr(gicd + GICD_ISENABLER, 1u << TIMER_IRQ);   /* 设备线 30 由 kernel/dev.c 按需打开 */
    mmio_wr(gicc + GICC_PMR, 0xff);
    mmio_wr(gicc + GICC_BPR, 0);
    mmio_wr(gicd + GICD_CTLR, 1);
    mmio_wr(gicc + GICC_CTLR, 1);
}

static uint64_t mmio_rd64(uint64_t a)
{
    return (uint64_t)mmio_rd(a) | ((uint64_t)mmio_rd(a + 4) << 32);
}

/* 找到 affinity 和本核 MPIDR 一致的 redistributor 帧。 */
static uint64_t v3_find_redist(uint64_t base, uint64_t size)
{
    uint64_t mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint32_t aff = (uint32_t)(((mpidr >> 32) & 0xff) << 24 | (mpidr & 0xffffff));
    for (uint64_t off = 0; off + GICR_STRIDE <= size; off += GICR_STRIDE) {
        uint64_t typer = mmio_rd64(base + off + GICR_TYPER);
        if ((uint32_t)(typer >> 32) == aff)
            return base + off;
        if (typer & (1u << 4)) /* Last */
            break;
    }
    return 0;
}

static int v3_init(uint64_t gicr_base, uint64_t gicr_size)
{
    uint64_t rd = v3_find_redist(gicr_base, gicr_size);
    if (!rd) {
        printf("[boot] ERROR: GICv3 redistributor for this CPU not found in 0x%lx+0x%lx\n",
               gicr_base, gicr_size);
        return -1;
    }
    /* distributor：亲和路由 + Group 1。QEMU 没有 EL3 时 GIC 只有一个安全状态（DS=1）。 */
    mmio_wr(gicd + GICD_CTLR, 0);
    while (mmio_rd(gicd + GICD_CTLR) & GICD_CTLR_RWP)
        ;
    mmio_wr(gicd + GICD_CTLR, GICD_CTLR_ARE | GICD_CTLR_GRP1);
    while (mmio_rd(gicd + GICD_CTLR) & GICD_CTLR_RWP)
        ;

    /* 唤醒 redistributor。 */
    uint32_t w = mmio_rd(rd + GICR_WAKER);
    mmio_wr(rd + GICR_WAKER, w & ~(1u << 1));
    while (mmio_rd(rd + GICR_WAKER) & (1u << 2))
        ;

    uint64_t sgi = rd + GICR_SGI;
    gicr_sgi = sgi;
    mmio_wr(sgi + GICR_IGROUPR0, mmio_rd(sgi + GICR_IGROUPR0) | (1u << TIMER_IRQ) | (1u << DEV_IRQ));
    for (int i = 0; i < 8; i++)
        mmio_wr(sgi + GICR_IPRIORITYR + (uint64_t)i * 4, 0xa0a0a0a0u);
    mmio_wr(sgi + GICR_ISENABLER0, 1u << TIMER_IRQ);

    /* CPU interface 走系统寄存器。 */
    uint64_t sre;
    asm volatile("mrs %0, S3_0_C12_C12_5" : "=r"(sre));      /* ICC_SRE_EL1 */
    asm volatile("msr S3_0_C12_C12_5, %0; isb" :: "r"(sre | 1));
    asm volatile("mrs %0, S3_0_C12_C12_5" : "=r"(sre));
    if (!(sre & 1)) {
        printf("[boot] ERROR: GICv3 system-register interface is disabled (ICC_SRE_EL1.SRE=0)\n");
        return -1;
    }
    asm volatile("msr S3_0_C4_C6_0, %0" :: "r"(0xfful));      /* ICC_PMR_EL1 */
    asm volatile("msr S3_0_C12_C12_3, %0" :: "r"(0ul));       /* ICC_BPR1_EL1 */
    asm volatile("msr S3_0_C12_C12_7, %0; isb" :: "r"(1ul));  /* ICC_IGRPEN1_EL1 */
    return 0;
}

static void halt(void)
{
    printf("[boot] halted: no supported interrupt controller, the scheduler cannot run.\n");
    for (;;)
        asm volatile("wfi");
}

void gic_init(void)
{
    uint64_t reg[4];
    int n;
    if ((n = dtb_find_compatible("arm,gic-v3", reg, 4)) >= 4) {
        gicd = reg[0];
        gic_version = 3;
        printf("[boot] GICv3 from DTB: distributor 0x%lx, redistributor 0x%lx\n", reg[0], reg[2]);
        if (v3_init(reg[2], reg[3]) != 0)
            halt();
        return;
    }
    if ((n = dtb_find_compatible("arm,cortex-a15-gic", reg, 4)) >= 4 ||
        (n = dtb_find_compatible("arm,gic-400", reg, 4)) >= 4) {
        gicd = reg[0];
        gicc = reg[2];
        gic_version = 2;
        printf("[boot] GICv2 from DTB: distributor 0x%lx, cpu interface 0x%lx\n", gicd, gicc);
        v2_init();
        return;
    }
    printf("[boot] ERROR: no GICv2 (arm,cortex-a15-gic / arm,gic-400) or GICv3 (arm,gic-v3)\n");
    printf("       interrupt-controller node in the device tree.\n");
    halt();
}

uint32_t gic_ack(void)
{
    if (gic_version == 3) {
        uint64_t v;
        asm volatile("mrs %0, S3_0_C12_C12_0" : "=r"(v));    /* ICC_IAR1_EL1 */
        return (uint32_t)v;
    }
    return mmio_rd(gicc + GICC_IAR);
}

void gic_eoi(uint32_t iar)
{
    if (gic_version == 3) {
        asm volatile("msr S3_0_C12_C12_1, %0; isb" :: "r"((uint64_t)iar)); /* ICC_EOIR1_EL1 */
        return;
    }
    mmio_wr(gicc + GICC_EOIR, iar);
}

/* 打开或屏蔽一条 PPI（只用于设备线 30）。 */
void gic_ppi_enable(uint32_t id, int on)
{
    if (gic_version == 3)
        mmio_wr(gicr_sgi + (on ? GICR_ISENABLER0 : GICR_ICENABLER0), 1u << id);
    else
        mmio_wr(gicd + (on ? GICD_ISENABLER : GICD_ICENABLER), 1u << id);
}
