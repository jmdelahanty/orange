// offthreadmachine.h – revised header to match new lifecycle logic
#pragma once

#include <atomic>
#include <mutex>
#include <string>

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

    // True once the worker thread has caught a fatal exception (e.g. a CUDA
    // error surfaced by CHECK()). The thread never calls exit(); it logs, sets
    // this flag and keeps shutdown/drain paths functional so recordings can
    // be finalized.
    bool HasFatalError() const { return fatalError.load(std::memory_order_acquire); }

    // The first fatal exception's message (with a wall-clock timestamp
    // appended). Empty until HasFatalError() is true. First error wins;
    // later exceptions only bump GetFatalErrorCount(). Safe from any thread.
    std::string GetFatalErrorMessage() const;

    // Monotonic count of fatal exceptions noted on the worker thread
    // (0 == healthy). Never resets for the lifetime of the object.
    int GetFatalErrorCount() const {
        return fatalErrorCount.load(std::memory_order_relaxed);
    }

    // NUL-terminated label passed to the constructor / StartThread().
    const char *GetThreadName() const { return threadName; }

protected:
    // Record a fatal exception observed on the worker thread: log it with the
    // worker's name (rate-limited) and latch the fatal-error flag. Never throws.
    void NoteWorkerException(const char *what) noexcept;

    char threadName[64]{}; // NUL‑terminated label for debugging / naming

private:
    static THREAD_FUNCTION MachineThread(void *);

    // derive these two in subclasses
    virtual void ThreadRunning() = 0; // main body
    virtual void DoStopThread();      // extra signal if needed

    // data
    std::atomic<bool> threadOn{false}; // true while thread should stay alive
    std::atomic<bool> fatalError{false}; // latched by NoteWorkerException
    std::atomic<int>  fatalErrorCount{0}; // total exceptions noted; also
                                          // rate-limits fatal-error logging
    mutable std::mutex fatalErrorMutex;   // guards fatalErrorMessage
    std::string fatalErrorMessage;        // first exception's text; written
                                          // once before fatalError is set
    THREAD_HANDLE      threadHandle{}; // 0 / nullptr when not running
    int                cpuToUse = -1;  // 0‑based CPU#, ‑1 ⇒ no affinity
};
