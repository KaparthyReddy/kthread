// ================================================================
//  demo.c — kthread feature showcase
//
//  Demo 1 — PREEMPTION  : 4 CPU-bound threads, zero yields.
//                          SIGALRM forces all context switches.
//  Demo 2 — MUTEX       : 8 threads do concurrent bank deposits
//                          and withdrawals; balance must stay exact.
//  Demo 3 — PROD/CONS   : mutex + condition variables; 2 producers,
//                          4 consumers, bounded buffer.
//  Demo 4 — SEMAPHORE   : 8 workers competing for 3 resources;
//                          semaphore enforces the cap.
// ================================================================
#define _GNU_SOURCE
#include "kthread.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define BANNER(s) \
    printf("\n╔══════════════════════════════════════════╗\n"); \
    printf("║  %-40s║\n", s); \
    printf("╚══════════════════════════════════════════╝\n\n")

// ─────────────────────────────────────────────────────────────
//  Demo 1 — Preemption
// ─────────────────────────────────────────────────────────────
static volatile long hog_count[4];

static void cpu_hog(void *arg) {
    int id = (int)(intptr_t)arg;
    for (long i = 0; i < 50000000L; i++) hog_count[id]++;
}

static void demo_preemption(void) {
    BANNER("Demo 1: Preemption via SIGALRM");
    printf("  4 CPU-bound threads, no yields — pure preemption\n\n");
    kthread_init(SCHED_MLFQ, 10);

    kthread_tid_t tids[4];
    for (int i = 0; i < 4; i++)
        tids[i] = kthread_create(cpu_hog, (void *)(intptr_t)i, NULL);
    for (int i = 0; i < 4; i++) kthread_join(tids[i], NULL);

    printf("  Final counts (each should be 50,000,000):\n");
    for (int i = 0; i < 4; i++)
        printf("    Thread %d: %ld\n", i, hog_count[i]);
    kthread_print_stats();
    kthread_shutdown();
}

// ─────────────────────────────────────────────────────────────
//  Demo 2 — Mutex: bank account
// ─────────────────────────────────────────────────────────────
static kthread_mutex_t *bank_mutex;
static long             balance;
#define BANK_OPS 2000

static void banker(void *arg) {
    (void)arg;
    for (int i = 0; i < BANK_OPS; i++) {
        kthread_mutex_lock(bank_mutex);   balance++; kthread_mutex_unlock(bank_mutex);
        kthread_yield();
        kthread_mutex_lock(bank_mutex);   balance--; kthread_mutex_unlock(bank_mutex);
    }
}

static void demo_mutex(void) {
    BANNER("Demo 2: Mutex — Bank Account");
    printf("  8 threads: %d deposits + %d withdrawals each\n\n",
           BANK_OPS, BANK_OPS);
    kthread_init(SCHED_RR, 5);

    bank_mutex  = kthread_mutex_create();
    balance     = 1000;
    long initial = balance;

    kthread_tid_t tids[8];
    for (int i = 0; i < 8; i++)
        tids[i] = kthread_create(banker, NULL, NULL);
    for (int i = 0; i < 8; i++) kthread_join(tids[i], NULL);

    printf("  Initial balance : %ld\n", initial);
    printf("  Final balance   : %ld\n", balance);
    printf("  Result          : %s\n",
           balance == initial ? "PASS — mutex prevented data races"
                              : "FAIL — data race detected!");
    kthread_mutex_destroy(bank_mutex);
    kthread_print_stats();
    kthread_shutdown();
}

// ─────────────────────────────────────────────────────────────
//  Demo 3 — Producer / Consumer
// ─────────────────────────────────────────────────────────────
#define BUF_SIZE  8
#define NUM_ITEMS 32

static kthread_mutex_t *pc_mutex;
static kthread_cond_t  *not_full, *not_empty;
static int   pc_buf[BUF_SIZE], pc_head, pc_tail, pc_count;
static long  total_consumed;

static void producer(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < NUM_ITEMS / 2; i++) {
        kthread_mutex_lock(pc_mutex);
        while (pc_count == BUF_SIZE) kthread_cond_wait(not_full, pc_mutex);
        pc_buf[pc_tail] = id * 100 + i;
        pc_tail = (pc_tail + 1) % BUF_SIZE;
        pc_count++;
        kthread_cond_signal(not_empty);
        kthread_mutex_unlock(pc_mutex);
        kthread_sleep(2);
    }
}

static void consumer(void *arg) {
    (void)arg;
    for (int i = 0; i < NUM_ITEMS / 4; i++) {
        kthread_mutex_lock(pc_mutex);
        while (pc_count == 0) kthread_cond_wait(not_empty, pc_mutex);
        pc_head = (pc_head + 1) % BUF_SIZE;
        pc_count--;
        total_consumed++;
        kthread_cond_signal(not_full);
        kthread_mutex_unlock(pc_mutex);
        kthread_sleep(3);
    }
}

static void demo_prod_cons(void) {
    BANNER("Demo 3: Producer/Consumer (Cond Vars)");
    printf("  2 producers, 4 consumers, buffer size %d\n\n", BUF_SIZE);
    kthread_init(SCHED_MLFQ, 10);

    pc_mutex  = kthread_mutex_create();
    not_full  = kthread_cond_create();
    not_empty = kthread_cond_create();
    pc_head = pc_tail = pc_count = 0;
    total_consumed = 0;

    kthread_tid_t tids[6];
    tids[0] = kthread_create(producer, (void *)(intptr_t)0, NULL);
    tids[1] = kthread_create(producer, (void *)(intptr_t)1, NULL);
    for (int i = 0; i < 4; i++)
        tids[2+i] = kthread_create(consumer, NULL, NULL);
    for (int i = 0; i < 6; i++) kthread_join(tids[i], NULL);

    printf("  Items produced : %d\n",  NUM_ITEMS);
    printf("  Items consumed : %ld\n", total_consumed);
    printf("  Result         : %s\n",
           total_consumed == NUM_ITEMS ? "PASS" : "FAIL — item mismatch!");
    kthread_mutex_destroy(pc_mutex);
    kthread_cond_destroy(not_full);
    kthread_cond_destroy(not_empty);
    kthread_print_stats();
    kthread_shutdown();
}

// ─────────────────────────────────────────────────────────────
//  Demo 4 — Semaphore: resource pool
// ─────────────────────────────────────────────────────────────
#define NUM_WORKERS   8
#define NUM_RESOURCES 3

static kthread_sem_t   *resource_sem;
static kthread_mutex_t *max_mutex;
static volatile int     concurrent_holders;
static volatile int     max_concurrent;

static void worker(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < 3; i++) {
        kthread_sem_wait(resource_sem);

        kthread_mutex_lock(max_mutex);
        concurrent_holders++;
        if (concurrent_holders > max_concurrent)
            max_concurrent = concurrent_holders;
        kthread_mutex_unlock(max_mutex);

        kthread_sleep(10 + id * 2);   // use resource

        kthread_mutex_lock(max_mutex);
        concurrent_holders--;
        kthread_mutex_unlock(max_mutex);

        kthread_sem_post(resource_sem);
        kthread_sleep(5);
    }
}

static void demo_semaphore(void) {
    BANNER("Demo 4: Semaphore — Resource Pool");
    printf("  %d workers competing for %d resources\n\n",
           NUM_WORKERS, NUM_RESOURCES);
    kthread_init(SCHED_MLFQ, 10);

    resource_sem       = kthread_sem_create(NUM_RESOURCES);
    max_mutex          = kthread_mutex_create();
    concurrent_holders = 0;
    max_concurrent     = 0;

    kthread_tid_t tids[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++)
        tids[i] = kthread_create(worker, (void *)(intptr_t)i, NULL);
    for (int i = 0; i < NUM_WORKERS; i++) kthread_join(tids[i], NULL);

    printf("  Max simultaneous holders : %d\n",  max_concurrent);
    printf("  Enforced limit           : %d\n",  NUM_RESOURCES);
    printf("  Result                   : %s\n",
           max_concurrent <= NUM_RESOURCES
           ? "PASS — semaphore enforced the cap"
           : "FAIL — limit exceeded!");
    kthread_sem_destroy(resource_sem);
    kthread_mutex_destroy(max_mutex);
    kthread_print_stats();
    kthread_shutdown();
}

// ─────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────
int main(void) {
    printf("\n");
    printf("  ██╗  ██╗████████╗██╗  ██╗██████╗ ███████╗ █████╗ ██████╗ \n");
    printf("  ██║ ██╔╝╚══██╔══╝██║  ██║██╔══██╗██╔════╝██╔══██╗██╔══██╗\n");
    printf("  █████╔╝    ██║   ███████║██████╔╝█████╗  ███████║██║  ██║\n");
    printf("  ██╔═██╗    ██║   ██╔══██║██╔══██╗██╔══╝  ██╔══██║██║  ██║\n");
    printf("  ██║  ██╗   ██║   ██║  ██║██║  ██║███████╗██║  ██║██████╔╝\n");
    printf("  ╚═╝  ╚═╝   ╚═╝   ╚═╝  ╚═╝╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝╚═════╝\n");
    printf("  Preemptive Userspace Thread Library\n\n");

    demo_preemption();
    demo_mutex();
    demo_prod_cons();
    demo_semaphore();

    printf("\n  All demos complete.\n\n");
    return 0;
}
