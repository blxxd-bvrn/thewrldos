; THE WRLD OS (x64): entry and exit for Linux programs running in ring 3.
; A system call builds the same frame an interrupt from ring 3 does (regs_t),
; so both leave through iretq after lx_return_check (preemption, exit).
bits 64
section .text

extern lx_syscall_c
extern lx_return_check
extern lx_kstack_top
extern lx_user_rsp_tmp

; SYSCALL: RCX = user RIP, R11 = user RFLAGS, interrupts off (FMASK)
global syscall_entry
syscall_entry:
    mov [rel lx_user_rsp_tmp], rsp
    mov rsp, [rel lx_kstack_top]
    push qword 0x23                     ; ss
    push qword [rel lx_user_rsp_tmp]    ; rsp
    push r11                            ; rflags
    push qword 0x2B                     ; cs
    push rcx                            ; rip
    push qword 0                        ; err_code
    push qword 0x100                    ; int_no: marks a system call
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rbp, rsp                        ; regs_t *
    sub rsp, 512
    and rsp, -16
    fxsave [rsp]
    sti
    mov rdi, rbp
    call lx_syscall_c
    mov rdi, rbp
    call lx_return_check
    cli
    fxrstor [rsp]
    mov rsp, rbp
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    iretq

; void enter_user(regs_t *r): first entry into ring 3 with a clean FPU state
global enter_user
enter_user:
    cli
    fninit
    push qword 0x1F80
    ldmxcsr [rsp]
    add rsp, 8
    mov rsp, rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    iretq

; void gdt_load64(void *ptr, uint16_t tss_sel)
global gdt_load64
gdt_load64:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    push qword 0x08
    lea rax, [rel .reload]
    push rax
    retfq
.reload:
    ltr si
    ret
