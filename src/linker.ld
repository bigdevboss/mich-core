ENTRY(_start)

SECTIONS
{
    . = 1M;

    .multiboot2 : {
        *(.multiboot2_header)
    }

    .text : {
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(.bss)
        _bss_end = .;
    }
}
