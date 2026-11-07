; THE WRLD OS - multiboot entry point
MBALIGN  equ 1 << 0
MEMINFO  equ 1 << 1
VIDEO    equ 1 << 2
FLAGS    equ MBALIGN | MEMINFO | VIDEO
MAGIC    equ 0x1BADB002
CHECKSUM equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    dd 0, 0, 0, 0, 0        ; address fields (unused, ELF kernel)
    dd 0                    ; linear framebuffer
    dd 1024                 ; width
    dd 768                  ; height
    dd 32                   ; depth

section .bss
align 16
global boot_stack_top
boot_stack_bottom:
    resb 65536
boot_stack_top:

section .text
global _start
extern kmain
_start:
    mov esp, boot_stack_top
    xor ebp, ebp
    push ebx                ; multiboot info
    push eax                ; multiboot magic
    call kmain
.hang:
    cli
    hlt
    jmp .hang
