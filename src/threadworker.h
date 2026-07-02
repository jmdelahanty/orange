// src/threadworker.h

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <vector>
#include "offthreadmachine.h"
// For should_log_worker_entry_release_issue() (exponentially spaced
// rate-limited logging shared with the entry-ownership diagnostics).
#include "worker_entry_ownership_core.h"
#include <cstdio>

#if defined(__GNUC__)
#include <unistd.h>
#endif

template<typename T>
class CThreadWorker : public COffThreadMachine
{
public:
    CThreadWorker(const char* name);
    virtual ~CThreadWorker();

    int StartThread(const char* tName = nullptr);
    void SetID(int i) { id = i; }
    int GetID() const { return id; }

    void Reset();
    int GetMyWork() const { return myWork; }

    // Type-safe methods using templates
    bool PutObjectToQueueIn(T* f);

    // Enqueue an ordered flush marker. Because it rides the input queue, the
    // worker processes every item ahead of it FIRST, then OnFlushTick() runs
    // on the worker thread. Used by the recording drain cascade (end-of-
    // stream marker) and as a wakeup for drain-condition polling.
    bool EnqueueFlushTick() { return PutObjectToQueueIn(nullptr); }
    void GetObjectsFromQueueOut(std::vector<T*>& items);
    T* GetObjectFromQueueOut();
    void PutObjectToQueueOut(T* f);
    T* GetObjectFromQueueIn();
    void SetMaxQueueSize(int size) { maxQueueSize = size; }

    // Optional bound for the output queue. 0 (the default) keeps the historic
    // unbounded behavior. When bounded and full, PutObjectToQueueOut drops the
    // OLDEST queued entry (drop-oldest keeps the freshest data for display /
    // preview consumers), increments the drop counter, and hands the dropped
    // entry to ReleaseDroppedQueueOutEntry(). Only opt in workers whose
    // output-queue consumers tolerate loss (display/preview); data-critical
    // workers must stay unbounded and rely on input-queue backpressure.
    //
    // IMPORTANT: entries are typically pool-backed and refcounted (see
    // worker_entry_ownership_core.h). If the entries placed on this worker's
    // output queue still own a pool reference that the consumer would release,
    // the subclass MUST override ReleaseDroppedQueueOutEntry() to release the
    // dropped entry exactly as the consumer would have, or the pool leaks.
    // Shrinking the bound below the current backlog trims (drops) the oldest
    // surplus entries through the same path.
    void SetMaxQueueOutSize(int size);

    int GetCountQueueInSize();
    int GetCountQueueOutSize();
    int GetCountQueueIn() const { return countQueueIn; }
    int GetCountQueueOut() const { return countQueueOut; }
    int GetCountInTotal() const { return countInTotal; }
    int GetCountOutTotal() const { return countOutTotal; }
    int GetCountQueueInMax() const { return countQueueInMax; }
    uint64_t GetCountQueueOutDropped() const
    {
        return countQueueOutDropped.load(std::memory_order_acquire);
    }

    void SetInterval(unsigned int i) {
        interval = i;
#ifdef _WIN32
        intervalMilliSeconds = interval / 1000;
        if(interval % 1000) intervalMilliSeconds++;
#endif
    }

protected:
    // The main worker function that derived classes must implement.
    // Returns true if the item should be passed to the output queue.
    //
    // Contract: f is never nullptr. Flush ticks (drain markers, shutdown
    // wakeups) are delivered to OnFlushTick() instead.
    virtual bool WorkerFunction(T* f) = 0;

    // Called on the worker thread when a flush tick is dequeued (see
    // EnqueueFlushTick) or when the stop signal wakes an empty queue at
    // shutdown. Because the tick rides the FIFO queue, everything enqueued
    // before it has already been processed when this runs. Override for
    // drain/close housekeeping (e.g. PoseWorker closes its event log,
    // EncoderHwWorker finalizes the video file or re-posts the tick while
    // drain conditions are pending).
    virtual void OnFlushTick() {}

    // This function is called when the worker is reset.
    virtual void WorkerReset() {}
    virtual void OnQueueInEnqueued(T*, int) {}
    virtual void OnQueueInDequeued(T*, int) {}

    // Called (off the queue lock) for every entry evicted from a bounded
    // output queue, and for entries discarded when Reset() clears the output
    // queue. Must release the entry exactly as the output-queue consumer
    // would have (e.g. release_worker_entry_to_recycle with this worker's
    // recycle queue), otherwise pool-backed entries leak. The default is a
    // no-op, which is only correct when the worker's WorkerFunction has
    // already released its reference before returning true (the pointer on
    // the output queue then owns nothing).
    virtual void ReleaseDroppedQueueOutEntry(T*) {}

    void GetQueueInSnapshotForInstrumentation(int* size, T** oldest);

private:
    // This overrides the pure virtual function in the base class COffThreadMachine.
    void ThreadRunning() override;
    void DoStopThread() override;

    void ResetInner();
    T* WaitForObjectFromQueueIn();
    void HandleDroppedQueueOutEntry(T* dropped, int capacity);

private:
    int id = 0;
    std::mutex mutexQueueIn;
    std::condition_variable queueInNotEmptyCv;
    std::condition_variable queueInNotFullCv;
    std::queue<T*> queueIn;
    std::mutex mutexQueueOut;
    std::queue<T*> queueOut;
    bool stopRequested = false;

    // Tracing counts. Atomic because they are mutated under the queue mutexes
    // but read lock-free from other threads (drain checks, GUI instrumentation).
    std::atomic<int> countQueueIn{0};
    std::atomic<int> countQueueOut{0};
    std::atomic<int> countInTotal{0};
    std::atomic<int> countOutTotal{0};
    std::atomic<int> countQueueInMax{0};
    std::atomic<uint64_t> countQueueOutDropped{0};
    int maxQueueSize = 40; // Default max queue size
    int maxQueueOutSize = 0; // Guarded by mutexQueueOut. 0 = unbounded (default)

    int myWork = 0;
    unsigned int interval;
#ifdef _WIN32
    unsigned int intervalMilliSeconds;
#endif
};

// --- Template Implementation ---

template<typename T>
CThreadWorker<T>::CThreadWorker(const char* name)
    : COffThreadMachine(name),
      id(0),
      countQueueIn(0),
      countQueueOut(0),
      countInTotal(0),
      countOutTotal(0),
      countQueueInMax(0),
      myWork(0),
      interval(10)
{
    this->ResetInner();
#ifdef _WIN32
    intervalMilliSeconds = 1;
#endif
}

template<typename T>
CThreadWorker<T>::~CThreadWorker()
{
}

template<typename T>
int CThreadWorker<T>::StartThread(const char* tName)
{
    {
        std::lock_guard<std::mutex> lock(mutexQueueIn);
        stopRequested = false;
    }
    const int result = COffThreadMachine::StartThread(tName);
    if (result != 0) {
        std::lock_guard<std::mutex> lock(mutexQueueIn);
        if (!this->IsMachineOn()) {
            stopRequested = true;
        }
    }
    return result;
}

template<typename T>
void CThreadWorker<T>::Reset()
{
    this->ResetInner();
    this->WorkerReset();
}

template<typename T>
void CThreadWorker<T>::ResetInner()
{
    {
        std::lock_guard<std::mutex> lock(mutexQueueIn);
        if (this->IsMachineOn()) {
            stopRequested = false;
        }
    }
    myWork = 0;
    countQueueIn = 0;
    countQueueOut = 0;
    countInTotal = 0;
    countOutTotal = 0;
    countQueueInMax = 0;
    countQueueOutDropped = 0;

    // Safely clear the queues
    {
        std::lock_guard<std::mutex> lock(mutexQueueIn);
        std::queue<T*> emptyIn;
        std::swap(queueIn, emptyIn);
    }
    queueInNotEmptyCv.notify_all();
    queueInNotFullCv.notify_all();

    std::queue<T*> emptyOut;
    {
        std::lock_guard<std::mutex> lock(mutexQueueOut);
        std::swap(queueOut, emptyOut);
    }
    // Release discarded output entries the same way a consumer would have,
    // so pool-backed entries are not leaked by a reset. Not counted as
    // overflow drops. (During construction this dispatches to the base-class
    // no-op; the queue is empty then, so nothing is lost.)
    while (!emptyOut.empty()) {
        T* entry = emptyOut.front();
        emptyOut.pop();
        if (entry) {
            this->ReleaseDroppedQueueOutEntry(entry);
        }
    }
}

template<typename T>
bool CThreadWorker<T>::PutObjectToQueueIn(T* f)
{
    std::unique_lock<std::mutex> lock(mutexQueueIn);
    queueInNotFullCv.wait(lock, [this]() {
        return queueIn.size() < static_cast<size_t>(maxQueueSize) || stopRequested;
    });
    if (stopRequested) {
        return false;
    }
    queueIn.push(f);
    countQueueIn++;
    countInTotal++;
    if (countQueueInMax < countQueueIn) {
        countQueueInMax = countQueueIn.load();
    }
    OnQueueInEnqueued(f, countQueueIn);
    lock.unlock();
    queueInNotEmptyCv.notify_one();
    return true;
}

template<typename T>
void CThreadWorker<T>::GetObjectsFromQueueOut(std::vector<T*>& items)
{
    std::lock_guard<std::mutex> lock(mutexQueueOut);
    items.clear();
    while (!queueOut.empty())
    {
        items.push_back(queueOut.front());
        queueOut.pop();
    }
    countOutTotal += static_cast<int>(items.size());
    countQueueOut = 0; // The queue is now empty
}

template<typename T>
T* CThreadWorker<T>::GetObjectFromQueueOut()
{
    T* f = nullptr;
    std::lock_guard<std::mutex> lock(mutexQueueOut);
    if (!queueOut.empty())
    {
        f = queueOut.front();
        queueOut.pop();
        countQueueOut--;
    }
    return f;
}


template<typename T>
void CThreadWorker<T>::PutObjectToQueueOut(T* f)
{
    T* dropped = nullptr;
    bool didDrop = false;
    int capacity = 0;
    {
        std::lock_guard<std::mutex> lock(mutexQueueOut);
        capacity = maxQueueOutSize;
        if (capacity > 0 && queueOut.size() >= static_cast<size_t>(capacity))
        {
            // Bounded and full: evict the oldest entry so the freshest data
            // stays available to the (slow) consumer.
            dropped = queueOut.front();
            queueOut.pop();
            countQueueOut--;
            didDrop = true;
        }
        queueOut.push(f);
        countQueueOut++;
    }
    if (didDrop) {
        this->HandleDroppedQueueOutEntry(dropped, capacity);
    }
}

template<typename T>
void CThreadWorker<T>::SetMaxQueueOutSize(int size)
{
    std::vector<T*> trimmed;
    {
        std::lock_guard<std::mutex> lock(mutexQueueOut);
        maxQueueOutSize = size;
        if (size > 0) {
            while (queueOut.size() > static_cast<size_t>(size)) {
                trimmed.push_back(queueOut.front());
                queueOut.pop();
                countQueueOut--;
            }
        }
    }
    for (T* entry : trimmed) {
        this->HandleDroppedQueueOutEntry(entry, size);
    }
}

// Runs off the queue lock: bumps the drop counter, releases the dropped entry
// via the subclass hook, and logs at exponentially spaced drop counts.
template<typename T>
void CThreadWorker<T>::HandleDroppedQueueOutEntry(T* dropped, int capacity)
{
    const uint64_t drops =
        countQueueOutDropped.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (dropped) {
        // Pool-backed, refcounted entries must be released exactly as the
        // consumer would have released them (see the hook's contract).
        this->ReleaseDroppedQueueOutEntry(dropped);
    }
    if (should_log_worker_entry_release_issue(drops)) {
        std::fprintf(stderr,
            "[CThreadWorker] %s: output queue full (cap=%d); dropped oldest "
            "entry (total drops=%llu)\n",
            threadName,
            capacity,
            static_cast<unsigned long long>(drops));
    }
}

template<typename T>
int CThreadWorker<T>::GetCountQueueInSize()
{
    std::lock_guard<std::mutex> lock(mutexQueueIn);
    return static_cast<int>(queueIn.size());
}

template<typename T>
int CThreadWorker<T>::GetCountQueueOutSize()
{
    std::lock_guard<std::mutex> lock(mutexQueueOut);
    return static_cast<int>(queueOut.size());
}

template<typename T>
T* CThreadWorker<T>::GetObjectFromQueueIn()
{
    T* f = nullptr;
    bool popped = false;
    {
        std::lock_guard<std::mutex> lock(mutexQueueIn);
        if (!queueIn.empty())
        {
            f = queueIn.front();
            queueIn.pop();
            countQueueIn--;
            popped = true;
        }
    }
    if (popped) {
        queueInNotFullCv.notify_one();
    }
    return f;
}

template<typename T>
void CThreadWorker<T>::GetQueueInSnapshotForInstrumentation(int* size, T** oldest)
{
    std::lock_guard<std::mutex> lock(mutexQueueIn);
    if (size) {
        *size = static_cast<int>(queueIn.size());
    }
    if (oldest) {
        *oldest = queueIn.empty() ? nullptr : queueIn.front();
    }
}

template<typename T>
T* CThreadWorker<T>::WaitForObjectFromQueueIn()
{
    std::unique_lock<std::mutex> lock(mutexQueueIn);
    queueInNotEmptyCv.wait(lock, [this]() {
        return !queueIn.empty() || stopRequested;
    });
    if (queueIn.empty()) {
        return nullptr;
    }
    T* f = queueIn.front();
    queueIn.pop();
    countQueueIn--;
    OnQueueInDequeued(f, countQueueIn);
    lock.unlock();
    queueInNotFullCv.notify_one();
    return f;
}

template<typename T>
void CThreadWorker<T>::DoStopThread()
{
    {
        std::lock_guard<std::mutex> lock(mutexQueueIn);
        stopRequested = true;
    }
    queueInNotEmptyCv.notify_all();
    queueInNotFullCv.notify_all();
}

template<typename T>
void CThreadWorker<T>::ThreadRunning()
{
    printf("Child Thread Start %d (%s)\n", id, threadName);

    {
        std::lock_guard<std::mutex> lock(mutexQueueIn);
        if (this->IsMachineOn()) {
            stopRequested = false;
        }
    }

    while (true)
    {
        T* f = this->WaitForObjectFromQueueIn();

        if (f)
        {
            if (this->WorkerFunction(f))
            {
                this->PutObjectToQueueOut(f);
            }
            myWork++;
        }
        else
        {
            // A flush tick: either an EnqueueFlushTick() marker reached the
            // front of the queue (FIFO, so everything ahead of it has been
            // processed), or the stop signal woke an empty queue.
            this->OnFlushTick();

            // If the machine is shutting down and the queue is empty, exit.
            if (!this->IsMachineOn()) {
                break;
            }
        }
    }
    printf("Child Thread DONE %d (%s)\n", id, threadName);
}
