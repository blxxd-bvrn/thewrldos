/* Cooperative multitasking kernel scheduler */
#include "kernel.h"

extern void switch_context(uintptr_t *old_esp, uintptr_t new_esp, uintptr_t cr3);
static task_t task0;
static task_t *current = &task0;
static task_t *tasks = &task0;
static int next_pid = 1;
static task_t *zombies;
void (*task_kill_hook)(task_t *t);
static struct { int pid, code; } exit_ring[32];
static int exit_ring_pos;
static void record_exit(task_t *t) { exit_ring[exit_ring_pos].pid = t->pid; exit_ring[exit_ring_pos].code = t->exit_code; exit_ring_pos = (exit_ring_pos + 1) % 32; }
int task_exit_code(int pid) { for (int i = 0; i < 32; i++) if (exit_ring[i].pid == pid) return exit_ring[i].code; return -1; }

void task_init(void) {
    task0.pid = 0;
    strcpy(task0.name, "System");
    task0.state = TASK_READY;
    task0.cr3 = paging_kernel_root();
    task0.kind = 0;
}
task_t *task_current(void) { return current; }
task_t *task_list(void) { return tasks; }
task_t *task_find(int pid) { for (task_t *t = tasks; t; t = t->next) if (t->pid == pid) return t; return NULL; }
int task_count(void) { int n = 0; for (task_t *t = tasks; t; t = t->next) n++; return n; }

static void task_trampoline(void) {
    sti();
    current->entry(current->arg);
    task_exit(0);
}

task_t *task_create(const char *name, void (*entry)(void *), void *arg, uint32_t stack_size) {
    if (!stack_size) stack_size = 64 * 1024;
    /* NULL when memory runs out (callers must check) */
    task_t *t = kzalloc(sizeof(task_t));
    if (t) t->stack = kmalloc(stack_size);
    if (!t || !t->stack) { klog("task: not enough memory to start '%s'", name); kfree(t); return NULL; }
    t->stack_size = stack_size;
    t->pid = next_pid++;
    strlcpy(t->name, name, sizeof t->name);
    t->entry = entry; t->arg = arg;
    t->cr3 = paging_kernel_root();
    t->started = timer_ticks();
    t->kind = 3;
    /* initial frame popped by switch_context (see src/arch/<arch>/isr.asm) */
    uintptr_t *sp = (uintptr_t *)(((uintptr_t)t->stack + stack_size) & ~(uintptr_t)15);
#ifdef __x86_64__
    *--sp = 0;                          /* alignment: rsp % 16 == 8 at the trampoline's entry */
    *--sp = (uintptr_t)task_trampoline; /* ret */
    for (int i = 0; i < 6; i++) *--sp = 0;   /* rbp rbx r12 r13 r14 r15 */
#else
    *--sp = 0;                          /* fake return for trampoline */
    *--sp = (uintptr_t)task_trampoline; /* ret */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; /* ebp ebx esi edi */
#endif
    *--sp = 0x002;                      /* eflags (IF set in trampoline) */
    t->esp = (uintptr_t)sp;
    t->state = TASK_READY;
    /* append */
    task_t *p = tasks; while (p->next) p = p->next; p->next = t;
    klog("task: created pid %d '%s'", t->pid, t->name);
    return t;
}
void task_set_cr3(task_t *t, uintptr_t cr3) { t->cr3 = cr3; }

#ifdef __x86_64__
extern void arch_set_kernel_stack(uintptr_t top);
extern void arch_set_user_fs(uint64_t base);
extern void lx_task_switched(task_t *t);
extern void lx_task_reaped(task_t *t);
static void arch_switch_to(task_t *t) {
    if (t->stack) arch_set_kernel_stack(((uintptr_t)t->stack + t->stack_size) & ~(uintptr_t)15);
    if (t->lx) lx_task_switched(t);
}
#else
static void arch_switch_to(task_t *t) { (void)t; }
#endif
static void reap(void) {
    while (zombies) {
        task_t *z = zombies; zombies = z->next;
#ifdef __x86_64__
        if (z->lx) lx_task_reaped(z);
#endif
        kfree(z->stack);
        kfree(z);
    }
}
static void unlink_task(task_t *t) {
    for (task_t **pp = &tasks; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; break; }
}

static void schedule(void) {
    task_t *start = current, *t = current;
    uint32_t now = timer_ticks();
    for (;;) {
        t = t->next ? t->next : tasks;
        if (t->state == TASK_SLEEPING && (int32_t)(now - t->wake_at) >= 0) t->state = TASK_READY;
        if (t->state == TASK_READY) break;
        if (t == start) {
            if (start->state == TASK_READY) break;
            /* nothing runnable: idle until next interrupt */
            extern volatile bool cpu_idle_flag;
            cpu_idle_flag = true; sti(); hlt(); cpu_idle_flag = false;
            now = timer_ticks();
        }
    }
    if (t == current) return;
    task_t *prev = current;
    current = t;
    gdt_set_fs_base(t->teb);
    arch_switch_to(t);
    switch_context(&prev->esp, t->esp, t->cr3);
    reap();
}

volatile uint32_t gui_heartbeat;             /* last time the main (GUI) task had the CPU */
task_t *gui_task;                            /* shellmanager.exe task, once it starts */
void task_set_gui_task(task_t *t) { gui_task = t; }
void task_yield(void) {
    if (current == &task0 || (gui_task && current == gui_task)) gui_heartbeat = timer_ticks();
    uint32_t f = irq_save();
    schedule();
    irq_restore(f);
}
void task_sleep(uint32_t ms) {
    if (current == &task0) {
        /* the main (GUI) task is never descheduled: let the other tasks run while it waits */
        uint32_t end = timer_ticks() + ms;
        while ((int32_t)(end - timer_ticks()) > 0) { task_yield(); idle_hlt(); }
        return;
    }
    uint32_t f = irq_save();
    current->wake_at = timer_ticks() + ms;
    current->state = TASK_SLEEPING;
    schedule();
    irq_restore(f);
}
void task_exit(int code) {
    cli();
    task_t *t = current;
    if (t == &task0) panic("System task exited");
    t->exit_code = code;
    klog("task: pid %d '%s' exited (%d)", t->pid, t->name, code);
    if (task_kill_hook) task_kill_hook(t);
    record_exit(t);
    t->state = TASK_DEAD;
    unlink_task(t);
    t->next = NULL;
    /* we can't free our own stack while on it: queue for reaping */
    task_t *nxt = tasks;
    current = nxt;
    while (current->state != TASK_READY && current->next) current = current->next;
    if (current->state != TASK_READY) current = &task0;
    t->next = zombies; zombies = t;
    gdt_set_fs_base(current->teb);
    arch_switch_to(current);
    uintptr_t dummy;
    current->state = current->state == TASK_SLEEPING ? TASK_READY : current->state;
    switch_context(&dummy, current->esp, current->cr3);
    for (;;) hlt();
}
bool task_kill(int pid) {
    task_t *t = task_find(pid);
    if (!t || t == &task0) return false;
    if (t == current) task_exit(-1);
    uint32_t f = irq_save();
    klog("task: killed pid %d '%s'", t->pid, t->name);
    if (task_kill_hook) task_kill_hook(t);
    t->exit_code = -1; record_exit(t);
    t->state = TASK_DEAD;
    unlink_task(t);
    t->next = zombies; zombies = t;
    irq_restore(f);
    return true;
}

/* For debugging hung programs (Ctrl+Alt+Shift+F12): log each task's state and the values on
 * its stack that look like code addresses, read through the task's own address space. Match
 * them against the kernel's symbols and the "loaded ... at" lines of program modules. */
void task_dump_stacks(void) {
    extern char kernel_end[];
    static uintptr_t buf[1024];
    klog("task dump at %u ms:", timer_ticks());
    for (task_t *t = tasks; t; t = t->next) {
        if (t == current) { klog("  pid %d '%s' (running: the dumper)", t->pid, t->name); continue; }
        uintptr_t sp = t->esp, end = t->stack && sp >= (uintptr_t)t->stack && sp < (uintptr_t)t->stack + t->stack_size ? (uintptr_t)t->stack + t->stack_size : (sp | 0xFFF) + 1;
        int n = (int)MIN((end - sp) / sizeof(uintptr_t), ARRAY_LEN(buf));
        uintptr_t f = irq_save(), old;
        __asm__ volatile("mov %%cr3, %0" : "=r"(old));
        if (t->cr3 && t->cr3 != old) __asm__ volatile("mov %0, %%cr3" :: "r"(t->cr3) : "memory");
        memcpy(buf, (void *)sp, n * sizeof(uintptr_t));
        if (t->cr3 && t->cr3 != old) __asm__ volatile("mov %0, %%cr3" :: "r"(old) : "memory");
        irq_restore(f);
        klog("  pid %d '%s' state %d sp %llx:", t->pid, t->name, t->state, (unsigned long long)sp);
        char line[200]; int k = 0, shown = 0;
        for (int i = 0; i < n && shown < 96; i++) {
            uintptr_t v = buf[i];
            if (!(v >= 0x100000 && v < 0x80000000u && (v < 0x1000000 || v < (uintptr_t)kernel_end))) continue;
            k += snprintf(line + k, sizeof line - k, " %llx", (unsigned long long)v); shown++;
            if (shown % 12 == 0) { klog("   %s", line); k = 0; line[0] = 0; }
        }
        if (k) klog("   %s", line);
    }
}
