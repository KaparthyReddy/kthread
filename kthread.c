// ================================================================
//  kthread.c — Preemptive Userspace Thread Library
//
//  Context switching  : POSIX ucontext_t (makecontext/swapcontext)
//  Preemption         : SIGALRM → signal handler → scheduler
//  Scheduler          : Multi-Level Feedback Queue (MLFQ)
//                       3 priority queues, quantum doubles per level
//                       Periodic priority boost to prevent starvation
// ================================================================

#define _GNU_SOURCE
#include "kthread.h"

#include <ucontext.h>
#include <signal.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <unistd.h>

// ── Configuration ─────────────────────────────────────────────
#define MAX_THREADS       256
#define DEFAULT_STACK_KB  512
#define MLFQ_LEVELS       3
#define BOOST_INTERVAL_MS 500   // priority boost period (starvation prevention)

// ── Thread states ──────────────────────────────────────────────
typedef enum {
    TS_UNUSED   = 0,
    TS_READY    = 1,
    TS_RUNNING  = 2,
    TS_BLOCKED  = 3,   // waiting on mutex / cond / sem
    TS_SLEEPING = 4,   // kthread_sleep()
    TS_ZOMBIE   = 5,   // finished, waiting to be joined
} thread_state_t;

// ── Thread Control Block ───────────────────────────────────────
typedef struct tcb {
    kthread_tid_t   tid;
    thread_state_t  state;
    ucontext_t      ctx;
    char           *stack;
    size_t          stack_size;

    void (*fn)(void *);
    void           *arg;
    void           *retval;

    int             priority;       // static priority (SCHED_PRIO)
    int             mlfq_level;     // current MLFQ queue (0=top)
    int             quanta_used;    // quanta consumed at this level
    int             quanta_limit;   // before demotion

    long long       sleep_until_ms; // wake time for TS_SLEEPING

    kthread_tid_t   join_waiting;   // thread waiting to join us (-1 if none)

    struct tcb     *next;           // intrusive linked list (for queues)
} tcb_t;

// ── Simple intrusive queue ─────────────────────────────────────
typedef struct { tcb_t *head, *tail; } tqueue_t;

static void tq_push(tqueue_t *q, tcb_t *t) {
    t->next = NULL;
    if (!q->tail) { q->head = q->tail = t; }
    else          { q->tail->next = t; q->tail = t; }
}
static tcb_t *tq_pop(tqueue_t *q) {
    if (!q->head) return NULL;
    tcb_t *t = q->head;
    q->head = t->next;
    if (!q->head) q->tail = NULL;
    t->next = NULL;
    return t;
}
static void tq_remove(tqueue_t *q, tcb_t *target) {
    tcb_t *prev = NULL, *cur = q->head;
    while (cur) {
        if (cur == target) {
            if (prev) prev->next = cur->next; else q->head = cur->next;
            if (cur == q->tail) q->tail = prev;
            cur->next = NULL;
            return;
        }
        prev = cur; cur = cur->next;
    }
}

// ── Global scheduler state ─────────────────────────────────────
static tcb_t          threads[MAX_THREADS];
static tcb_t         *current      = NULL;
static ucontext_t     scheduler_ctx;
static char           scheduler_stack[64 * 1024];

static tqueue_t       mlfq[MLFQ_LEVELS]; // ready queues
static tqueue_t       sleep_queue;       // sleeping threads

static kthread_sched_t sched_policy;
static int             base_quantum_ms;  // quantum for MLFQ level 0
static long long       last_boost_ms;

static kthread_stats_t stats;

// ── Preemption guard (re-entrant signal protection) ───────────
static volatile sig_atomic_t preempt_disabled = 0;

static void disable_preempt(void) { preempt_disabled = 1; }
static void enable_preempt(void)  { preempt_disabled = 0; }

// ── Time helper ────────────────────────────────────────────────
static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// ── MLFQ quantum for a given level ────────────────────────────
static int level_quantum(int level) {
    int q = base_quantum_ms;
    for (int i = 0; i < level; i++) q *= 2;
    return q;
}

// ── Enqueue a thread as READY ─────────────────────────────────
static void make_ready(tcb_t *t) {
    t->state = TS_READY;
    if (sched_policy == SCHED_PRIO) {
        // Insert in priority order (higher priority → front)
        int level = MLFQ_LEVELS - 1 - (t->priority * MLFQ_LEVELS / 10);
        if (level < 0) level = 0;
        if (level >= MLFQ_LEVELS) level = MLFQ_LEVELS - 1;
        tq_push(&mlfq[level], t);
    } else {
        tq_push(&mlfq[t->mlfq_level], t);
    }
}

// ── Wake sleeping threads whose timer has expired ──────────────
static void wake_sleepers(void) {
    long long now = now_ms();
    tcb_t *t = sleep_queue.head;
    while (t) {
        tcb_t *next = t->next;
        if (t->sleep_until_ms <= now) {
            tq_remove(&sleep_queue, t);
            make_ready(t);
        }
        t = next;
    }
}

// ── MLFQ priority boost (prevent starvation) ──────────────────
static void maybe_boost(void) {
    long long now = now_ms();
    if (now - last_boost_ms < BOOST_INTERVAL_MS) return;
    last_boost_ms = now;
    // Move all threads from lower queues to queue 0
    for (int lvl = 1; lvl < MLFQ_LEVELS; lvl++) {
        tcb_t *t;
        while ((t = tq_pop(&mlfq[lvl])) != NULL) {
            t->mlfq_level  = 0;
            t->quanta_used = 0;
            t->quanta_limit = level_quantum(0);
            tq_push(&mlfq[0], t);
        }
    }
}

// ── Pick next thread to run ────────────────────────────────────
static tcb_t *pick_next(void) {
    for (int lvl = 0; lvl < MLFQ_LEVELS; lvl++) {
        tcb_t *t = tq_pop(&mlfq[lvl]);
        if (t) return t;
    }
    return NULL;
}

// ── Thread wrapper: calls fn(arg), then kthread_exit ──────────
static void thread_entry(void) {
    // current is set before swapcontext into us
    void (*fn)(void *) = current->fn;
    void *arg          = current->arg;
    enable_preempt();
    fn(arg);
    kthread_exit(NULL);
}

// ── Scheduler: runs in its own context ────────────────────────
static void scheduler(void) {
    while (1) {
        disable_preempt();
        wake_sleepers();
        if (sched_policy == SCHED_MLFQ) maybe_boost();

        tcb_t *next = pick_next();
        if (!next) {
            // No runnable thread — spin-wait (idle)
            enable_preempt();
            usleep(500);
            continue;
        }

        next->state = TS_RUNNING;
        current = next;
        stats.context_switches++;

        // Arm the preemption timer
        struct itimerval itv = {
            .it_value    = { .tv_sec = 0,
                             .tv_usec = level_quantum(next->mlfq_level) * 1000 },
            .it_interval = { 0, 0 }
        };
        setitimer(ITIMER_REAL, &itv, NULL);

        enable_preempt();
        swapcontext(&scheduler_ctx, &next->ctx);
        // We return here when the thread yields, blocks, exits, or is preempted
        disable_preempt();

        // Disarm timer (thread returned voluntarily)
        struct itimerval off = {0};
        setitimer(ITIMER_REAL, &off, NULL);
    }
}

// ── SIGALRM handler: preempt current thread ───────────────────
static void sigalrm_handler(int sig) {
    (void)sig;
    if (preempt_disabled) return;
    if (!current) return;

    stats.preemptions++;

    // MLFQ demotion: used up quantum → move to next level
    if (sched_policy == SCHED_MLFQ) {
        current->quanta_used++;
        if (current->quanta_used >= current->quanta_limit &&
            current->mlfq_level < MLFQ_LEVELS - 1) {
            current->mlfq_level++;
            current->quanta_used  = 0;
            current->quanta_limit = level_quantum(current->mlfq_level);
        }
    }

    tcb_t *preempted = current;
    current = NULL;
    preempted->state = TS_READY;
    tq_push(&mlfq[preempted->mlfq_level], preempted);

    // Jump back to scheduler without saving preempted ctx
    // (swapcontext from scheduler will return here on resume)
    swapcontext(&preempted->ctx, &scheduler_ctx);
}

// ─────────────────────────────────────────────────────────────
//  PUBLIC API
// ─────────────────────────────────────────────────────────────

void kthread_init(kthread_sched_t policy, int quantum_ms) {
    memset(threads, 0, sizeof(threads));
    for (int i = 0; i < MLFQ_LEVELS; i++) mlfq[i] = (tqueue_t){0};
    sleep_queue = (tqueue_t){0};
    memset(&stats, 0, sizeof(stats));

    sched_policy    = policy;
    base_quantum_ms = quantum_ms > 0 ? quantum_ms : 10;
    last_boost_ms   = now_ms();

    // Thread 0 = main thread (already running)
    threads[0].tid         = 0;
    threads[0].state       = TS_RUNNING;
    threads[0].mlfq_level  = 0;
    threads[0].quanta_used = 0;
    threads[0].quanta_limit= level_quantum(0);
    threads[0].join_waiting= -1;
    current = &threads[0];
    stats.total_threads_created = 1;

    // Set up scheduler context
    getcontext(&scheduler_ctx);
    scheduler_ctx.uc_stack.ss_sp   = scheduler_stack;
    scheduler_ctx.uc_stack.ss_size = sizeof(scheduler_stack);
    scheduler_ctx.uc_link          = NULL;
    makecontext(&scheduler_ctx, scheduler, 0);

    // Install SIGALRM handler for preemption
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigalrm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
}

void kthread_shutdown(void) {
    struct itimerval off = {0};
    setitimer(ITIMER_REAL, &off, NULL);

    for (int i = 1; i < MAX_THREADS; i++) {
        if (threads[i].state != TS_UNUSED && threads[i].stack) {
            free(threads[i].stack);
            threads[i].stack = NULL;
        }
    }
}

kthread_attr_t kthread_attr_default(void) {
    return (kthread_attr_t){
        .stack_size = DEFAULT_STACK_KB * 1024,
        .priority   = 5,
        .policy     = SCHED_MLFQ,
    };
}

kthread_tid_t kthread_create(void (*fn)(void *), void *arg,
                             const kthread_attr_t *attr) {
    disable_preempt();

    // Find free TCB slot
    int tid = -1;
    for (int i = 1; i < MAX_THREADS; i++) {
        if (threads[i].state == TS_UNUSED) { tid = i; break; }
    }
    if (tid < 0) { enable_preempt(); return -1; }

    kthread_attr_t a = attr ? *attr : kthread_attr_default();
    size_t ssize = a.stack_size > 0 ? a.stack_size : DEFAULT_STACK_KB * 1024;

    tcb_t *t     = &threads[tid];
    t->tid       = tid;
    t->fn        = fn;
    t->arg       = arg;
    t->retval    = NULL;
    t->priority  = a.priority;
    t->mlfq_level  = 0;
    t->quanta_used = 0;
    t->quanta_limit= level_quantum(0);
    t->join_waiting= -1;
    t->stack_size  = ssize;
    t->stack       = (char *)malloc(ssize);

    getcontext(&t->ctx);
    t->ctx.uc_stack.ss_sp   = t->stack;
    t->ctx.uc_stack.ss_size = ssize;
    t->ctx.uc_link          = NULL;
    makecontext(&t->ctx, thread_entry, 0);

    stats.total_threads_created++;
    make_ready(t);
    enable_preempt();
    return tid;
}

void kthread_exit(void *retval) {
    disable_preempt();
    current->retval = retval;
    current->state  = TS_ZOMBIE;

    // Wake any thread waiting to join us
    if (current->join_waiting >= 0) {
        tcb_t *waiter = &threads[current->join_waiting];
        if (waiter->state == TS_BLOCKED)
            make_ready(waiter);
    }

    tcb_t *me = current;
    current = NULL;
    swapcontext(&me->ctx, &scheduler_ctx);
    __builtin_unreachable();
}

int kthread_join(kthread_tid_t tid, void **retval) {
    if (tid <= 0 || tid >= MAX_THREADS) return -1;
    disable_preempt();
    tcb_t *target = &threads[tid];

    if (target->state != TS_ZOMBIE) {
        target->join_waiting = current->tid;
        current->state = TS_BLOCKED;
        tcb_t *me = current;
        current = NULL;
        swapcontext(&me->ctx, &scheduler_ctx);
        disable_preempt();
    }

    if (retval) *retval = target->retval;

    // Clean up the zombie
    free(target->stack);
    target->stack = NULL;
    target->state = TS_UNUSED;

    enable_preempt();
    return 0;
}

kthread_tid_t kthread_self(void) {
    return current ? current->tid : -1;
}

void kthread_yield(void) {
    disable_preempt();
    stats.voluntary_yields++;
    tcb_t *me = current;
    me->state = TS_READY;
    tq_push(&mlfq[me->mlfq_level], me);
    current = NULL;
    swapcontext(&me->ctx, &scheduler_ctx);
    enable_preempt();
}

void kthread_sleep(int ms) {
    disable_preempt();
    current->sleep_until_ms = now_ms() + ms;
    current->state = TS_SLEEPING;
    tq_push(&sleep_queue, current);
    tcb_t *me = current;
    current = NULL;
    swapcontext(&me->ctx, &scheduler_ctx);
    enable_preempt();
}

// ─────────────────────────────────────────────────────────────
//  MUTEX
// ─────────────────────────────────────────────────────────────

struct kthread_mutex {
    volatile int  locked;
    kthread_tid_t owner;
    tqueue_t      waiters;
};

kthread_mutex_t *kthread_mutex_create(void) {
    kthread_mutex_t *m = calloc(1, sizeof(*m));
    m->owner = -1;
    return m;
}
void kthread_mutex_destroy(kthread_mutex_t *m) { free(m); }

void kthread_mutex_lock(kthread_mutex_t *m) {
    disable_preempt();
    while (m->locked) {
        current->state = TS_BLOCKED;
        tq_push(&m->waiters, current);
        tcb_t *me = current;
        current = NULL;
        swapcontext(&me->ctx, &scheduler_ctx);
        disable_preempt();
    }
    m->locked = 1;
    m->owner  = current->tid;
    enable_preempt();
}

int kthread_mutex_trylock(kthread_mutex_t *m) {
    disable_preempt();
    int got = 0;
    if (!m->locked) {
        m->locked = 1;
        m->owner  = current->tid;
        got = 1;
    }
    enable_preempt();
    return got;
}

void kthread_mutex_unlock(kthread_mutex_t *m) {
    disable_preempt();
    assert(m->locked && m->owner == current->tid);
    m->locked = 0;
    m->owner  = -1;
    tcb_t *waiter = tq_pop(&m->waiters);
    if (waiter) make_ready(waiter);
    enable_preempt();
}

// ─────────────────────────────────────────────────────────────
//  CONDITION VARIABLE
// ─────────────────────────────────────────────────────────────

struct kthread_cond {
    tqueue_t waiters;
};

kthread_cond_t *kthread_cond_create(void) {
    return calloc(1, sizeof(kthread_cond_t));
}
void kthread_cond_destroy(kthread_cond_t *c) { free(c); }

void kthread_cond_wait(kthread_cond_t *c, kthread_mutex_t *m) {
    disable_preempt();
    kthread_mutex_unlock(m);

    current->state = TS_BLOCKED;
    tq_push(&c->waiters, current);
    tcb_t *me = current;
    current = NULL;
    swapcontext(&me->ctx, &scheduler_ctx);

    // Woken — re-acquire mutex
    kthread_mutex_lock(m);
}

void kthread_cond_signal(kthread_cond_t *c) {
    disable_preempt();
    tcb_t *t = tq_pop(&c->waiters);
    if (t) make_ready(t);
    enable_preempt();
}

void kthread_cond_broadcast(kthread_cond_t *c) {
    disable_preempt();
    tcb_t *t;
    while ((t = tq_pop(&c->waiters)) != NULL) make_ready(t);
    enable_preempt();
}

// ─────────────────────────────────────────────────────────────
//  SEMAPHORE
// ─────────────────────────────────────────────────────────────

struct kthread_sem {
    int      count;
    tqueue_t waiters;
};

kthread_sem_t *kthread_sem_create(int initial) {
    kthread_sem_t *s = calloc(1, sizeof(*s));
    s->count = initial;
    return s;
}
void kthread_sem_destroy(kthread_sem_t *s) { free(s); }

void kthread_sem_wait(kthread_sem_t *s) {
    disable_preempt();
    while (s->count == 0) {
        /* No resource available — block */
        current->state = TS_BLOCKED;
        tq_push(&s->waiters, current);
        tcb_t *me = current;
        current = NULL;
        swapcontext(&me->ctx, &scheduler_ctx);
        /* Woken by sem_post which already decremented count for us */
        enable_preempt();
        return;
    }
    s->count--;
    enable_preempt();
}

void kthread_sem_post(kthread_sem_t *s) {
    disable_preempt();
    tcb_t *t = tq_pop(&s->waiters);
    if (t) {
        /* Hand resource directly to waiter — don't increment count */
        make_ready(t);
    } else {
        s->count++;
    }
    enable_preempt();
}

// ─────────────────────────────────────────────────────────────
//  STATS
// ─────────────────────────────────────────────────────────────

kthread_stats_t kthread_get_stats(void) { return stats; }

void kthread_print_stats(void) {
    printf("\n  ── Scheduler Statistics ──────────────────────\n");
    printf("  Total threads created   : %ld\n", stats.total_threads_created);
    printf("  Context switches        : %ld\n", stats.context_switches);
    printf("  Preemptions (SIGALRM)   : %ld\n", stats.preemptions);
    printf("  Voluntary yields        : %ld\n", stats.voluntary_yields);
    printf("  ─────────────────────────────────────────────\n\n");
}
