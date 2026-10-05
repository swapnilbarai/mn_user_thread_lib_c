#define _GNU_SOURCE

#include "uthread.h"

#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <unistd.h>

/* ============================================================
 * WHY THIS FILE LOOKS DIFFERENT (read this first)
 *
 * Workers are created with raw clone() and WITHOUT CLONE_SETTLS,
 * so every worker shares the creating thread's TLS block (FS base).
 * glibc keeps per-thread state in TLS: malloc tcache/arena, errno,
 * the stdio lock owner, the cancellation state used by sem_wait...
 * Running that code on several kernel threads that share ONE TLS
 * corrupts it (this was the sysmalloc assertion).
 *
 * Rule: code that runs on a worker (the scheduler, every ULT, and
 * the library functions they call) must NOT touch TLS-dependent libc:
 *
 *   - no printf/fprintf/puts/perror   -> use uthread_printf()
 *   - no malloc/free                  -> call only from main
 *   - no sem_wait/sem_post            -> own futex semaphore below
 *   - no syscall()/sched_yield() etc. -> raw syscalls (errno is TLS)
 *
 * All of that is implemented below with inline-asm raw syscalls.
 * ============================================================ */

#define KSTACK_SIZE (128 * 1024)

/* ============================================================
 * Raw syscall (never touches errno / TLS)
 * ============================================================ */

static inline long rsys(long n, long a, long b, long c,
                        long d, long e, long f)
{
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

#define IS_ERR(r) ((unsigned long)(r) >= (unsigned long)-4095L)

static void raw_log(const char *msg)
{
    rsys(SYS_write, 2, (long)msg, (long)strlen(msg), 0, 0, 0);
}

static void raw_die(const char *msg)
{
    raw_log(msg);
    rsys(SYS_exit_group, 1, 0, 0, 0, 0, 0);
    for (;;)
        ;
}

static void raw_yield(void)
{
    rsys(SYS_sched_yield, 0, 0, 0, 0, 0, 0);
}

long uthread_gettid(void)
{
    return rsys(SYS_gettid, 0, 0, 0, 0, 0, 0);
}

/*
 * TLS-free printf replacement, safe in ULTs and workers.
 * Formats into a stack buffer, writes with one raw write().
 * Keep to %d %ld %s %x etc. (avoid floating point formats).
 */
int uthread_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0)
        return n;
    if (n >= (int)sizeof(buf))
        n = (int)sizeof(buf) - 1;

    rsys(SYS_write, 1, (long)buf, n, 0, 0, 0);
    return n;
}

/* ============================================================
 * Futex-based semaphore stored inside the existing sem_t fields.
 * (Replaces sem_init/sem_wait/sem_post, which use TLS.)
 * Only the first 4 bytes of the sem_t are used.
 * ============================================================ */

_Static_assert(sizeof(sem_t) >= sizeof(atomic_uint),
               "sem_t too small");

static void usem_init(sem_t *s, unsigned v)
{
    memset(s, 0, sizeof(*s));
    atomic_store((atomic_uint *)s, v);
}

static void usem_wait(sem_t *s)
{
    atomic_uint *a = (atomic_uint *)s;

    for (;;)
    {
        unsigned v = atomic_load(a);

        if (v > 0)
        {
            if (atomic_compare_exchange_weak(a, &v, v - 1))
                return;
            continue;
        }
        rsys(SYS_futex, (long)a, FUTEX_WAIT_PRIVATE, 0, 0, 0, 0);
    }
}

static void usem_post(sem_t *s)
{
    atomic_uint *a = (atomic_uint *)s;

    atomic_fetch_add(a, 1);
    rsys(SYS_futex, (long)a, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
}

/* ============================================================
 * Simple spinlock
 * ============================================================ */

typedef struct
{
    atomic_flag flag;
} spinlock_t;

static void lock(spinlock_t *l)
{
    while (atomic_flag_test_and_set_explicit(&l->flag,
                                             memory_order_acquire))
        raw_yield();
}

static void unlock(spinlock_t *l)
{
    atomic_flag_clear_explicit(&l->flag, memory_order_release);
}

/* ============================================================
 * Kernel worker: one clone()d Linux task
 * ============================================================ */

typedef struct worker
{
    int id;

    /* Linux TID of this worker. */
    _Atomic pid_t tid;

    /* Context of the scheduler itself. */
    struct context scheduler_ctx;

    /* ULT currently executing on this worker. */
    uthread_t *current;

    atomic_bool is_completed;

    /* Semaphore to post after switching away from a blocked ULT. */
    sem_t *waiting;

} worker_t;

/* ============================================================
 * Global state
 * ============================================================ */

static uthread_t threads[MAX_UTHREADS];
static int thread_count = 0;
static spinlock_t table_lock = {ATOMIC_FLAG_INIT};

/* Guards against double join / double free of a joinable ULT. */
static atomic_flag joined[MAX_UTHREADS];

static int ready_q[MAX_UTHREADS];
static int q_head = 0;
static int q_tail = 0;
static int q_count = 0;
static spinlock_t q_lock = {ATOMIC_FLAG_INIT};

/* ULTs still alive (READY / RUNNING / BLOCKED). */
static atomic_int live_threads;

static worker_t worker_state[MAX_WORKERS];
static int worker_count = 0;

/*
 * Kernel clears worker_ctid[i] and futex-wakes it when worker i
 * exits (CLONE_CHILD_CLEARTID). This is how we "join" a
 * CLONE_THREAD task (waitpid() does not work for those).
 */
static volatile pid_t worker_ctid[MAX_WORKERS];

static void scheduler(worker_t *w);
static int worker_main(void *arg);
static void uthread_trampoline(void);

/* ============================================================
 * Find the worker for the current kernel thread.
 * (gettid based: never use a __thread var here, since TLS is
 *  shared and ULTs may migrate between workers.)
 * ============================================================ */

__attribute__((noinline)) static worker_t *my_worker(void)
{
    pid_t tid = (pid_t)rsys(SYS_gettid, 0, 0, 0, 0, 0, 0);

    for (int i = 0; i < worker_count; ++i)
    {
        if (atomic_load(&worker_state[i].tid) == tid)
            return &worker_state[i];
    }
    return NULL;
}

/* ============================================================
 * Ready queue
 * ============================================================ */

static void enqueue(int id)
{
    lock(&q_lock);

    if (q_count >= MAX_UTHREADS)
        raw_die("ERROR: ready queue overflow\n");

    ready_q[q_tail] = id;
    q_tail = (q_tail + 1) % MAX_UTHREADS;
    ++q_count;

    unlock(&q_lock);
}

static int dequeue(void)
{
    int id = -1;

    lock(&q_lock);

    if (q_count > 0)
    {
        id = ready_q[q_head];
        q_head = (q_head + 1) % MAX_UTHREADS;
        --q_count;
    }

    unlock(&q_lock);
    return id;
}

/* ============================================================
 * ULT trampoline
 * ============================================================ */

static void uthread_trampoline(void)
{
    worker_t *w = my_worker();

    if (!w || !w->current)
        raw_die("ERROR: ULT trampoline has no current worker\n");

    uthread_t *t = w->current;

    t->fn(t->arg);

    uthread_exit();

    __builtin_unreachable();
}

/* ============================================================
 * Create a user-level thread
 * ============================================================ */

int uthread_create(void (*fn)(void *), void *arg, utype_t type)
{
    if (!fn)
        return -1;

    lock(&table_lock);
    if (thread_count >= MAX_UTHREADS)
    {
        unlock(&table_lock);
        return -1;
    }
    int id = thread_count++;
    unlock(&table_lock);

    uthread_t *t = &threads[id];

    memset(t, 0, sizeof(*t));

    t->id = id;
    t->state = U_READY;
    t->fn = fn;
    t->arg = arg;
    t->type = type;

    usem_init(&t->status, 0);

    t->joiner_id = -1;
    t->stack_size = STACK_SIZE;

    atomic_flag_clear(&joined[id]);

    long m = rsys(SYS_mmap, 0, (long)t->stack_size,
                  PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (IS_ERR(m))
    {
        t->stack = NULL;
        t->state = U_DONE; /* dead slot, never enqueued */
        return -1;
    }
    t->stack = (void *)m;

    uintptr_t stack_top = (uintptr_t)t->stack + t->stack_size;
    stack_top &= ~(uintptr_t)0xF;

    uintptr_t *sp = (uintptr_t *)stack_top;

    /*
     * After `ret` pops the trampoline address, rsp must be 16n+8
     * (as if called). So: pad slot first, then the trampoline.
     */
    *--sp = 0;
    *--sp = (uintptr_t)uthread_trampoline;

    t->ctx.rsp = (uint64_t)sp;
    t->ctx.rip = (uint64_t)uthread_trampoline;
    t->ctx.rbp = 0;
    t->ctx.rbx = 0;
    t->ctx.r12 = 0;
    t->ctx.r13 = 0;
    t->ctx.r14 = 0;
    t->ctx.r15 = 0;

    atomic_fetch_add_explicit(&live_threads, 1, memory_order_release);
    enqueue(t->id);

    return t->id;
}

/* ============================================================
 * Yield: RUNNING -> WAITING -> (scheduler) READY
 * The scheduler re-enqueues AFTER the switch, so no other worker
 * can run this ULT while it is still on its own stack.
 * ============================================================ */

void uthread_yield(void)
{
    worker_t *w = my_worker();

    if (!w || !w->current)
        return;

    uthread_t *old = w->current;

    old->state = U_WAITING;
    w->current = NULL;

    context_switch(&old->ctx, &w->scheduler_ctx);
}

/* ============================================================
 * Join: joinable stacks are freed here, exactly once.
 * ============================================================ */

int uthread_join(int tid)
{
    lock(&table_lock);
    int count = thread_count;
    unlock(&table_lock);

    if (tid < 0 || tid >= count)
        return -1;

    uthread_t *target = &threads[tid];

    if (target->type == DETACHABLE)
        return 0;

    if (atomic_flag_test_and_set(&joined[tid]))
        return 0;

    usem_wait(&target->status);

    if (target->stack)
    {
        rsys(SYS_munmap, (long)target->stack,
             (long)target->stack_size, 0, 0, 0, 0);
        target->stack = NULL;
    }

    return 0;
}

/* ============================================================
 * Exit current ULT
 * ============================================================ */

void uthread_exit(void)
{
    worker_t *w = my_worker();

    if (!w || !w->current)
        rsys(SYS_exit_group, 0, 0, 0, 0, 0, 0);

    uthread_t *old = w->current;

    old->state = U_DONE;

    if (old->joiner_id != -1)
    {
        int joiner_id = old->joiner_id;

        if (joiner_id >= 0 && joiner_id < thread_count)
        {
            uthread_t *joiner = &threads[joiner_id];

            if (joiner->state == U_BLOCKED)
            {
                joiner->state = U_READY;
                enqueue(joiner_id);
            }
        }
        old->joiner_id = -1;
    }

    w->current = NULL;

    atomic_fetch_sub_explicit(&live_threads, 1, memory_order_acq_rel);

    context_switch(&old->ctx, &w->scheduler_ctx);

    __builtin_unreachable();
}

/* ============================================================
 * Scheduler (runs on each kernel worker)
 * ============================================================ */

static void scheduler(worker_t *w)
{
    for (;;)
    {
        if (atomic_load(&w->is_completed))
            return;

        int id = dequeue();

        if (id < 0)
        {
            raw_yield();
            continue;
        }

        uthread_t *t = &threads[id];

        if (t->state != U_READY)
            continue;

        t->state = U_RUNNING;
        w->current = t;

        context_switch(&w->scheduler_ctx, &t->ctx);

        /* ---- ULT is fully switched out here ---- */

        if (w->waiting)
        {
            sem_t *sem = w->waiting;
            w->waiting = NULL;
            usem_post(sem);
        }

        if (t->state == U_WAITING)
        {
            t->state = U_READY;
            enqueue(t->id);
        }

        if (t->state == U_DONE && t->type == JOINABLE)
        {
            usem_post(&t->status);
        }
        else if (t->state == U_DONE && t->type == DETACHABLE)
        {
            rsys(SYS_munmap, (long)t->stack, (long)t->stack_size,
                 0, 0, 0, 0);
            t->stack = NULL;
        }
    }
}

/* ============================================================
 * Worker entry point (called by clone)
 * ============================================================ */

static int worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;

    atomic_store(&w->tid, (pid_t)rsys(SYS_gettid, 0, 0, 0, 0, 0, 0));

    scheduler(w);

    return 0; /* glibc's clone() stub turns this into SYS_exit */
}

/* ============================================================
 * Start M kernel workers with clone()
 * ============================================================ */

int intialize_workers(int nworkers)
{
    if (nworkers < 1 || nworkers > MAX_WORKERS)
        return -1;

    worker_count = nworkers;

    /* Fully initialise all workers BEFORE any of them starts. */
    for (int i = 0; i < nworkers; ++i)
    {
        worker_t *w = &worker_state[i];

        memset(w, 0, sizeof(*w));
        w->id = i;
        w->waiting = NULL;
        atomic_store(&w->tid, 0);
        atomic_store(&w->is_completed, false);
        worker_ctid[i] = 0;
    }

    for (int i = 0; i < nworkers; ++i)
    {
        long m = rsys(SYS_mmap, 0, KSTACK_SIZE,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (IS_ERR(m))
        {
            raw_log("mmap worker stack failed\n");
            return -1;
        }

        void *stack_top = (char *)m + KSTACK_SIZE; /* 16-byte aligned */

        /*
         * CLONE_THREAD            same thread group (needs SIGHAND, VM)
         * CLONE_PARENT_SETTID     store tid in worker_ctid[i] before return
         * CLONE_CHILD_CLEARTID    kernel zeroes it + futex-wakes on exit
         */
        int flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                    CLONE_THREAD | CLONE_SYSVSEM |
                    CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;

        pid_t pid = clone(worker_main,
                          stack_top,
                          flags,
                          &worker_state[i],
                          (pid_t *)&worker_ctid[i], /* ptid */
                          NULL,                     /* tls: none */
                          (pid_t *)&worker_ctid[i] /* ctid */);

        if (pid < 0)
        {
            raw_log("clone failed\n");
            return -1;
        }
    }

    return 0;
}

int initialize_workers(int nworkers)
{
    return intialize_workers(nworkers);
}

/* ============================================================
 * Stop and wait for all kernel workers.
 * Waits for all ULTs to finish, tells workers to stop, then waits
 * for each clone()d task to exit.
 * ============================================================ */

void shutdown_workers(void)
{
    while (atomic_load(&live_threads) > 0)
        usleep(1000);

    for (int i = 0; i < worker_count; i++)
        atomic_store(&worker_state[i].is_completed, true);

    for (int i = 0; i < worker_count; i++)
    {
        pid_t t;
        while ((t = worker_ctid[i]) != 0)
            rsys(SYS_futex, (long)&worker_ctid[i], FUTEX_WAIT, t, 0, 0, 0);
    }
}

/* ============================================================
 * Barrier
 * (init/destroy use malloc/free: call them from main only.)
 * ============================================================ */

void uthread_barrier_init(uthread_barrier_t *barrier, int threads_t)
{
    barrier->count = 0;
    barrier->total_threads = threads_t;
    usem_init(&barrier->mutex, 1);
    barrier->pos = 0;
    barrier->blocked_threads = (int *)malloc(sizeof(int) * threads_t);
}

void uthread_barrier_wait(uthread_barrier_t *barrier)
{
    usem_wait(&barrier->mutex);
    barrier->count++;

    if (barrier->count != barrier->total_threads)
    {
        worker_t *w = my_worker();
        uthread_t *thread = w->current;

        w->current = NULL;
        thread->state = U_BLOCKED;
        barrier->blocked_threads[barrier->pos++] = thread->id;

        /* Scheduler posts the mutex once this ULT is off its stack. */
        w->waiting = &barrier->mutex;
        context_switch(&thread->ctx, &w->scheduler_ctx);
    }
    else
    {
        for (int i = 0; i < barrier->pos; i++)
        {
            int thread_id = barrier->blocked_threads[i];
            uthread_t *t = &threads[thread_id];
            t->state = U_READY;
            enqueue(thread_id);
        }
        barrier->pos = 0;
        barrier->count = 0;
        usem_post(&barrier->mutex);
    }
}

void uthread_barrier_destroy(uthread_barrier_t *barrier)
{
    free(barrier->blocked_threads);
    barrier->blocked_threads = NULL;
}