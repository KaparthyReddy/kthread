#ifndef KTHREAD_H
#define KTHREAD_H

// ================================================================
//  kthread — Preemptive Userspace Thread Library
//
//  A from-scratch POSIX-style threading library built entirely in
//  userspace. Implements:
//    • Context switching via POSIX ucontext
//    • Preemption via SIGALRM (timer interrupt, like a real OS)
//    • Multi-Level Feedback Queue (MLFQ) scheduler
//    • Mutexes with priority-inheritance-style blocking
//    • Condition variables (wait / signal / broadcast)
//    • Semaphores
//    • Join (parent waits for child to finish)
// ================================================================

#include <stddef.h>

// ── Thread identifier ───────────────────────────────────────────
typedef int kthread_tid_t;

// ── Scheduling policies ──────────────────────────────────────────
typedef enum {
    SCHED_MLFQ  = 0,   // Multi-Level Feedback Queue (default)
    SCHED_RR    = 1,   // Round-Robin (fixed quantum)
    SCHED_PRIO  = 2,   // Static Priority (higher number = higher prio)
} kthread_sched_t;

// ── Thread attributes ────────────────────────────────────────────
typedef struct {
    size_t          stack_size;   // default: 512 KB
    int             priority;     // 0 (lowest) – 9 (highest)
    kthread_sched_t policy;
} kthread_attr_t;

// ── Mutex ────────────────────────────────────────────────────────
typedef struct kthread_mutex kthread_mutex_t;

// ── Condition variable ───────────────────────────────────────────
typedef struct kthread_cond kthread_cond_t;

// ── Semaphore ────────────────────────────────────────────────────
typedef struct kthread_sem kthread_sem_t;

// ── Library lifecycle ────────────────────────────────────────────
void kthread_init(kthread_sched_t policy, int quantum_ms);
void kthread_shutdown(void);

// ── Thread lifecycle ─────────────────────────────────────────────
kthread_tid_t kthread_create(void (*fn)(void *), void *arg,
                             const kthread_attr_t *attr);
void          kthread_exit(void *retval);
int           kthread_join(kthread_tid_t tid, void **retval);
kthread_tid_t kthread_self(void);
void          kthread_yield(void);
void          kthread_sleep(int ms);

// ── Default attr helper ──────────────────────────────────────────
kthread_attr_t kthread_attr_default(void);

// ── Mutex ────────────────────────────────────────────────────────
kthread_mutex_t *kthread_mutex_create(void);
void             kthread_mutex_destroy(kthread_mutex_t *m);
void             kthread_mutex_lock(kthread_mutex_t *m);
void             kthread_mutex_unlock(kthread_mutex_t *m);
int              kthread_mutex_trylock(kthread_mutex_t *m);

// ── Condition variable ───────────────────────────────────────────
kthread_cond_t *kthread_cond_create(void);
void            kthread_cond_destroy(kthread_cond_t *c);
void            kthread_cond_wait(kthread_cond_t *c, kthread_mutex_t *m);
void            kthread_cond_signal(kthread_cond_t *c);
void            kthread_cond_broadcast(kthread_cond_t *c);

// ── Semaphore ────────────────────────────────────────────────────
kthread_sem_t *kthread_sem_create(int initial);
void           kthread_sem_destroy(kthread_sem_t *s);
void           kthread_sem_wait(kthread_sem_t *s);
void           kthread_sem_post(kthread_sem_t *s);

// ── Scheduler stats ──────────────────────────────────────────────
typedef struct {
    long context_switches;
    long preemptions;
    long voluntary_yields;
    long total_threads_created;
} kthread_stats_t;

kthread_stats_t kthread_get_stats(void);
void            kthread_print_stats(void);

#endif // KTHREAD_H
