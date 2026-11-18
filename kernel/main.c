#include "quotaos.h"
#include "commit.h"

extern char __image_start[], __image_end[];

void kmain(uint64_t dtb)
{
    asm volatile("msr daifset, #2" ::: "memory");
    uart_init();
    mmu_init();
    proc_init();
    printf("\nQuota OS microkernel\n");
    printf("Version: %s (%s)\n", QUOTAOS_VERSION, QUOTAOS_COMMIT);
    printf("基于三级资源声明的微内核，\n保证一级任务永远不卡顿、二级任务运行不卡顿，三级任务则不保证。\n");
    {
        uint64_t cel, sctlr, spsel;
        asm volatile("mrs %0, CurrentEL" : "=r"(cel));
        asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        asm volatile("mrs %0, spsel" : "=r"(spsel));
        unsigned el = (unsigned)(cel >> 2);
        unsigned mmu = (unsigned)(sctlr & 1u);
        if (el == 1 && spsel == 1 && mmu == 0)
            printf("[boot] EL1h, MMU off. No hardware isolation.\n");
        else
            printf("[boot] EL%u SPSel=%u SCTLR_EL1.M=%u\n", el, (unsigned)spsel, mmu);
    }

    _Static_assert(sizeof(struct irq_frame) == 272, "irq frame must match vectors.S");

    uint64_t ram = dtb_ram_bytes(dtb);
    resource_init(ram);
    color_init(ram);
    printf("[boot] kernel image+bss %lu KB of the %u KB system reserve\n",
           (uint64_t)(__image_end - __image_start) / 1024, system_reserve_kb());
    gic_init();
    timer_init();
    printf("[boot] CNTFRQ %lu Hz (1 tick = %lu ns), virtual timer PPI 27\n", timer_freq(),
           1000000000ull / timer_freq());
    clock_detect();
    dev_init();
    printf("[boot] simulated device IRQ line: EL1 physical timer, PPI 30\n");

    sched_init();
    services_start();
    printf("[boot] tier-1 display-vsync and timer-server bound to their slots\n");
    sched_start(shell_main);
}
