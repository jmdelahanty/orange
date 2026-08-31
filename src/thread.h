#ifndef ORANGE_THREADS
#define ORANGE_THREADS
#include <stdint.h>
#include <time.h>
#include <atomic>
#include <memory>
#include <queue> // Required for std::queue
#include "genericmutex.h" 

inline float tick()
{
    struct timespec ts;
    int res = clock_gettime(CLOCK_MONOTONIC, &ts);
    if (res == -1)
    {
        return 0;
    }
    return ((float)((ts.tv_sec * 1e9) + ts.tv_nsec)) / (float)1.0e9;
}

inline uint64_t sync_fetch_and_add(volatile uint64_t *x, uint64_t by)
{
    return __sync_fetch_and_add(x, by);
}

// A simple, thread-safe queue using the CGenericMutex
template <typename T>
class SafeQueue
{
public:
    void push(T val)
    {
        mutex.Lock();
        LockGuard unlock(mutex);
        queue.push(val);
    }

    bool pop(T& val)
    {
        bool success = false;
        mutex.Lock();
        LockGuard unlock(mutex);
        if (!queue.empty())
        {
            val = queue.front();
            queue.pop();
            success = true;
        }
        return success;
    }

private:
    class LockGuard final
    {
    public:
        explicit LockGuard(CGenericMutex& mutex) noexcept
            : mutex_(mutex)
        {
        }

        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;

        ~LockGuard() noexcept
        {
            mutex_.Unlock();
        }

    private:
        CGenericMutex& mutex_;
    };

    std::queue<T> queue;
    CGenericMutex mutex;
};

#endif // ORANGE_THREADS
