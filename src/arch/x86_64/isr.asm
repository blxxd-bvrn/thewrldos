; THE WRLD OS (x64): interrupt stubs, IDT loading and the cooperative context switch
bits 64
section .text

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
    sub rsp, 512
    and rsp, -16
    fxsave [rsp]
    mov rdi, rbp
    call isr_handler
    test qword [rbp + 18*8], 3          ; interrupted ring 3 (a Linux program)?
    jz .to_kernel
    mov rdi, rbp
    call lx_return_check                ; may preempt it or end it
.to_kernel:
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
