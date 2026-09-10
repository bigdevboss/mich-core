ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x100000000;

    .text : ALIGN(4096) {
        *(.text*)
    } :text

    . = ALIGN(4096);
    .rodata : ALIGN(4096) {
        *(.rodata*)
    } :rodata

    . = ALIGN(4096);
    .data : ALIGN(4096) {
        *(.data*)
    } :data

    .bss : ALIGN(4096) {
        *(COMMON)
        *(.bss*)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
