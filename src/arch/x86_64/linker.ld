ENTRY(_start)
SECTIONS
{
    . = 16M;                    /* identity-mapped; below it: 4..16 MiB for program images */
    kernel_start = .;
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.multiboot2)
        *(.text*)
    }
    .rodata BLOCK(4K) : ALIGN(4K) { *(.rodata*) }
    .data BLOCK(4K) : ALIGN(4K) { *(.data*) }
    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss*)
    }
    kernel_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
