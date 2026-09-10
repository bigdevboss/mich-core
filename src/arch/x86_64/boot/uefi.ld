OUTPUT_FORMAT("pei-x86-64")
ENTRY(efi_main)

SECTIONS
{
    .text 0x3001000 : ALIGN(4096) {
        *(.text .text.*)
    }

    .data ALIGN(4096) : ALIGN(4096) {
        *(.data .data.*)
        *(.bss .bss.*)
        *(COMMON)
    }

    .rdata ALIGN(4096) : ALIGN(4096) {
        *(.rdata .rdata.*)
        *(.rodata .rodata.*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.debug*)
        *(.idata*)
        *(.reloc)
    }
}
