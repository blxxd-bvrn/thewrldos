; Application processor start-up (smp.c): copied to physical TRAMP and entered by the
; startup IPI in 16-bit real mode; to protected mode on the kernel's page directory,
; a stack by index, and the C entry.
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
    jmp dword 0x08:A(pm32)

bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov eax, [A(p_cr4)]
    mov cr4, eax
    mov eax, [A(p_cr3)]                  ; the kernel's page directory (paging is on after boot)
    test eax, eax
    jz .nopg
    mov cr3, eax
.nopg:
    mov eax, [A(p_cr0)]
    mov cr0, eax
    mov eax, 1
    lock xadd [A(p_count)], eax
    mov esp, [A(p_stacks)]
    lea ecx, [eax + 1]
    imul ecx, [A(p_stacksz)]
    add esp, ecx
    and esp, -16
    push eax
    push eax
    push eax
    push eax                             ; argument, stack kept 16-byte aligned
    mov eax, [A(p_entry)]
    call eax
.hang:
    cli
    hlt
    jmp .hang

align 8
tgdt:
    dq 0
    dq 0x00CF9A000000FFFF                ; 0x08 code
    dq 0x00CF92000000FFFF                ; 0x10 data
    dq 0
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
