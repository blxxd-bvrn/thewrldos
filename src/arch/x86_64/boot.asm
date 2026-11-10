; THE WRLD OS (x64) - multiboot entry point.
; GRUB starts us in 32-bit protected mode with paging off. Build page tables
; that identity-map the first 4 GiB with 2 MiB pages, switch to long mode,
; enable the FPU and SSE, then call kmain(magic, multiboot_info).
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
    dd 0, 0, 0, 0, 0
    dd 0                    ; linear framebuffer
    dd 1024
    dd 768
    dd 32

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
align 4096
global boot_pml4, boot_pdpt, boot_pd
boot_pml4: resb 4096
boot_pdpt: resb 4096
boot_pd:   resb 4096 * 4    ; four page directories = 4 GiB
align 16
global boot_stack_top
boot_stack_bottom:
    resb 65536
boot_stack_top:

section .data
align 8
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF   ; 0x08: 64-bit code
    dq 0x00CF92000000FFFF   ; 0x10: data
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64
boot_magic: dd 0
boot_info:  dd 0

section .text
bits 32
global _start
_start:
    cli
    mov esp, boot_stack_top
    mov [boot_magic], eax
    mov [boot_info], ebx
    ; long mode available? (CPUID 0x80000001 EDX bit 29)
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long
    ; page tables: PML4[0] -> PDPT, PDPT[0..3] -> PD0..PD3, each PD = 512 x 2 MiB
    mov edi, boot_pml4
    mov eax, boot_pdpt
    or eax, 3
    mov [edi], eax
    mov edi, boot_pdpt
    mov eax, boot_pd
    or eax, 3
    mov ecx, 4
.pdpt:
    mov [edi], eax
    add eax, 4096
    add edi, 8
    loop .pdpt
    mov edi, boot_pd
    xor eax, eax
    xor edx, edx            ; high dword of the physical address
    mov ecx, 2048
.pd:
    mov ebx, eax
    or ebx, 0x83            ; present, writable, 2 MiB page
    mov [edi], ebx
    mov [edi + 4], edx
    add eax, 0x200000
    adc edx, 0
    add edi, 8
    loop .pd
    ; PAE, OSFXSR, OSXMMEXCPT
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov cr4, eax
    mov eax, boot_pml4
    mov cr3, eax
    ; EFER.LME
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    ; paging on, FPU: EM off, MP and NE on
    mov eax, cr0
    and eax, ~(1 << 2)
    or eax, (1 << 31) | (1 << 1) | (1 << 5)
    mov cr0, eax
    lgdt [gdt64_ptr]
    jmp 0x08:long_mode
.no_long:
    ; tell the user on the serial port (the screen is in graphics mode)
    mov esi, no_long_msg
.msg:
    lodsb
    test al, al
    jz .halt
    mov dx, 0x3F8
    out dx, al
    jmp .msg
.halt:
    hlt
    jmp .halt
no_long_msg: db "THE WRLD OS x64 needs a 64-bit (x86-64) processor. Boot the 32-bit edition instead.", 13, 10, 0

bits 64
extern kmain
long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    mov rsp, boot_stack_top
    fninit
    xor rbp, rbp
    mov edi, [boot_magic]
    mov esi, [boot_info]
    call kmain
.hang:
    cli
    hlt
    jmp .hang
