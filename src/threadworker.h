#pragma once

#include <queue>
#include <vector>
#include "offthreadmachine.h"
#include "genericmutex.h"
#include <cstdio>
#include "thread.h" // For SafeQueue

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

    // This method allows the main application to tell the worker where to get its input from.
    void SetInputQueue(SafeQueue<T*>* input_queue) {
        m_input_queue = input_queue;
    }

    // These methods now operate on the externally-provided input queue.
    void PutObjectToQueueIn(T* f);
    T* GetObjectFromQueueIn();

    // Output queue methods remain the same as the worker owns its output.
    void GetObjectsFromQueueOut(std::vector<T*>& items);
    T* GetObjectFromQueueOut();
    void PutObjectToQueueOut(T* f);

    // Getters for stats
    int GetCountQueueInSize();
    int GetCountQueueOutSize();
    int GetCountInTotal() const { return countInTotal; }
    int GetCountOutTotal() const { return countOutTotal; }

    void SetInterval(unsigned int i) {
        interval = i;
#ifdef _WIN32
        intervalMilliSeconds = interval / 1000;
        if(interval % 1000) intervalMilliSeconds++;
#endif
    }

protected:
    virtual bool WorkerFunction(T* f) = 0;
    virtual void WorkerReset() {}

private:
    void ThreadRunning() override;
    void ResetInner();

private:
    int id = 0;
    
    // The worker no longer owns its input queue. It gets a pointer to one.
    SafeQueue<T*>* m_input_queue = nullptr;
    
    // The worker still owns its output queue.
    CGenericMutex mutexQueueOut;
    std::queue<T*> queueOut;

    // Tracing counts
    int countInTotal = 0;
    int countOutTotal = 0;
    
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
      countInTotal(0),
      countOutTotal(0),
      myWork(0),
      interval(1000) // Start with a 1ms sleep interval
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
    myWork = 0;
    countInTotal = 0;
    countOutTotal = 0;

    // We no longer manage the input queue's memory here
    mutexQueueOut.Lock();
    std::queue<T*> emptyOut;
    std::swap(queueOut, emptyOut);
    mutexQueueOut.Unlock();
}

template<typename T>
void CThreadWorker<T>::PutObjectToQueueIn(T* f)
{
    if (m_input_queue) {
        m_input_queue->push(f);
        countInTotal++;
    }
}

template<typename T>
T* CThreadWorker<T>::GetObjectFromQueueIn()
{
    T* f = nullptr;
    if (m_input_queue) {
        m_input_queue->pop(f);
    }
    return f;
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
    }
    mutexQueueOut.Unlock();
    return f;
}


template<typename T>
void CThreadWorker<T>::PutObjectToQueueOut(T* f)
{
    mutexQueueOut.Lock();
    queueOut.push(f);
    mutexQueueOut.Unlock();
}

template<typename T>
int CThreadWorker<T>::GetCountQueueInSize()
{
    if (m_input_queue) {
        // This function will need to be added to your SafeQueue implementation
        // return m_input_queue->size(); 
        return 0; // Placeholder
    }
    return 0;
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
void CThreadWorker<T>::ThreadRunning()
{
    printf("Child Thread Start %d\n", id);
    while (this->IsMachineOn())
    {
        if (m_input_queue) { // Check if the input queue has been set
            T* f = nullptr;
            if (m_input_queue->pop(f)) {
                if (f) {
                    if (this->WorkerFunction(f)) {
                        this->PutObjectToQueueOut(f);
                    }
                    myWork++;
                }
            } else {
                usleep(interval); // Sleep if the queue is empty
            }
        } else {
            // If no input queue is set, just sleep.
            usleep(interval * 10);
        }
    }

    // Process remaining items in the input queue after thread is stopped
    if (m_input_queue) {
        while (true) {
            T* f = nullptr;
            if (m_input_queue->pop(f)) {
                if (f) {
                    if (this->WorkerFunction(f)) {
                         this->PutObjectToQueueOut(f);
                    }
                    myWork++;
                }
            } else {
                break; // Queue is empty
            }
        }
    }
    printf("Child Thread DONE %d\n", id);
}