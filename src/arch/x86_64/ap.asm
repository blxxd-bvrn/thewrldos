; Application processor start-up (smp.c): copied to physical TRAMP and entered by the
; startup IPI in 16-bit real mode. Each processor goes to protected mode, then long mode
; on the kernel's page tables, takes a stack by its index and calls the C entry.
TRAMP equ 0x8000
%define A(x) (TRAMP + (x) - ap_tramp_start)

section .rodata
global ap_tramp_start, ap_tramp_end, ap_tramp_params

bits 16
ap_tramp_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    o32 lgdt [A(tgdt_ptr)]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword 0x18:A(pm32)

bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, [A(p_cr4)]
    mov cr4, eax
    mov eax, [A(p_cr3)]
    mov cr3, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, [A(p_efer)]
    wrmsr
    mov eax, [A(p_cr0)]
    mov cr0, eax
    jmp 0x08:A(lm64)

bits 64
lm64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    mov eax, 1
    lock xadd [A(p_count)], eax          ; eax = this processor's index
    mov rsp, [A(p_stacks)]
    mov edx, [A(p_stacksz)]
    lea ecx, [rax + 1]
    imul rcx, rdx
    add rsp, rcx
    and rsp, -16
    mov edi, eax
    mov rax, [A(p_entry)]
    call rax
.hang:
    cli
    hlt
    jmp .hang

align 8
tgdt:
    dq 0
    dq 0x00AF9A000000FFFF                ; 0x08 64-bit code
    dq 0x00CF92000000FFFF                ; 0x10 data
    dq 0x00CF9A000000FFFF                ; 0x18 32-bit code
tgdt_ptr:
    dw 31
    dd A(tgdt)
align 8
ap_tramp_params:
p_cr3:    dd 0
p_cr4:    dd 0
p_cr0:    dd 0
p_efer:   dd 0
p_stacks: dq 0
p_stacksz: dd 0
p_count:  dd 0
p_entry:  dq 0
ap_tramp_end:
