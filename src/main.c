#include "uthread.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>


uthread_barrier_t barrier;

struct b_data
{
    int limit;
    int count;
    int product;
};

/*
 * IMPORTANT: code running inside a ULT executes on a clone()d worker
 * that shares TLS with main. Do NOT call printf/malloc/etc. here.
 * Use uthread_printf() instead.
 */

static void product(void *args)
{
    struct b_data *b = (struct b_data *)args;
    int p = 1;

    for (int i = 1; i <= b->limit; i++)
        p = (i * p) % 10001;

    b->product = p;
    uthread_printf("Product: %d\n", p);
}

static void sieve_of_eratosthenes(void *args)
{
    struct b_data *b = (struct b_data *)args;
    int limit = b->limit;
    bool is_prime[limit + 1];

    memset(is_prime, true, sizeof(is_prime));
    is_prime[0] = is_prime[1] = false;

    for (int p = 2; p * p <= limit; p++)
        if (is_prime[p])
            for (int i = p * p; i <= limit; i += p)
                is_prime[i] = false;

    int count = 0;
    for (int p = 2; p <= limit; p++)
        if (is_prime[p])
            count++;

    b->count = count;
    uthread_printf("Primes: %d\n", count);
}

static void worker(void *arg)
{
    int id = *(int *)arg;

    for (int i = 0; i < 2; ++i)
        uthread_printf("ULT %d | iteration %d | kernel tid %ld\n",
                       id, i, uthread_gettid());

    uthread_printf("ULT %d finished\n", id);
}

int main(void)
{
    enum { M = 3, N = 5 };

    int args[N];
    int tids[N];

    if (intialize_workers(M) < 0)
    {
        fprintf(stderr, "runtime_start failed\n");
        return 1;
    }

    struct b_data b = {.limit = 1000};

    tids[0] = uthread_create(sieve_of_eratosthenes, &b, JOINABLE);
    tids[1] = uthread_create(product, &b, JOINABLE);

    for (int i = 2; i < 4; ++i)
    {
        args[i] = i;
        tids[i] = uthread_create(worker, &args[i], JOINABLE);
        if (tids[i] < 0)
        {
            fprintf(stderr, "uthread_create failed\n");
            return 1;
        }
    }

    /* Join instead of sleep(). */
    for (int i = 0; i < 4; ++i)
        uthread_join(tids[i]);

    /* main is not a worker, so its own printf is fine. */
    printf("primes=%d product=%d\n", b.count, b.product);
    printf("Creating Detachable Thread\n");
    fflush(stdout);

    args[4] = 4;
    uthread_create(worker, &args[4], DETACHABLE);

    shutdown_workers(); /* waits for all ULTs, then for the workers */

    printf("All ULTs joined.\n");
    return 0;
}