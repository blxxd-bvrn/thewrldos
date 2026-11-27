/* The other processors as compute workers (src/kernel/smp.c). */
#ifndef SMP_H
#define SMP_H
#include "types.h"
#define SMP_MAX 16                       /* workers at most, the boot processor included */
extern int smp_workers_n;                /* workers smp_run uses (1: just this processor) */
void smp_init(void);
/* Run fn(arg, worker, nworkers) on every worker at once (worker 0 is the caller) and
 * wait for all. fn on the other processors must not call into the kernel: no
 * allocation, no logging, no locks, no yielding; it computes on memory it was given. */
void smp_run(void (*fn)(void *arg, int worker, int nworkers), void *arg);
#endif
