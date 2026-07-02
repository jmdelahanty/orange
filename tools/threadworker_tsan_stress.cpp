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
#include "worker_entry_ownership_core.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

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

// --- Bounded output queue stress ---
//
// Topology mirrors a display/preview worker with a slow GUI consumer:
//   - producer thread leases pool entries and enqueues to the input queue
//   - worker thread passes every entry to a BOUNDED output queue; overflow
//     evicts the oldest entry, which is released back to the pool on the
//     worker thread (ReleaseDroppedQueueOutEntry)
//   - slow consumer thread drains the output queue and releases entries on
//     its own thread
//   - monitor thread polls the drop counter and queue depths with no lock
//
// Entries are refcounted like WORKER_ENTRY (worker_entry_ownership_core.h),
// so this also hammers the drop path against consumer releases.

struct PooledItem {
    std::atomic<int> ref_count{0};
    int value = 0;
};

class ItemPool {
public:
    explicit ItemPool(std::vector<PooledItem>& storage) {
        for (auto& item : storage) {
            free_.push_back(&item);
        }
        total_ = free_.size();
    }

    PooledItem* Acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_.empty()) {
            return nullptr;
        }
        PooledItem* item = free_.back();
        free_.pop_back();
        item->ref_count.store(1, std::memory_order_release);
        return item;
    }

    void Recycle(PooledItem* item) {
        std::lock_guard<std::mutex> lock(mutex_);
        free_.push_back(item);
    }

    // Releases one reference exactly as an output-queue consumer would; the
    // final release recycles the entry into the freelist.
    void ReleaseAsConsumer(PooledItem* item) {
        release_worker_entry_ref(
            item,
            WorkerEntryReleaseContext{"tsan", "bounded_out_stress"},
            [this](PooledItem* final_item) { Recycle(final_item); });
    }

    size_t free_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return free_.size();
    }

    size_t total() const { return total_; }

private:
    std::mutex mutex_;
    std::vector<PooledItem*> free_;
    size_t total_ = 0;
};

class BoundedOutWorker : public CThreadWorker<PooledItem> {
public:
    explicit BoundedOutWorker(ItemPool* pool)
        : CThreadWorker<PooledItem>("TsanBoundedOutWorker"), pool_(pool) {}

    std::atomic<long> processed{0};

protected:
    void OnFlushTick() override {}

    bool WorkerFunction(PooledItem*) override {
        processed.fetch_add(1, std::memory_order_relaxed);
        return true;  // every entry rides the bounded output queue
    }

    void ReleaseDroppedQueueOutEntry(PooledItem* item) override {
        pool_->ReleaseAsConsumer(item);
    }

private:
    ItemPool* pool_;
};

int run_input_queue_stress();
int run_bounded_output_queue_stress();

}  // namespace

int main() {
    const int input_stress = run_input_queue_stress();
    if (input_stress != 0) {
        return input_stress;
    }
    return run_bounded_output_queue_stress();
}

namespace {

int run_input_queue_stress() {
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

int run_bounded_output_queue_stress() {
    constexpr int kPoolSize = 64;
    constexpr int kQueueOutCap = 8;
    constexpr auto kRunFor = std::chrono::milliseconds(300);

    static std::vector<PooledItem> storage(kPoolSize);
    ItemPool pool(storage);

    BoundedOutWorker worker(&pool);
    worker.SetMaxQueueSize(16);
    worker.SetMaxQueueOutSize(kQueueOutCap);
    worker.StartThread("TsanBoundedOutWorker");

    std::atomic<bool> stop{false};

    // Fast producer: leases entries from the pool as fast as they come back.
    std::thread producer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            PooledItem* item = pool.Acquire();
            if (!item) {
                std::this_thread::yield();
                continue;
            }
            if (!worker.PutObjectToQueueIn(item)) {
                pool.ReleaseAsConsumer(item);  // rejected: give the lease back
            }
        }
    });

    // Slow consumer: drains the bounded output queue at a fraction of the
    // production rate, forcing steady drop-oldest evictions on the worker
    // thread while releases happen here.
    std::thread consumer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            PooledItem* item = worker.GetObjectFromQueueOut();
            if (item) {
                pool.ReleaseAsConsumer(item);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Lock-free pollers, as the GUI/instrumentation would run them.
    std::thread monitor([&] {
        long sink = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            sink += worker.GetCountQueueOut();
            sink += static_cast<long>(worker.GetCountQueueOutDropped());
            sink += worker.GetCountQueueOutSize();
            std::this_thread::yield();
        }
        std::printf("bounded-out monitor sink=%ld (keeps reads alive)\n", sink);
    });

    std::this_thread::sleep_for(kRunFor);
    stop.store(true, std::memory_order_relaxed);

    producer.join();
    consumer.join();
    monitor.join();
    worker.StopThread();

    // Drain the residual backlog exactly as the consumer would.
    std::vector<PooledItem*> leftovers;
    worker.GetObjectsFromQueueOut(leftovers);
    for (PooledItem* item : leftovers) {
        pool.ReleaseAsConsumer(item);
    }

    const long processed = worker.processed.load();
    const unsigned long long dropped =
        static_cast<unsigned long long>(worker.GetCountQueueOutDropped());
    std::printf("bounded-out processed=%ld dropped=%llu pool_free=%zu/%zu\n",
                processed, dropped, pool.free_count(), pool.total());

    if (worker.GetCountQueueOutSize() != 0) {
        std::fprintf(stderr, "bounded-out stress: output queue not drained\n");
        return 1;
    }
    if (dropped == 0) {
        std::fprintf(stderr,
                     "bounded-out stress: expected overflow drops with a slow consumer\n");
        return 1;
    }
    // Every pool entry must be back in the freelist: dropped entries were
    // released exactly like consumed ones, so nothing leaked.
    if (pool.free_count() != pool.total()) {
        std::fprintf(stderr, "bounded-out stress: pool leak (%zu/%zu free)\n",
                     pool.free_count(), pool.total());
        return 1;
    }
    return 0;
}

}  // namespace
