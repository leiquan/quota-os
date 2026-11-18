ENTRY(_start)
SECTIONS
{
    . = 0x0;
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) *(.ARM.attributes) }
}
