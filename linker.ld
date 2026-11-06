ENTRY(_start)
SECTIONS
{
    . = 16M;                    /* above the 4..16 MiB window used for Win32 program images */
    kernel_start = .;
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
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
