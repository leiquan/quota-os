/*
 * EL0 paint app. Linked at VA 0. No libc.
 *
 * x0 role, x1 limit, x2 other-app PA, x3 framebuffer PA,
 * x4 kernel VA, x5 viewport pack (x | y<<16 | w<<32 | h<<48).
 * Own pages: code 0x0, stack 0x1000 (SP = 0x2000), draw 0x2000.
 * Word at 0x1000 is a harness command for role 14.
 *
 * SVC x8: 0 yield, 1 exit, 2 submit.
 * Submit x0 buf, x1 bytes, x2 x, x3 y, x4 w, x5 h.
 */
.section .text
.global _start

_start:
    mov     x19, x0
    mov     x20, x1
    mov     x21, x2
    mov     x22, x3
    mov     x23, x4
    mov     x24, x5
    mov     x25, xzr
    mov     x26, #0x1000
    cmp     x19, #9
    b.eq    do_exec
    cmp     x19, #14
    b.eq    scripted
    cmp     x19, #7
    b.eq    probes

run:
    bl      submit_frame
    add     x25, x25, #1
    cmp     x19, #6
    b.eq    leave
    cmp     x25, x20
    b.lo    yld
    cmp     x19, #0
    b.eq    yld
    cmp     x19, #2
    b.eq    do_kern
    cmp     x19, #3
    b.eq    do_dev
    cmp     x19, #4
    b.eq    do_other
    cmp     x19, #5
    b.eq    do_fb
    cmp     x19, #8
    b.eq    do_unmap
    b       leave

yld:
    mov     x8, #0
    svc     #0
    b       run

scripted:
    ldr     w6, [x26]
    cmp     w6, #2
    b.eq    leave
    cmp     w6, #3
    b.eq    burst
    cmp     w6, #1
    b.ne    syld
    str     wzr, [x26]
    bl      submit_frame
    b       syld
burst:
    str     wzr, [x26]
    bl      submit_frame
    bl      submit_frame
    bl      submit_frame
syld:
    mov     x8, #0
    svc     #0
    b       scripted

probes:
    mov     x0, #0x2000
    mov     x1, #8192
    and     x2, x24, #0xffff
    lsr     x3, x24, #16
    and     x3, x3, #0xffff
    mov     x4, #32
    mov     x5, #32
    mov     x8, #2
    svc     #0
    mov     x0, x23
    mov     x1, #4
    mov     x4, #1
    mov     x5, #1
    mov     x8, #2
    svc     #0
    mov     x0, x22
    mov     x1, #4
    mov     x8, #2
    svc     #0
    mov     x0, #0x2000
    mov     x1, #4096
    mov     x2, xzr
    mov     x3, xzr
    mov     x4, #32
    mov     x5, #32
    mov     x8, #2
    svc     #0
    b       run

/* Fill the draw page and submit it at the packed viewport. */
submit_frame:
    mov     x27, x30
    and     w11, w25, #0xf0
    add     w11, w11, #0x3f
    and     w12, w24, #0xffff
    cmp     w12, #200
    b.ge    1f
    lsl     w11, w11, #8
1:
    mov     x10, #0x2000
    mov     x9, #1024
2:
    str     w11, [x10], #4
    subs    x9, x9, #1
    b.ne    2b
    mov     x0, #0x2000
    mov     x1, #4096
    and     x2, x24, #0xffff
    lsr     x3, x24, #16
    and     x3, x3, #0xffff
    lsr     x4, x24, #32
    and     x4, x4, #0xffff
    lsr     x5, x24, #48
    mov     x8, #2
    svc     #0
    mov     x30, x27
    ret

leave:
    mov     x0, x25
    mov     x8, #1
    svc     #0
    b       leave

do_kern:
    ldr     w0, [x23]
    b       do_kern

do_dev:
    movz    x0, #0x0900, lsl #16
    ldr     w1, [x0]
    b       do_dev

do_other:
    ldr     w0, [x21]
    b       do_other

do_fb:
    ldr     w0, [x22]
    b       do_fb

do_unmap:
    mov     x0, #0x3000
    ldr     w0, [x0]
    b       do_unmap

do_exec:
    br      x23
