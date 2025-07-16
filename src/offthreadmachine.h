// offthreadmachine.h – revised header to match new lifecycle logic
#pragma once

#include <atomic>

#if defined(__GNUC__)
#  include <pthread.h>
#  define THREAD_HANDLE pthread_t
#  define THREAD_FUNCTION void*
#else
#  include <windows.h>
#  define THREAD_HANDLE HANDLE
#  define THREAD_FUNCTION DWORD WINAPI
#endif

class COffThreadMachine {
public:
    explicit COffThreadMachine(const char *tName = nullptr);
    virtual ~COffThreadMachine();

    // optional: fix CPU affinity (‑1 ⇒ no affinity)
    void SetCPU(int cpu) { cpuToUse = cpu; }

    // start/stop helpers; returns 0 on success, EEXIST if already running
    int  StartThread(const char *tName = nullptr);
    void StopThread();

    // read‑only helpers
    bool IsMachineOn() const { return threadOn.load(std::memory_order_acquire); }

protected:
    char threadName[64]{}; // NUL‑terminated label for debugging / naming

private:
    static THREAD_FUNCTION MachineThread(void *);

    // derive these two in subclasses
    virtual void ThreadRunning() = 0; // main body
    virtual void DoStopThread();      // extra signal if needed

    // data
    std::atomic<bool> threadOn{false}; // true while thread should stay alive
    THREAD_HANDLE      threadHandle{}; // 0 / nullptr when not running
    int                cpuToUse = -1;  // 0‑based CPU#, ‑1 ⇒ no affinity
};
