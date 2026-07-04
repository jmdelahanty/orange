// offthreadmachine.cpp – revised thread‑lifecycle handling
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <exception>
#include "offthreadmachine.h"

COffThreadMachine::COffThreadMachine(const char *tName)
    : threadOn(0), threadHandle(0), cpuToUse(-1) {
  if (tName) {
    strncpy(threadName, tName, 63);
    threadName[63] = 0; // ensure null‑termination
  } else {
    threadName[0] = 0;
  }
}

COffThreadMachine::~COffThreadMachine() {
  StopThread();
#ifndef NDEBUG
  if (threadHandle) {
    fprintf(stderr,
            "[BUG] OffThreadMachine %s destroyed while thread still joinable\n",
            threadName);
  }
#endif
}

/*
 * Return 0 on success, EEXIST if already started, ECHILD on create error.
 */
int COffThreadMachine::StartThread(const char *tName) {
  if (threadHandle)
    return EEXIST;

  // Update name if caller provided a new one.
  if (tName) {
    strncpy(threadName, tName, sizeof(threadName) - 1);
    threadName[sizeof(threadName) - 1] = 0;
  }

  threadOn = 1;

#if defined(__GNUC__)
  if (pthread_create(&threadHandle, nullptr, MachineThread, (void *)this) != 0) {
    threadOn = 0;
    return ECHILD;
  }

  // POSIX thread naming (limited to 15 chars + null).
  if (threadName[0] != '\0') {
    char truncated[16];
    strncpy(truncated, threadName, 15);
    truncated[15] = 0;
    pthread_setname_np(threadHandle, truncated);
  }
#else // Windows
  if (!(threadHandle =
            CreateThread(nullptr, 0, MachineThread, (void *)this, 0, nullptr))) {
    threadOn = 0;
    return ECHILD;
  }
  // TODO: SetThreadDescription(threadHandle, …) on Win10+
#endif

  return 0;
}

void COffThreadMachine::StopThread() {
  if (!threadHandle)
    return; // nothing to do – already joined / never started

  // Signal the worker to exit. This must run even if the newly-created thread
  // has not yet entered MachineThread.
  threadOn = 0;
  DoStopThread();

#if defined(__GNUC__)
  pthread_join(threadHandle, nullptr);
#else
  WaitForSingleObject(threadHandle, INFINITE);
  CloseHandle(threadHandle);
#endif

  threadHandle = 0;
  threadOn = 0;
}

void COffThreadMachine::DoStopThread() {
  // Default implementation does nothing – override in derived classes if
  // additional stop signalling is required (e.g., pushing exit tokens into
  // queues).
}

std::string COffThreadMachine::GetFatalErrorMessage() const {
  // Fast path: lock-free and allocation-free while healthy (SSO empty string).
  if (!fatalError.load(std::memory_order_acquire))
    return std::string();
  std::lock_guard<std::mutex> lock(fatalErrorMutex);
  return fatalErrorMessage;
}

void COffThreadMachine::NoteWorkerException(const char *what) noexcept {
  const int count =
      fatalErrorCount.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count == 1) {
    // First error wins: record the message (with a wall-clock timestamp)
    // *before* publishing the latch below, so a reader that observes
    // HasFatalError()==true (acquire) also observes the completed message
    // (release). The message is never modified again after this point; the
    // mutex additionally makes the string read/write itself race-free.
    char stamp[32];
    stamp[0] = 0;
    const time_t now = time(nullptr);
    struct tm tm_buf {};
#if defined(__GNUC__)
    if (localtime_r(&now, &tm_buf))
      strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf);
#else
    if (localtime_s(&tm_buf, &now) == 0)
      strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf);
#endif
    try {
      std::lock_guard<std::mutex> lock(fatalErrorMutex);
      fatalErrorMessage = what ? what : "unknown";
      if (stamp[0]) {
        fatalErrorMessage += " (at ";
        fatalErrorMessage += stamp;
        fatalErrorMessage += ")";
      }
    } catch (...) {
      // bad_alloc while already handling a failure: keep the latch usable,
      // drop the message text (GetFatalErrorMessage() returns "").
    }
  }
  fatalError.store(true, std::memory_order_release);
  if (count <= 5) {
    fprintf(stderr, "[%s] worker thread exception%s: %s\n", threadName,
            count == 5 ? " (suppressing further reports)" : "",
            what ? what : "unknown");
  }
}

THREAD_FUNCTION COffThreadMachine::MachineThread(void *arg) {
  auto *self = static_cast<COffThreadMachine *>(arg);

  // Optional CPU affinity.
  if (self->cpuToUse >= 0) {
#if defined(__GNUC__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(self->cpuToUse, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#else
    SetThreadAffinityMask(GetCurrentThread(),
                          (static_cast<unsigned long long>(1) << self->cpuToUse));
#endif
  }

  // Last line of defence: an exception escaping a thread body would call
  // std::terminate and kill the whole process (no NVENC flush, no MP4
  // trailer). Log it, latch the fatal-error flag, and let the thread exit
  // cleanly instead. DoStopThread() unblocks any producers waiting on this
  // worker's queues so the pipeline can drain.
  try {
    self->ThreadRunning();
  } catch (const std::exception &e) {
    self->NoteWorkerException(e.what());
    self->DoStopThread();
  } catch (...) {
    self->NoteWorkerException("non-std exception");
    self->DoStopThread();
  }
  self->threadOn = 0; // mark finished before exiting
  return 0;
}
