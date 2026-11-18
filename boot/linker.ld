/*
 * Quota OS 链接脚本。
 * QEMU virt 把 -kernel 映像放在 RAM 起点之上 0x80000 处，
 * 即物理地址 0x40080000。入口必须落在这里。
 */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    tls PT_TLS FLAGS(4);
}

SECTIONS
{
    . = 0x40080000;
    __image_start = .;

    .text.boot : {
        KEEP(*(.text.boot))
    } :text

    .text : {
        *(.text .text.*)
    } :text

    .rodata : {
        *(.rodata .rodata.*)
    } :text

    .data : {
        *(.data .data.*)
        *(.data.rel.ro .data.rel.ro.*)
        *(.got .got.*)
    } :data

    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :data

    /*
     * AArch64 TP points 16 bytes before the TLS block. Those 16 bytes are
     * the TCB and are not part of PT_TLS, so they do not change symbol offsets.
     */
    . = ALIGN(16);
    . += 16;
    __tls_start = .;
    .tdata : { *(.tdata .tdata.*) } :data :tls
    .tbss : {
        *(.tbss .tbss.*)
        __tls_end = .;
    } :data :tls

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data
    __image_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note*)
    }
}
