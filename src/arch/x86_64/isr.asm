; THE WRLD OS (x64): interrupt stubs, IDT loading and the cooperative context switch
bits 64
section .text

; FPU, SSE and AVX state around kernel code: XSAVE (x87, SSE and AVX, 832 bytes) when the
; kernel turned AVX on at boot (cpu_enable_avx), FXSAVE (512 bytes) otherwise. The XSAVE
; header is cleared first: XRSTOR faults on stale bytes there. rax and rdx are already saved.
extern fpu_xsave
%macro FPU_SAVE 0
    sub rsp, 1088
    and rsp, -64
    cmp byte [rel fpu_xsave], 0
    je %%fx
    xor eax, eax
    mov [rsp + 512], rax
    mov [rsp + 520], rax
    mov [rsp + 528], rax
    mov [rsp + 536], rax
    mov [rsp + 544], rax
    mov [rsp + 552], rax
    mov [rsp + 560], rax
    mov [rsp + 568], rax
    mov eax, 7
    xor edx, edx
    xsave [rsp]
    jmp %%done
%%fx:
    fxsave [rsp]
%%done:
%endmacro
%macro FPU_RESTORE 0
    cmp byte [rel fpu_xsave], 0
    je %%fx
    mov eax, 7
    xor edx, edx
    xrstor [rsp]
    jmp %%done
%%fx:
    fxrstor [rsp]
%%done:
%endmacro

global idt_flush
idt_flush:
    lidt [rdi]
    ret

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro
%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
%assign i 20
%rep 12
ISR_NOERR i
%assign i i+1
%endrep
%assign i 32
%rep 16
ISR_NOERR i
%assign i i+1
%endrep
ISR_NOERR 128

; Handlers are C compiled with SSE, so the interrupted code's SSE/x87 state is
; saved around them (FXSAVE needs a 16-byte aligned 512-byte area).
extern isr_handler
extern lx_return_check
isr_common:
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
    mov rbp, rsp            ; regs_t *
    FPU_SAVE
    mov rdi, rbp
    call isr_handler
    test qword [rbp + 18*8], 3          ; interrupted ring 3 (a Linux program)?
    jz .to_kernel
    mov rdi, rbp
    call lx_return_check                ; may preempt it or end it
.to_kernel:
    FPU_RESTORE
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
    add rsp, 16             ; int_no, err_code
    iretq

; void switch_context(uintptr_t *old_rsp, uintptr_t new_rsp, uintptr_t new_cr3)
; Only callee-saved registers matter (it is a function call); SSE registers are
; all caller-saved, and MXCSR / the x87 control word are left as they are.
global switch_context
switch_context:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    pushfq
    mov [rdi], rsp
    mov rax, cr3
    cmp rax, rdx
    je .same
    mov cr3, rdx
.same:
    mov rsp, rsi
    popfq
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; Soft restart of shellmanager.exe: the watchdog redirects the timer interrupt
; here.  Abandon the stuck call stack, switch to a private recovery stack and
; enter a fresh shell loop.  Only the shell task is disturbed; the machine and
; the other tasks keep running.
section .bss
align 16
shell_rec_stack:
    resb 131072
shell_rec_stack_top:
section .text
global shell_restart_trampoline
extern shell_recover
shell_restart_trampoline:
    mov rsp, shell_rec_stack_top
    xor rbp, rbp
    call shell_recover
    hlt
    jmp $
