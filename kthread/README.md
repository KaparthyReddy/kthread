# kthread

A preemptive userspace thread library built from scratch in C. Implements everything a kernel does at its core — context switching, a Multi-Level Feedback Queue scheduler, and all classical synchronization primitives — entirely in userspace, with zero external dependencies beyond libc.

---

## Why this exists

Most developers use `pthread_mutex_lock` without knowing what's underneath it. This project builds the underneath. Every piece — the scheduler, the context switch, the mutex, the condition variable, the semaphore — is written from first principles. No OS threading APIs, no atomic builtins for synchronization, no shortcuts.

---

## How it works

### Context Switching — `ucontext`
Each thread gets its own stack and a `ucontext_t` register snapshot. Switching threads is a single `swapcontext()` call — the CPU registers, instruction pointer, and stack pointer are all swapped atomically. This is exactly what a kernel does on a timer interrupt, just without ring-0 privilege.

### Preemption — `SIGALRM`
A real OS preempts threads using a hardware timer interrupt. `kthread` replicates this with `SIGALRM` — the kernel fires a signal after each quantum expires, the signal handler yanks the CPU away from whatever thread is running and jumps back to the scheduler. The running thread had no say in the matter.

```
Thread running → SIGALRM fires → sigalrm_handler() → swapcontext() → scheduler
```

### Scheduler — Multi-Level Feedback Queue (MLFQ)
The same algorithm used in real operating systems (BSD, early Windows NT). Three priority queues with doubling time quanta:

```
Queue 0  (highest) — quantum: 10ms   ← new threads start here
Queue 1  (middle)  — quantum: 20ms
Queue 2  (lowest)  — quantum: 40ms   ← CPU hogs end up here
```

- A thread that burns through its full quantum gets **demoted** to the next queue
- A thread that blocks (I/O, mutex, sleep) gets to **stay** in its current queue
- Every 500ms, all threads get a **priority boost** back to queue 0 to prevent starvation

This means I/O-bound threads naturally stay responsive at the top, and CPU hogs get fair but lower-priority time at the bottom — without any manual priority annotation.

### Synchronization Primitives

**Mutex** — a thread that can't acquire the lock is moved to the mutex's private wait queue and context-switches out immediately. No spinning. When the holder unlocks, it pulls the first waiter out of the queue and makes it runnable.

**Condition Variable** — `cond_wait` atomically releases the mutex and blocks on the condition's wait queue (no lost-wakeup window). `cond_signal` wakes one waiter; `cond_broadcast` wakes all.

**Semaphore** — classic counting semaphore. `sem_post` hands the resource directly to a waiter if one exists, rather than incrementing and re-checking — this avoids the retry-loop deadlock that catches most from-scratch implementations.

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                   User Threads                      │
│  thread_0   thread_1   thread_2   ...   thread_N    │
└────────┬────────────────────────────────────────────┘
         │  swapcontext() / SIGALRM
┌────────▼────────────────────────────────────────────┐
│                    Scheduler                        │
│                                                     │
│  MLFQ Queue 0  [ T3 → T7 → T1 ]  (10ms quantum)   │
│  MLFQ Queue 1  [ T5 → T2      ]  (20ms quantum)   │
│  MLFQ Queue 2  [ T4           ]  (40ms quantum)   │
│                                                     │
│  Sleep Queue   [ T6(wake@120ms) → T8(wake@250ms) ] │
└─────────────────────────────────────────────────────┘
```

---

## Project Structure

```
kthread/
├── kthread.h     # Public API — all types and function declarations
├── kthread.c     # Scheduler, context switcher, all primitives
├── demo.c        # 4 live demonstrations
├── Makefile
└── Dockerfile
```

---

## Demos & Output

### Demo 1 — Preemption via SIGALRM
4 CPU-bound threads each count to 50,000,000. Zero voluntary yields. The scheduler cuts them off every 10ms via SIGALRM.

```
  4 CPU-bound threads, no yields — pure preemption

  Final counts (each should be 50,000,000):
    Thread 0: 50000000
    Thread 1: 50000000
    Thread 2: 50000000
    Thread 3: 50000000

  Context switches : 15
  Preemptions      : 9      ← SIGALRM fired, not the thread
  Voluntary yields : 0
```

### Demo 2 — Mutex: Bank Account
8 threads each perform 2,000 deposits (+1) and 2,000 withdrawals (-1) on a shared balance. Without the mutex the balance would be garbage. With it, the invariant holds perfectly.

```
  8 threads: 2000 deposits + 2000 withdrawals each

  Initial balance : 1000
  Final balance   : 1000
  Result          : PASS — mutex prevented data races

  Context switches : 16009
  Voluntary yields : 16000
```

### Demo 3 — Producer/Consumer (Condition Variables)
2 producers and 4 consumers sharing a bounded buffer of size 8. Producers block when the buffer is full; consumers block when it's empty. 32 items produced and consumed with zero losses.

```
  2 producers, 4 consumers, buffer size 8

  Items produced : 32
  Items consumed : 32
  Result         : PASS

  Context switches : 102
```

### Demo 4 — Semaphore: Resource Pool
8 workers competing for 3 shared resources. The semaphore guarantees at most 3 workers hold a resource simultaneously — verified by tracking the real-time concurrent count.

```
  8 workers competing for 3 resources

  Max simultaneous holders : 3
  Enforced limit           : 3
  Result                   : PASS — semaphore enforced the cap

  Context switches : 85
```

---

## How to Run

### Docker (recommended — no install needed)

```bash
docker build -t kthread-demo .
docker run --rm kthread-demo
```

### Local build (Linux / macOS)

```bash
make
./demo
```

> **Note:** `ucontext` is a POSIX standard but deprecated on macOS (it still works). For full signal preemption behaviour, Linux inside Docker is recommended.

---

## Implementation Notes

- **No `pthread` anywhere.** The only system calls used are `sigaction`, `setitimer`, `swapcontext`, `makecontext`, `getcontext`, and `gettimeofday`.
- **Preemption guard.** A `preempt_disabled` flag prevents the signal handler from firing mid-context-switch — the equivalent of a kernel's `cli`/`sti` (disable/enable interrupts).
- **Semaphore correctness.** The classic from-scratch bug is implementing `sem_wait` as a retry loop. This library hands the resource directly from `sem_post` to a waiter, so no waiter ever needs to retry.
- **No memory leaks.** Thread stacks are `malloc`'d on create and `free`'d on join.

---

## Concepts Demonstrated

| Concept | Where |
|---|---|
| Context switching | `kthread_yield`, `kthread_exit`, SIGALRM handler |
| Hardware timer interrupt (simulated) | `SIGALRM` + `setitimer` |
| Multi-Level Feedback Queue scheduling | `scheduler()`, `make_ready()`, `sigalrm_handler()` |
| Starvation prevention | `maybe_boost()` — periodic priority reset |
| Mutex with blocking (no spinlock) | `kthread_mutex_lock/unlock` |
| Condition variable (no lost wakeup) | `kthread_cond_wait/signal/broadcast` |
| Counting semaphore | `kthread_sem_wait/post` |
| Thread join | `kthread_join` |
| Sleep queue | `kthread_sleep`, `wake_sleepers()` |
