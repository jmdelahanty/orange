// src/threadworker.h

#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>
#include "offthreadmachine.h"
#include "genericmutex.h"
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

    void SetID(int i) { id = i; }
    int GetID() const { return id; }

    void Reset();
    int GetMyWork() const { return myWork; }

    // Type-safe methods using templates
    void PutObjectToQueueIn(T* f);
    void GetObjectsFromQueueOut(std::vector<T*>& items);
    T* GetObjectFromQueueOut();
    void PutObjectToQueueOut(T* f);
    T* GetObjectFromQueueIn();
    void SetMaxQueueSize(int size) { maxQueueSize = size; }

    int GetCountQueueInSize();
    int GetCountQueueOutSize();
    int GetCountQueueIn() const { return countQueueIn; }
    int GetCountQueueOut() const { return countQueueOut; }
    int GetCountInTotal() const { return countInTotal; }
    int GetCountOutTotal() const { return countOutTotal; }
    int GetCountQueueInMax() const { return countQueueInMax; }

    void SetInterval(unsigned int i) {
        interval = i;
#ifdef _WIN32
        intervalMilliSeconds = interval / 1000;
        if(interval % 1000) intervalMilliSeconds++;
#endif
    }

protected:
    // The main worker function that derived classes must implement.
    // It now returns a bool to indicate if the item should be passed to the output queue.
    virtual bool WorkerFunction(T* f) = 0;

    // This function is called when the worker is reset.
    virtual void WorkerReset() {}
    virtual void OnQueueInEnqueued(T*, int) {}
    virtual void OnQueueInDequeued(T*, int) {}

    void GetQueueInSnapshotForInstrumentation(int* size, T** oldest);

private:
    // This overrides the pure virtual function in the base class COffThreadMachine.
    void ThreadRunning() override;
    void DoStopThread() override;

    void ResetInner();
    T* WaitForObjectFromQueueIn();

private:
    int id = 0;
    std::mutex mutexQueueIn;
    std::condition_variable queueInNotEmptyCv;
    std::condition_variable queueInNotFullCv;
    std::queue<T*> queueIn;
    CGenericMutex mutexQueueOut;
    std::queue<T*> queueOut;
    bool stopRequested = false;

    // Tracing counts
    int countQueueIn = 0;
    int countQueueOut = 0;
    int countInTotal = 0;
    int countOutTotal = 0;
    int countQueueInMax = 0;
    int maxQueueSize = 40; // Default max queue size

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
        stopRequested = false;
    }
    myWork = 0;
    countQueueIn = 0;
    countQueueOut = 0;
    countInTotal = 0;
    countOutTotal = 0;
    countQueueInMax = 0;

    // Safely clear the queues
    {
        std::lock_guard<std::mutex> lock(mutexQueueIn);
        std::queue<T*> emptyIn;
        std::swap(queueIn, emptyIn);
    }
    queueInNotEmptyCv.notify_all();
    queueInNotFullCv.notify_all();

    mutexQueueOut.Lock();
    std::queue<T*> emptyOut;
    std::swap(queueOut, emptyOut);
    mutexQueueOut.Unlock();
}

template<typename T>
void CThreadWorker<T>::PutObjectToQueueIn(T* f)
{
    std::unique_lock<std::mutex> lock(mutexQueueIn);
    queueInNotFullCv.wait(lock, [this]() {
        return queueIn.size() < static_cast<size_t>(maxQueueSize) || stopRequested;
    });
    if (stopRequested) {
        return;
    }
    queueIn.push(f);
    countQueueIn++;
    countInTotal++;
    if (countQueueInMax < countQueueIn) {
        countQueueInMax = countQueueIn;
    }
    OnQueueInEnqueued(f, countQueueIn);
    lock.unlock();
    queueInNotEmptyCv.notify_one();
}

template<typename T>
void CThreadWorker<T>::GetObjectsFromQueueOut(std::vector<T*>& items)
{
    mutexQueueOut.Lock();
    items.clear();
    while (!queueOut.empty())
    {
        items.push_back(queueOut.front());
        queueOut.pop();
    }
    countOutTotal += static_cast<int>(items.size());
    countQueueOut = 0; // The queue is now empty
    mutexQueueOut.Unlock();
}

template<typename T>
T* CThreadWorker<T>::GetObjectFromQueueOut()
{
    T* f = nullptr;
    mutexQueueOut.Lock();
    if (!queueOut.empty())
    {
        f = queueOut.front();
        queueOut.pop();
        countQueueOut--;
    }
    mutexQueueOut.Unlock();
    return f;
}


template<typename T>
void CThreadWorker<T>::PutObjectToQueueOut(T* f)
{
    mutexQueueOut.Lock();
    queueOut.push(f);
    countQueueOut++;
    mutexQueueOut.Unlock();
}

template<typename T>
int CThreadWorker<T>::GetCountQueueInSize()
{
    int size = -1;
    mutexQueueIn.lock();
    size = static_cast<int>(queueIn.size());
    mutexQueueIn.unlock();
    return size;
}

template<typename T>
int CThreadWorker<T>::GetCountQueueOutSize()
{
    int size = -1;
    mutexQueueOut.Lock();
    size = static_cast<int>(queueOut.size());
    mutexQueueOut.Unlock();
    return size;
}

template<typename T>
T* CThreadWorker<T>::GetObjectFromQueueIn()
{
    T* f = nullptr;
    mutexQueueIn.lock();
    if (!queueIn.empty())
    {
        f = queueIn.front();
        queueIn.pop();
        countQueueIn--;
    }
    mutexQueueIn.unlock();
    if (f) {
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
        stopRequested = false;
    }

    while (true)
    {
        T* f = this->WaitForObjectFromQueueIn();

        // The worker function is now called even with a nullptr.
        // It is the responsibility of the derived class to handle the nullptr case.
        if (this->WorkerFunction(f) && f)
        {
            this->PutObjectToQueueOut(f);
        }

        if (f)
        {
            myWork++;
        }
        else
        {
            // If the machine is shutting down and the queue is empty, we can exit.
            if (!this->IsMachineOn()) {
                break;
            }
        }
    }
    printf("Child Thread DONE %d (%s)\n", id, threadName);
}
