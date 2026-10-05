#ifndef UTHREAD_H
#define UTHREAD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <semaphore.h>

#define MAX_UTHREADS 128
#define STACK_SIZE (64 * 1024)
#define MAX_WORKERS 32

struct context
{
    uint64_t rsp;
    uint64_t rip;

    uint64_t rbp;
    uint64_t rbx;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
};

typedef enum
{
    U_READY,
    U_RUNNING,
    U_BLOCKED,
    U_DONE,
    U_WAITING
} ustate_t;

typedef enum
{
    JOINABLE,
    DETACHABLE
} utype_t;

typedef struct uthread
{
    int id;

    ustate_t state;

    struct context ctx;

    void *stack;
    size_t stack_size;

    void (*fn)(void *);
    void *arg;
    utype_t type;
    sem_t status;

    /*
     * ULT which is waiting for this thread.
     *
     * For now we support one joiner per ULT,
     * similar to pthread_join semantics.
     */
    int joiner_id;

} uthread_t;

typedef struct
{
    int total_threads; // Number of threads required to trip the barrier
    int count;         // Current number of threads arrived
    sem_t mutex;
    int *blocked_threads;
    int pos; // Protects the counter

} uthread_barrier_t;
/* Assembly context switch */
void context_switch(
    struct context *old,
    const struct context *new_ctx);

/* Provided by uthread.c (add these prototypes to uthread.h if you like). */
int uthread_printf(const char *fmt, ...);
long uthread_gettid(void);

/* ULT API */
int uthread_create(
    void (*fn)(void *),
    void *arg, utype_t t);

void uthread_yield(void);

void uthread_exit(void)
    __attribute__((noreturn));

int uthread_join(int tid);

/* Runtime */
int intialize_workers(int workers);

void shutdown_workers(void);

void uthread_barrier_init(uthread_barrier_t *barrier, int count);
void uthread_barrier_wait(uthread_barrier_t *barrier);
void uthread_barrier_destroy(uthread_barrier_t *barrier);

#endif