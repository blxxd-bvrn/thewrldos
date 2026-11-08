/* Threads on THE WRLD OS: pthread_create/join, mutexes, condition variables,
 * thread-local storage, and preemption (a busy thread must not starve the rest). */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static long counter;
static int ready;
static __thread int my_id;
static volatile int stop_spinner;

static void *worker(void *arg) {
    my_id = (int)(long)arg;
    for (int i = 0; i < 100000; i++) { pthread_mutex_lock(&lock); counter++; pthread_mutex_unlock(&lock); }
    pthread_mutex_lock(&lock); ready++; pthread_cond_signal(&cond); pthread_mutex_unlock(&lock);
    return (void *)(long)(my_id * 10);
}
static void *spinner(void *arg) { (void)arg; while (!stop_spinner) {} return NULL; }   /* never yields: only preemption lets others run */

int main(void) {
    pthread_t sp, t[4];
    pthread_create(&sp, NULL, spinner, NULL);
    my_id = 99;
    for (long i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, (void *)(i + 1));
    pthread_mutex_lock(&lock);
    while (ready < 4) pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
    long sum = 0;
    for (int i = 0; i < 4; i++) { void *r; pthread_join(t[i], &r); sum += (long)r; }
    stop_spinner = 1;
    pthread_join(sp, NULL);
    printf("threads: counter = %ld (expect 400000), join results = %ld (expect 100), main TLS id = %d (expect 99)\n", counter, sum, my_id);
    return counter == 400000 && sum == 100 && my_id == 99 ? 0 : 1;
}
