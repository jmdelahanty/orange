#include "threadworker.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

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

}  // namespace

int main()
{
    try {
        test_enqueue_reports_true_while_running();
        test_enqueue_reports_false_after_stop();
        test_immediate_stop_after_start_rejects_enqueue();
        test_immediate_enqueue_after_restart_succeeds();
        test_full_queue_enqueue_unblocks_false_on_stop();
    } catch (const std::exception& e) {
        std::cerr << "threadworker_tests failed: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "threadworker_tests passed" << std::endl;
    return 0;
}
