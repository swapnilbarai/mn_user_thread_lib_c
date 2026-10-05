#ifndef LOCK_FREE_QUEUE_HPP
#define LOCK_FREE_QUEUE_HPP

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>

template <typename T, std::size_t Capacity>
class LockFreeQueue
{
    static_assert(Capacity > 0);

    struct Cell
    {
        std::atomic<std::size_t> sequence;
        T data;

        Cell() : sequence(0) {}
    };

public:
    LockFreeQueue()
        : enqueue_pos_(0),
          dequeue_pos_(0)
    {
        for (std::size_t i = 0; i < Capacity; ++i)
        {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    bool try_push(const T &value)
    {
        Cell *cell;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        while (true)
        {
            cell = &buffer_[pos % Capacity];

            const std::size_t seq =
                cell->sequence.load(std::memory_order_acquire);

            const intptr_t diff =
                static_cast<intptr_t>(seq) -
                static_cast<intptr_t>(pos);

            if (diff == 0)
            {
                // Try to claim this position.
                if (enqueue_pos_.compare_exchange_weak(
                        pos,
                        pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                {
                    break;
                }
            }
            else if (diff < 0)
            {
                // Queue is full.
                return false;
            }
            else
            {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        cell->data = value;

        // Publish the data to consumers.
        cell->sequence.store(
            pos + 1,
            std::memory_order_release);

        return true;
    }

    bool try_pop(T &value)
    {
        Cell *cell;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        while (true)
        {
            cell = &buffer_[pos % Capacity];

            const std::size_t seq =
                cell->sequence.load(std::memory_order_acquire);

            const intptr_t diff =
                static_cast<intptr_t>(seq) -
                static_cast<intptr_t>(pos + 1);

            if (diff == 0)
            {
                // Try to claim this position.
                if (dequeue_pos_.compare_exchange_weak(
                        pos,
                        pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                {
                    break;
                }
            }
            else if (diff < 0)
            {
                // Queue is empty.
                return false;
            }
            else
            {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        value = cell->data;

        // Mark the slot as free for the next producer cycle.
        cell->sequence.store(
            pos + Capacity,
            std::memory_order_release);

        return true;
    }

private:
    alignas(64) std::atomic<std::size_t> enqueue_pos_;
    alignas(64) std::atomic<std::size_t> dequeue_pos_;

    alignas(64) Cell buffer_[Capacity];
};

#endif