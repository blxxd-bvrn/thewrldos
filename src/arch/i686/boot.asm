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

; Multiboot 2 header, used by GRUB on UEFI (and on BIOS just as happily).
section .multiboot2
align 8
mb2_start:
    dd 0xE85250D6                                   ; magic
    dd 0                                            ; architecture: i386
    dd mb2_end - mb2_start                          ; header length
    dd -(0xE85250D6 + 0 + (mb2_end - mb2_start))    ; checksum
    ; framebuffer request: 1024x768x32
    dw 5, 0
    dd 20
    dd 1024, 768, 32
    ; end tag
    dw 0, 0
    dd 8
mb2_end:

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
