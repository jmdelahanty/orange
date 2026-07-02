#include "threadworker.h"
#include "worker_entry_ownership_core.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class IntWorker final : public CThreadWorker<int> {
public:
    IntWorker()
        : CThreadWorker<int>("IntWorker")
    {
    }

    int processed() const
    {
        return processed_.load(std::memory_order_acquire);
    }

private:
    bool WorkerFunction(int* item) override
    {
        if (item) {
            processed_.fetch_add(1, std::memory_order_release);
        }
        return false;
    }

    std::atomic<int> processed_{0};
};

class BlockingWorker final : public CThreadWorker<int> {
public:
    BlockingWorker()
        : CThreadWorker<int>("BlockingWorker")
    {
        SetMaxQueueSize(1);
    }

    int entered() const
    {
        return entered_.load(std::memory_order_acquire);
    }

    int processed() const
    {
        return processed_.load(std::memory_order_acquire);
    }

private:
    bool WorkerFunction(int* item) override
    {
        if (item) {
            entered_.fetch_add(1, std::memory_order_release);
            while (IsMachineOn()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            processed_.fetch_add(1, std::memory_order_release);
        }
        return false;
    }

    std::atomic<int> entered_{0};
    std::atomic<int> processed_{0};
};

void test_enqueue_reports_true_while_running()
{
    IntWorker worker;
    require(worker.StartThread() == 0, "worker should start");
    int value = 1;
    require(worker.PutObjectToQueueIn(&value), "enqueue should succeed while worker is running");

    for (int i = 0; i < 100 && worker.processed() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(worker.processed() == 1, "worker should process one item");
    worker.StopThread();
}

void test_enqueue_reports_false_after_stop()
{
    IntWorker worker;
    require(worker.StartThread() == 0, "worker should start");
    int warmup = 1;
    require(worker.PutObjectToQueueIn(&warmup), "warmup enqueue should succeed");
    for (int i = 0; i < 100 && worker.processed() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(worker.processed() == 1, "warmup item should prove worker entered its loop");
    worker.StopThread();

    int value = 2;
    require(!worker.PutObjectToQueueIn(&value), "enqueue should be rejected after stop");
    require(worker.GetCountQueueInSize() == 0, "rejected item should not remain queued");
}

void test_immediate_stop_after_start_rejects_enqueue()
{
    for (int i = 0; i < 25; ++i) {
        IntWorker worker;
        require(worker.StartThread() == 0, "worker should start");
        worker.StopThread();

        int value = 3;
        require(
            !worker.PutObjectToQueueIn(&value),
            "enqueue should be rejected after immediate start/stop");
    }
}

void test_immediate_enqueue_after_restart_succeeds()
{
    IntWorker worker;
    require(worker.StartThread() == 0, "worker should start");
    worker.StopThread();

    require(worker.StartThread() == 0, "worker should restart");
    int value = 4;
    require(
        worker.PutObjectToQueueIn(&value),
        "enqueue should succeed immediately after restart");
    for (int i = 0; i < 100 && worker.processed() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(worker.processed() == 1, "restarted worker should process one item");
    worker.StopThread();
}

void test_full_queue_enqueue_unblocks_false_on_stop()
{
    BlockingWorker worker;
    require(worker.StartThread() == 0, "blocking worker should start");

    int first = 1;
    require(worker.PutObjectToQueueIn(&first), "first enqueue should succeed");
    for (int i = 0; i < 100 && worker.entered() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(worker.entered() == 1, "worker should be blocked on first item");

    int second = 2;
    require(worker.PutObjectToQueueIn(&second), "second enqueue should fill the queue");

    std::atomic<bool> enqueue_returned{false};
    std::atomic<bool> enqueue_result{true};
    int third = 3;
    std::thread producer([&]() {
        enqueue_result.store(worker.PutObjectToQueueIn(&third), std::memory_order_release);
        enqueue_returned.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(
        !enqueue_returned.load(std::memory_order_acquire),
        "third enqueue should wait while queue is full");

    worker.StopThread();
    producer.join();

    require(enqueue_returned.load(std::memory_order_acquire), "blocked enqueue should return");
    require(!enqueue_result.load(std::memory_order_acquire), "blocked enqueue should reject on stop");
    require(worker.GetCountQueueInSize() == 0, "stopped worker should drain queued items");
    require(worker.processed() == 2, "worker should process queued items during shutdown");
}

void test_manual_pop_of_null_flush_tick_wakes_blocked_enqueue()
{
    IntWorker worker;
    worker.SetMaxQueueSize(1);

    require(worker.PutObjectToQueueIn(nullptr), "flush tick should enqueue");
    require(worker.GetCountQueueInSize() == 1, "flush tick should occupy one queue slot");

    int value = 5;
    std::atomic<bool> enqueue_returned{false};
    std::atomic<bool> enqueue_result{false};
    std::thread producer([&]() {
        enqueue_result.store(worker.PutObjectToQueueIn(&value), std::memory_order_release);
        enqueue_returned.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(
        !enqueue_returned.load(std::memory_order_acquire),
        "producer should wait while flush tick fills the queue");

    require(
        worker.GetObjectFromQueueIn() == nullptr,
        "manual pop should return the queued flush tick");
    require(worker.GetCountQueueInSize() == 0, "manual pop should free the queue slot");

    bool woke_without_reset = false;
    for (int i = 0; i < 100; ++i) {
        if (enqueue_returned.load(std::memory_order_acquire)) {
            woke_without_reset = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!woke_without_reset) {
        worker.Reset();  // unblock the producer so the test can fail without hanging
    }
    producer.join();

    require(
        woke_without_reset,
        "popping a null flush tick should notify producers waiting for queue space");
    require(enqueue_result.load(std::memory_order_acquire), "producer enqueue should succeed");
    require(worker.GetObjectFromQueueIn() == &value, "manual pop should retrieve producer item");
    require(worker.GetCountQueueInSize() == 0, "queue should be empty after cleanup");
}

// --- Bounded output queue tests ---

// Pool-backed, refcounted entry shaped like the real WORKER_ENTRY ownership
// model (worker_entry_ownership_core.h): the last release recycles the entry
// back to a freelist ("pool").
struct PoolEntry {
    std::atomic<int> ref_count{0};
    int value = 0;
};

class BoundedOutWorker final : public CThreadWorker<PoolEntry> {
public:
    BoundedOutWorker()
        : CThreadWorker<PoolEntry>("BoundedOutWorker")
    {
    }

    // Releases an entry exactly as an output-queue consumer would: drop the
    // refcount; the final release recycles the entry to the pool.
    void ReleaseAsConsumer(PoolEntry* entry)
    {
        release_worker_entry_ref(
            entry,
            WorkerEntryReleaseContext{"testcam", "bounded_out"},
            [this](PoolEntry* final_entry) {
                std::lock_guard<std::mutex> lock(pool_mutex_);
                pool_.push_back(final_entry);
            });
    }

    std::vector<PoolEntry*> pooled_entries()
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        return pool_;
    }

    int processed() const
    {
        return processed_.load(std::memory_order_acquire);
    }

protected:
    bool WorkerFunction(PoolEntry* item) override
    {
        processed_.fetch_add(1, std::memory_order_release);
        return item != nullptr;  // pass every entry to the output queue
    }

    // Drop path: must release the dropped entry exactly as the consumer
    // would have, or the pool leaks.
    void ReleaseDroppedQueueOutEntry(PoolEntry* entry) override
    {
        ReleaseAsConsumer(entry);
    }

private:
    std::mutex pool_mutex_;
    std::vector<PoolEntry*> pool_;
    std::atomic<int> processed_{0};
};

void test_queue_out_unbounded_by_default()
{
    BoundedOutWorker worker;
    std::vector<PoolEntry> entries(100);
    for (auto& entry : entries) {
        entry.ref_count.store(1, std::memory_order_release);
        worker.PutObjectToQueueOut(&entry);
    }
    require(worker.GetCountQueueOutSize() == 100,
            "default (unbounded) output queue should keep every entry");
    require(worker.GetCountQueueOutDropped() == 0,
            "unbounded output queue should never drop");
    PoolEntry* popped = nullptr;
    while ((popped = worker.GetObjectFromQueueOut()) != nullptr) {
        worker.ReleaseAsConsumer(popped);
    }
}

void test_queue_out_bound_respected_and_drops_oldest()
{
    BoundedOutWorker worker;
    worker.SetMaxQueueOutSize(3);

    std::vector<PoolEntry> entries(5);
    for (int i = 0; i < 5; ++i) {
        entries[i].value = i;
        entries[i].ref_count.store(1, std::memory_order_release);
        worker.PutObjectToQueueOut(&entries[i]);
        require(worker.GetCountQueueOutSize() <= 3,
                "bounded output queue should never exceed its cap");
    }

    require(worker.GetCountQueueOutSize() == 3, "queue should sit at the cap");
    require(worker.GetCountQueueOut() == 3, "countQueueOut should match the cap");
    require(worker.GetCountQueueOutDropped() == 2, "two overflow drops expected");

    // Drop-oldest: entries 0 and 1 evicted, 2..4 retained in FIFO order.
    require(worker.GetObjectFromQueueOut() == &entries[2], "oldest survivor should be entry 2");
    require(worker.GetObjectFromQueueOut() == &entries[3], "next survivor should be entry 3");
    require(worker.GetObjectFromQueueOut() == &entries[4], "newest survivor should be entry 4");
    require(worker.GetObjectFromQueueOut() == nullptr, "queue should now be empty");

    const auto pooled = worker.pooled_entries();
    require(pooled.size() == 2, "both dropped entries should be recycled to the pool");
    require(pooled[0] == &entries[0] && pooled[1] == &entries[1],
            "dropped entries should be recycled in eviction order");
    require(entries[0].ref_count.load(std::memory_order_acquire) == 0 &&
                entries[1].ref_count.load(std::memory_order_acquire) == 0,
            "dropped entries should have fully released refcounts");

    // Survivors are released by the consumer.
    for (int i = 2; i < 5; ++i) {
        worker.ReleaseAsConsumer(&entries[i]);
    }
    require(worker.pooled_entries().size() == 5, "all entries should end up back in the pool");
}

void test_queue_out_shrinking_bound_trims_backlog()
{
    BoundedOutWorker worker;
    std::vector<PoolEntry> entries(5);
    for (auto& entry : entries) {
        entry.ref_count.store(1, std::memory_order_release);
        worker.PutObjectToQueueOut(&entry);
    }

    worker.SetMaxQueueOutSize(2);
    require(worker.GetCountQueueOutSize() == 2, "shrinking the bound should trim the backlog");
    require(worker.GetCountQueueOutDropped() == 3, "trimmed entries should count as drops");

    const auto pooled = worker.pooled_entries();
    require(pooled.size() == 3, "trimmed entries should be recycled to the pool");
    require(pooled[0] == &entries[0] && pooled[1] == &entries[1] && pooled[2] == &entries[2],
            "trim should evict the oldest entries first");

    require(worker.GetObjectFromQueueOut() == &entries[3], "entry 3 should survive the trim");
    require(worker.GetObjectFromQueueOut() == &entries[4], "entry 4 should survive the trim");
    worker.ReleaseAsConsumer(&entries[3]);
    worker.ReleaseAsConsumer(&entries[4]);
}

void test_queue_out_reset_releases_entries()
{
    BoundedOutWorker worker;
    std::vector<PoolEntry> entries(3);
    for (auto& entry : entries) {
        entry.ref_count.store(1, std::memory_order_release);
        worker.PutObjectToQueueOut(&entry);
    }

    worker.Reset();
    require(worker.GetCountQueueOutSize() == 0, "reset should clear the output queue");
    require(worker.GetCountQueueOutDropped() == 0, "reset should not count as overflow drops");
    require(worker.pooled_entries().size() == 3,
            "reset should release cleared output entries back to the pool");
    for (auto& entry : entries) {
        require(entry.ref_count.load(std::memory_order_acquire) == 0,
                "reset-released entries should have fully released refcounts");
    }
}

void test_queue_out_bound_through_worker_thread()
{
    constexpr int kEntries = 12;
    constexpr int kCap = 4;

    BoundedOutWorker worker;
    worker.SetMaxQueueOutSize(kCap);
    require(worker.StartThread() == 0, "bounded-out worker should start");

    std::vector<PoolEntry> entries(kEntries);
    for (int i = 0; i < kEntries; ++i) {
        entries[i].value = i;
        entries[i].ref_count.store(1, std::memory_order_release);
        require(worker.PutObjectToQueueIn(&entries[i]), "input enqueue should succeed");
    }

    for (int i = 0; i < 1000 && worker.processed() < kEntries; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(worker.processed() == kEntries, "worker should process every input entry");
    worker.StopThread();

    require(worker.GetCountQueueOutSize() == kCap,
            "output queue should hold exactly the cap after overflow");
    require(worker.GetCountQueueOutDropped() ==
                static_cast<uint64_t>(kEntries - kCap),
            "drop counter should equal the overflow amount");

    // Drain survivors as the consumer would.
    std::vector<PoolEntry*> survivors;
    worker.GetObjectsFromQueueOut(survivors);
    require(static_cast<int>(survivors.size()) == kCap, "drain should yield the capped backlog");
    for (PoolEntry* entry : survivors) {
        worker.ReleaseAsConsumer(entry);
    }

    // Every entry (dropped or consumed) must be back in the pool: no leaks.
    require(worker.pooled_entries().size() == static_cast<size_t>(kEntries),
            "all entries should return to the pool");
    for (auto& entry : entries) {
        require(entry.ref_count.load(std::memory_order_acquire) == 0,
                "every entry should have a fully released refcount");
    }
}

}  // namespace

int main()
{
    try {
        test_enqueue_reports_true_while_running();
        test_enqueue_reports_false_after_stop();
        test_immediate_stop_after_start_rejects_enqueue();
        test_immediate_enqueue_after_restart_succeeds();
        test_full_queue_enqueue_unblocks_false_on_stop();
        test_manual_pop_of_null_flush_tick_wakes_blocked_enqueue();
        test_queue_out_unbounded_by_default();
        test_queue_out_bound_respected_and_drops_oldest();
        test_queue_out_shrinking_bound_trims_backlog();
        test_queue_out_reset_releases_entries();
        test_queue_out_bound_through_worker_thread();
    } catch (const std::exception& e) {
        std::cerr << "threadworker_tests failed: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "threadworker_tests passed" << std::endl;
    return 0;
}
