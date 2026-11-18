/*
 * EL1 异常向量。调度只走 IRQ（当前异常级、使用 SP_ELx）。
 * 同步异常打印后停住，避免静默跑飞。
 *
 * 每个向量槽 128 字节，整张表 2KB 对齐，这是 VBAR_EL1 的硬件要求。
 */
/*
 * VBAR points at this page. It is the only kernel page in an EL0 table.
 * From EL0 the stub switches TTBR0 back to the kernel map before it touches
 * the kernel stack. EL1 slots just branch at the real handlers.
 */
.section .gate, "ax"
.align 11
.global gate_vectors
gate_vectors:
    b       unhandled_sync
    .align 7
    b       unhandled_irq
    .align 7
    b       unhandled_sync
    .align 7
    b       unhandled_sync
    .align 7
    b       el1_sync
    .align 7
    b       el1_irq
    .align 7
    b       unhandled_sync
    .align 7
    b       unhandled_sync
    .align 7
gate_el0_sync:
    msr     tpidr_el1, x16
    adr     x16, gate_kernel_ttbr
    ldr     x16, [x16]
    msr     ttbr0_el1, x16
    dsb     ish
    isb
    mrs     x16, tpidr_el1
    b       el0_sync
    .align 7
gate_el0_irq:
    msr     tpidr_el1, x16
    adr     x16, gate_kernel_ttbr
    ldr     x16, [x16]
    msr     ttbr0_el1, x16
    dsb     ish
    isb
    mrs     x16, tpidr_el1
    b       el0_irq
    .align 7
    b       unhandled_sync
    .align 7
    b       unhandled_sync
    .align 7
    b       unhandled_sync
    .align 7
    b       unhandled_irq
    .align 7
    b       unhandled_sync
    .align 7
    b       unhandled_sync

.align 3
.global gate_kernel_ttbr, gate_user_ttbr, gate_saved_x0
gate_kernel_ttbr: .quad 0
gate_user_ttbr:   .quad 0
gate_saved_x0:    .quad 0

.global gate_eret
gate_eret:
    msr     ttbr0_el1, x0
    dsb     ish
    tlbi    vmalle1
    dsb     ish
    isb
    adr     x0, gate_saved_x0
    ldr     x0, [x0]
    eret

.section .text
.align 11
.global vectors

.macro ventry label
    .align 7
    b \label
.endm

vectors:
    ventry unhandled_sync     /* 0x000 Current EL, SP0, Sync  */
    ventry unhandled_irq      /* 0x080 Current EL, SP0, IRQ   */
    ventry unhandled_sync     /* 0x100 FIQ */
    ventry unhandled_sync     /* 0x180 SError */

    ventry el1_sync           /* 0x200 Current EL, SPx, Sync */
    ventry el1_irq            /* 0x280 Current EL, SPx, IRQ  */
    ventry unhandled_sync     /* 0x300 FIQ */
    ventry unhandled_sync     /* 0x380 SError */

    ventry el0_sync           /* 0x400 Lower EL, AArch64 */
    ventry el0_irq
    ventry unhandled_sync
    ventry unhandled_sync

    ventry unhandled_sync     /* 0x600 Lower EL, AArch32 */
    ventry unhandled_irq
    ventry unhandled_sync
    ventry unhandled_sync

/*
 * 保存 x0-x30、ELR、SPSR。帧长 272，16 字节对齐。
 * 布局必须和 include/quotaos.h 里的 struct irq_frame 一致。
 */
.macro save_regs
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]
    mrs     x9, ELR_EL1
    mrs     x10, SPSR_EL1
    stp     x9, x10,  [sp, #248]
.endm

.macro restore_regs
    ldp     x9, x10, [sp, #248]
    msr     ELR_EL1, x9
    msr     SPSR_EL1, x10
    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30,      [sp, #240]
    add     sp, sp, #272
.endm

/*
 * 进出内核的时间戳和定时器装填，调度器（kernel/sched.c）读写这些变量：
 *   k_entry_ts    异常入口第一条指令读到的 CNTVCT
 *   k_eret_ts     出口 eret 前读到的 CNTVCT；任务这次运行记为 [k_eret_ts, 下一次 k_entry_ts)
 *   k_release_at  非 0：Tier 1 槽起点。出口先原地等到这一刻再 eret
 *   k_spin_late   等待循环退出时比 k_release_at 晚了多少 tick
 *   k_arm_abs     下一个绝对期限（槽尾、守卫起点、周期边界）
 *   k_arm_rel     非 0：预算剩余量，期限 = min(k_eret_ts + k_arm_rel, k_arm_abs)
 *   k_cval        实际写进 CNTV_CVAL 的值
 */
.macro stamp_entry
    stp     x16, x17, [sp, #-16]!
    mrs     x16, cntvct_el0
    adrp    x17, k_entry_ts
    str     x16, [x17, :lo12:k_entry_ts]
    ldp     x16, x17, [sp], #16
.endm

el1_irq:
    stamp_entry
    save_regs
    mov     x0, sp
    bl      handle_irq          /* 返回值 x0 = 下一个任务的栈（可能已切换） */
    b       k_exit

el1_sync:
    stamp_entry
    save_regs
    mov     x0, sp
    bl      handle_sync          /* SVC 主动让出时，返回值是下一个任务的栈 */
    b       k_exit

el0_sync:
    stamp_entry
    save_regs
    mrs     x9, sp_el0
    str     x9, [sp, #264]
    mov     x0, sp
    bl      handle_sync
    b       k_exit

el0_irq:
    stamp_entry
    save_regs
    mrs     x9, sp_el0
    str     x9, [sp, #264]
    mov     x0, sp
    bl      handle_irq
    b       k_exit

/*
 * 出口。等待循环每圈 3 条指令（icount 下 3 tick），所以退出时比槽起点晚 0、1 或 2 tick。
 * 晚 d tick 就少执行 d 条 nop，eret 落在槽起点之后固定的指令数上。
 */
k_exit:
    mov     sp, x0
    adrp    x0, k_release_at
    add     x0, x0, :lo12:k_release_at
    ldr     x1, [x0]
    cbz     x1, 5f
1:  mrs     x2, cntvct_el0
    cmp     x2, x1
    b.lo    1b
    sub     x3, x2, x1
    str     xzr, [x0]
    adrp    x4, k_spin_late
    str     x3, [x4, :lo12:k_spin_late]
    cmp     x3, #2
    b.hi    5f
    adr     x4, 4f
    add     x4, x4, x3, lsl #2
    br      x4
4:  nop
    nop
    /* 先恢复 x4-x30 和 ELR/SPSR，最后才读时间、写比较值，再恢复 x0-x3 就 eret：
     * 预算剩得很少时，比较值离 eret 只差几条指令，中断不会在关中断的出口里等。 */
5:  ldp     x9, x10, [sp, #248]
    msr     ELR_EL1, x9
    msr     SPSR_EL1, x10
    and     x9, x10, #0xf
    cbnz    x9, 7f
    ldr     x9, [sp, #264]
    msr     sp_el0, x9
7:  ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30,      [sp, #240]
    adr     x0, gate_user_ttbr
    ldr     x0, [x0]
    cbnz    x0, el0_leave
    adrp    x0, k_vars              /* k_eret_ts、k_arm_rel、k_arm_abs、k_cval 挨着放 */
    add     x0, x0, :lo12:k_vars
    ldp     x1, x3, [x0, #8]        /* x1 = k_arm_rel，x3 = k_arm_abs */
    mrs     x2, cntvct_el0          /* 从这里到 eret 共 10 条指令 */
    cbz     x1, 6f
    add     x1, x2, x1
    cmp     x1, x3
    csel    x3, x1, x3, lo
6:  msr     cntv_cval_el0, x3
    stp     x2, x3, [x0, #24]       /* k_eret_ts、k_cval（k_cval 紧跟 k_eret_ts） */
    ldp     x2, x3, [sp, #16]
    ldp     x0, x1, [sp], #272
    eret

el0_leave:
    /* x0 is the user TTBR. x4-x30 are already the user values. */
    adr     x1, gate_saved_x0
    ldr     x2, [sp]
    str     x2, [x1]
    adrp    x1, k_vars
    add     x1, x1, :lo12:k_vars
    ldr     x0, [x1, #8]
    ldr     x3, [x1, #16]
    mrs     x2, cntvct_el0
    cbz     x0, 9f
    add     x0, x2, x0
    cmp     x0, x3
    csel    x3, x0, x3, lo
9:  msr     cntv_cval_el0, x3
    stp     x2, x3, [x1, #24]
    adr     x0, gate_user_ttbr
    ldr     x0, [x0]
    ldr     x1, [sp, #8]
    ldp     x2, x3, [sp, #16]
    add     sp, sp, #272
    b       gate_eret

unhandled_sync:
    save_regs
    mov     x0, sp
    mov     x1, #1
    bl      handle_unhandled
    restore_regs
    eret

unhandled_irq:
    save_regs
    mov     x0, sp
    mov     x1, #2
    bl      handle_unhandled
    restore_regs
    eret

/* 第一次进入任务：x0 指向伪造好的 irq_frame。 */
.global start_first_task
start_first_task:
    b       k_exit

/*
 * 周期作业任务的循环。第一次被派上 CPU 就从 job_resume 开始，
 * 以后每次都从 svc 的下一条开始，所以 eret 之后第一条指令总是读 CNTVCT。
 */
.global job_resume
job_loop:
    svc     #0
job_resume:
    mrs     x0, cntvct_el0
    bl      job_body
    b       job_loop

.section .bss
.align 3
.global k_entry_ts, k_eret_ts, k_release_at, k_spin_late, k_arm_abs, k_arm_rel, k_cval
k_entry_ts:   .quad 0
k_release_at: .quad 0
k_spin_late:  .quad 0
k_vars:                     /* 出口按偏移访问，顺序不能改 */
k_pad:        .quad 0       /* +0 */
k_arm_rel:    .quad 0       /* +8 */
k_arm_abs:    .quad 0       /* +16 */
k_eret_ts:    .quad 0       /* +24 */
k_cval:       .quad 0       /* +32 */
