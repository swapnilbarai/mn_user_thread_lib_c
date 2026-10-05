// Inside tests/mpmc.cpp
extern "C"
{
#include "uthread.h" // Replace with your actual header name
}

#include "lock_free_queue.hpp"
#include <vector>

std::atomic<std::size_t> producer_s;
std::atomic<std::size_t> consumer_s;

LockFreeQueue<int, 1024> *queue;

struct producer_args
{
    int id;
    int s;
    int e;
};

void produce(void *args)
{
    struct producer_args *prod = static_cast<struct producer_args *>(args);
    uthread_printf("Producer %d start. s= %d, e= %d\n", prod->id, prod->s, prod->e);
    for (int i = prod->s; i < prod->e; i++)
    {
        uthread_printf("Sending %d from producer %d\n", i, prod->id);
        queue->try_push(i);
        producer_s.fetch_add(i, std::memory_order_relaxed);
        if (i % 3 == 0)
        {
            uthread_yield();
        }
    }
}
void consume(void *args)
{
    int id = *(int *)args;
    int i = 0;

    while (i < 10)
    {
        int val;
        if (queue->try_pop(val))
        {
            uthread_printf("Recieved %d to consumer %d\n", val, id);
            consumer_s.fetch_add(i, std::memory_order_relaxed);
            i++;
        }
        else
        {
            uthread_yield();
        }
    }
}

int main()
{
    queue = new LockFreeQueue<int, 1024>();
    producer_s.store(0, std::memory_order_relaxed);
    consumer_s.store(0, std::memory_order_relaxed);
    intialize_workers(2);
    int tids[3];
    std::vector<struct producer_args *> refs(2);
    for (int i = 0; i < 3; i++)
    {
        if (i < 2)
        {
            producer_args *args = new producer_args;

            args->id = i;
            args->s = i * 5;
            args->e = (i + 1) * 5;
            refs[i] = args;
            // printf("Creating Producer %d start. s= %d, e= %d\n", args.id, args.s, args.e);
            tids[i] = uthread_create(produce, args, JOINABLE);
        }
        else
        {
            int arg = i;
            tids[i] = uthread_create(consume, &arg, JOINABLE);
        }
    }
    for (int i = 0; i < 3; i++)
    {
        uthread_join(tids[i]);
    }
    int p_s = producer_s.load(std::memory_order_relaxed);
    int c_s = consumer_s.load(std::memory_order_relaxed);
    printf("Producer Sum : %d, Consumer Sum %d\n", p_s, c_s);

    delete queue;
    for (int i = 0; i < 2; i++)
    {
        delete refs[i];
    }
    refs.clear();
    shutdown_workers();
}