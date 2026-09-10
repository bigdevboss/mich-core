ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 1M;

    _text_start = .;
    .text : ALIGN(4096) {
        *(.text*)
    } :text
    _text_end = .;

    . = ALIGN(4096);
    _rodata_start = .;
    .rodata : ALIGN(4096) {
        *(.rodata*)
    } :rodata
    _rodata_end = .;

    . = ALIGN(4096);
    .data : ALIGN(4096) {
        *(.data*)
    } :data

    .bss : ALIGN(4096) {
        *(COMMON)
        *(.bss*)
        _bss_end = .;
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
