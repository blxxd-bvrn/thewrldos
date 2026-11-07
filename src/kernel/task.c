/* Cooperative multitasking kernel scheduler */
#include "kernel.h"

extern void switch_context(uint32_t *old_esp, uint32_t new_esp, uint32_t cr3);
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
    task0.cr3 = (uint32_t)paging_kernel_dir();
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
    task_t *t = kzalloc(sizeof(task_t));
    if (!stack_size) stack_size = 64 * 1024;
    t->stack = kmalloc(stack_size);
    t->stack_size = stack_size;
    t->pid = next_pid++;
    strlcpy(t->name, name, sizeof t->name);
    t->entry = entry; t->arg = arg;
    t->cr3 = (uint32_t)paging_kernel_dir();
    t->started = timer_ticks();
    t->kind = 3;
    uint32_t *sp = (uint32_t *)(t->stack + stack_size);
    *--sp = 0;                          /* fake return for trampoline */
    *--sp = (uint32_t)task_trampoline;  /* ret */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; /* ebp ebx esi edi */
    *--sp = 0x002;                      /* eflags (IF set in trampoline) */
    t->esp = (uint32_t)sp;
    t->state = TASK_READY;
    /* append */
    task_t *p = tasks; while (p->next) p = p->next; p->next = t;
    klog("task: created pid %d '%s'", t->pid, t->name);
    return t;
}
void task_set_cr3(task_t *t, uint32_t cr3) { t->cr3 = cr3; }

static void reap(void) {
    while (zombies) {
        task_t *z = zombies; zombies = z->next;
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
    switch_context(&prev->esp, t->esp, t->cr3);
    reap();
}

void task_yield(void) {
    uint32_t f = irq_save();
    schedule();
    irq_restore(f);
}
void task_sleep(uint32_t ms) {
    if (current == &task0) {
        /* the main (GUI) task is never descheduled: let the other tasks run while it waits */
        uint32_t end = timer_ticks() + ms;
        while ((int32_t)(end - timer_ticks()) > 0) { task_yield(); hlt(); }
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
    uint32_t dummy;
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
