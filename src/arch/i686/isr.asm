; Interrupt stubs, GDT/IDT loading and cooperative context switch
section .text

global gdt_flush
gdt_flush:
    mov eax, [esp+4]
    lgdt [eax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov gs, ax
    mov ax, 0x28
    mov fs, ax
    jmp 0x08:.flush
.flush:
    ret

global fs_reload
fs_reload:
    mov ax, 0x28
    mov fs, ax
    ret

global idt_flush
idt_flush:
    mov eax, [esp+4]
    lidt [eax]
    ret

%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
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

extern isr_handler
isr_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov gs, ax
    cld
    push esp
    call isr_handler
    add esp, 4
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

; void switch_context(uint32_t *old_esp, uint32_t new_esp, uint32_t new_cr3)
global switch_context
switch_context:
    mov eax, [esp+4]
    mov edx, [esp+8]
    mov ecx, [esp+12]
    push ebp
    push ebx
    push esi
    push edi
    pushfd
    mov [eax], esp
    mov eax, cr3
    cmp eax, ecx
    je .same
    mov cr3, ecx
.same:
    mov esp, edx
    popfd
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; Call a stdcall/cdecl function pointer with a fresh argument block (for PE callbacks)
; uint32_t call_wndproc(void *fn, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
global call_wndproc
call_wndproc:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    push dword [ebp+24]
    push dword [ebp+20]
    push dword [ebp+16]
    push dword [ebp+12]
    call [ebp+8]
    ; stdcall callee pops args; restore esp regardless
    lea esp, [ebp-12]
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; void call_entry(void *fn, uint32_t stack)  -- call PE entry point
global call_pe_entry
call_pe_entry:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    call [ebp+8]
    lea esp, [ebp-12]
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; tls callback / DllMain style: (hinst, reason, reserved)
global call_tls_cb
call_tls_cb:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    push dword [ebp+20]
    push dword [ebp+16]
    push dword [ebp+12]
    call [ebp+8]
    lea esp, [ebp-12]
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
