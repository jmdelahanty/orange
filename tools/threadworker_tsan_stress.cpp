// ThreadSanitizer stress for CThreadWorker.
//
// Reproduces the cross-thread topology used by the real pipeline:
//   - worker thread consumes items (mutates countQueueIn under mutexQueueIn)
//   - producer thread enqueues items (mutates countQueueIn under mutexQueueIn)
//   - monitor thread polls GetCountQueueIn()/GetCountQueueInMax() with no lock,
//     the same access pattern as drain detection in recording_ingress.cpp
//
// Normal usage (see docs/thread_sanitizer_runbook.md):
//   cmake --preset debug_tsan && cmake --build --preset debug_tsan
//   ctest --test-dir targets/debug_tsan -R threadworker_tsan_stress
//
// Standalone build (no CUDA/SDK deps):
//   g++ -fsanitize=thread -g -O1 -std=c++17 -Isrc \
//       tools/threadworker_tsan_stress.cpp src/offthreadmachine.cpp \
//       -o /tmp/threadworker_tsan_stress -lpthread
//   setarch $(uname -m) -R /tmp/threadworker_tsan_stress   # -R: see runbook
//
// A clean run exits 0 silently. A data race produces a TSan report on stderr
// and a non-zero exit code.

#include "threadworker.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

struct Item {
    int value = 0;
};

class CountingWorker : public CThreadWorker<Item> {
public:
    CountingWorker() : CThreadWorker<Item>("TsanStressWorker") {}

    std::atomic<long> processed{0};

protected:
    void OnFlushTick() override {}  // shutdown tick: nothing to flush

    bool WorkerFunction(Item*) override {
        processed.fetch_add(1, std::memory_order_relaxed);
        return false;  // items are pool-owned; nothing goes to queueOut
    }
};

}  // namespace

int main() {
    constexpr int kPoolSize = 64;
    constexpr auto kRunFor = std::chrono::milliseconds(300);

    static Item pool[kPoolSize];

    CountingWorker worker;
    worker.SetMaxQueueSize(16);
    worker.StartThread("TsanStressWorker");

    std::atomic<bool> stop{false};

    std::thread producer([&] {
        int i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            worker.PutObjectToQueueIn(&pool[i++ % kPoolSize]);
        }
    });

    // The racy reader: same pattern as IsDrained()-style checks that poll
    // queue depth from outside the worker thread.
    std::thread monitor([&] {
        long sink = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            sink += worker.GetCountQueueIn();
            sink += worker.GetCountQueueInMax();
            std::this_thread::yield();
        }
        std::printf("monitor sink=%ld (keeps reads alive)\n", sink);
    });

    std::this_thread::sleep_for(kRunFor);
    stop.store(true, std::memory_order_relaxed);

    producer.join();
    monitor.join();
    worker.StopThread();

    std::printf("processed=%ld items\n", worker.processed.load());
    return 0;
}
