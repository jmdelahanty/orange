#include "worker_entry_release.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct FakeWorkerEntryWorker {
    bool accept = true;
    int enqueue_attempts = 0;
    WORKER_ENTRY* last_entry = nullptr;

    bool PutObjectToQueueIn(WORKER_ENTRY* entry)
    {
        enqueue_attempts++;
        last_entry = entry;
        return accept;
    }
};

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_retain_from_positive_ref_count_increments()
{
    reset_worker_entry_release_diagnostics_for_tests();
    WORKER_ENTRY entry{};
    entry.ref_count.store(1, std::memory_order_release);

    require(
        retain_worker_entry(
            &entry,
            WorkerEntryReleaseContext{"2010096", "unit_test_retain"}),
        "retain from one should succeed");
    require(entry.ref_count.load(std::memory_order_acquire) == 2, "retain should increment ref count");
    require(
        worker_entry_retain_after_release_count().load(std::memory_order_acquire) == 0,
        "normal retain should not count as retain-after-release");
}

void test_retain_from_zero_ref_count_fails_without_increment()
{
    reset_worker_entry_release_diagnostics_for_tests();
    WORKER_ENTRY entry{};
    entry.ref_count.store(0, std::memory_order_release);
    entry.frame_id = 300;
    entry.recording_frame_id = 77;

    require(
        !retain_worker_entry(
            &entry,
            WorkerEntryReleaseContext{"2010096", "unit_test_retain_zero"}),
        "retain from zero should fail");
    require(entry.ref_count.load(std::memory_order_acquire) == 0, "retain from zero should not increment");
    require(
        worker_entry_retain_after_release_count().load(std::memory_order_acquire) == 1,
        "retain from zero should count as retain-after-release");
}

void test_retain_from_negative_ref_count_fails_without_increment()
{
    reset_worker_entry_release_diagnostics_for_tests();
    WORKER_ENTRY entry{};
    entry.ref_count.store(-2, std::memory_order_release);

    require(
        !retain_worker_entry(
            &entry,
            WorkerEntryReleaseContext{"2010096", "unit_test_retain_negative"}),
        "retain from negative ref count should fail");
    require(entry.ref_count.load(std::memory_order_acquire) == -2, "retain from negative should not increment");
    require(
        worker_entry_retain_after_release_count().load(std::memory_order_acquire) == 1,
        "retain from negative should count as retain-after-release");
}

void test_retain_and_enqueue_succeeds_when_worker_accepts()
{
    reset_worker_entry_release_diagnostics_for_tests();
    SafeQueue<WORKER_ENTRY*> recycle_queue;
    WORKER_ENTRY entry{};
    entry.ref_count.store(1, std::memory_order_release);
    FakeWorkerEntryWorker worker;
    bool enqueue_rejected = true;

    require(
        retain_and_enqueue_worker_entry(
            &worker,
            recycle_queue,
            &entry,
            WorkerEntryReleaseContext{"2010096", "unit_test_enqueue"},
            &enqueue_rejected),
        "retain-and-enqueue should succeed when worker accepts");
    require(!enqueue_rejected, "successful enqueue should not report rejection");
    require(worker.enqueue_attempts == 1, "successful enqueue should call worker once");
    require(worker.last_entry == &entry, "successful enqueue should pass entry to worker");
    require(entry.ref_count.load(std::memory_order_acquire) == 2, "successful enqueue should keep retained ref");

    WORKER_ENTRY* recycled = nullptr;
    require(!recycle_queue.pop(recycled), "successful enqueue should not recycle");
}

void test_retain_and_enqueue_releases_when_worker_rejects()
{
    reset_worker_entry_release_diagnostics_for_tests();
    SafeQueue<WORKER_ENTRY*> recycle_queue;
    WORKER_ENTRY entry{};
    entry.ref_count.store(1, std::memory_order_release);
    entry.gpu_direct_mode = false;
    FakeWorkerEntryWorker worker;
    worker.accept = false;
    bool enqueue_rejected = false;

    require(
        !retain_and_enqueue_worker_entry(
            &worker,
            recycle_queue,
            &entry,
            WorkerEntryReleaseContext{"2010096", "unit_test_enqueue_reject"},
            &enqueue_rejected),
        "retain-and-enqueue should fail when worker rejects");
    require(enqueue_rejected, "worker rejection should be reported");
    require(worker.enqueue_attempts == 1, "rejected enqueue should still call worker once");
    require(entry.ref_count.load(std::memory_order_acquire) == 1, "rejected enqueue should release retained ref");

    WORKER_ENTRY* recycled = nullptr;
    require(!recycle_queue.pop(recycled), "rejected fanout should not recycle while base ref remains");
    require(
        worker_entry_retain_after_release_count().load(std::memory_order_acquire) == 0,
        "worker rejection after successful retain should not count as retain-after-release");
}

void test_null_entry_is_noop()
{
    reset_worker_entry_release_diagnostics_for_tests();
    SafeQueue<WORKER_ENTRY*> recycle_queue;
    require(
        !release_worker_entry_to_recycle(recycle_queue, nullptr),
        "null entry should not recycle");
    require(
        worker_entry_release_double_release_count().load(std::memory_order_acquire) == 0,
        "null entry should not count as double release");
    require(
        worker_entry_release_underflow_count().load(std::memory_order_acquire) == 0,
        "null entry should not count as underflow");
}

void test_non_final_release_decrements_without_recycling()
{
    reset_worker_entry_release_diagnostics_for_tests();
    SafeQueue<WORKER_ENTRY*> recycle_queue;
    WORKER_ENTRY entry{};
    entry.ref_count.store(2, std::memory_order_release);
    entry.gpu_direct_mode = false;

    require(
        !release_worker_entry_to_recycle(recycle_queue, &entry),
        "first release should not recycle with one ref remaining");
    require(entry.ref_count.load(std::memory_order_acquire) == 1, "ref count should decrement to one");

    WORKER_ENTRY* recycled = nullptr;
    require(!recycle_queue.pop(recycled), "recycle queue should stay empty before final release");
    require(
        worker_entry_release_double_release_count().load(std::memory_order_acquire) == 0,
        "normal non-final release should not count as double release");
    require(
        worker_entry_release_underflow_count().load(std::memory_order_acquire) == 0,
        "normal non-final release should not count as underflow");
}

void test_final_release_recycles_exactly_once()
{
    reset_worker_entry_release_diagnostics_for_tests();
    SafeQueue<WORKER_ENTRY*> recycle_queue;
    WORKER_ENTRY entry{};
    entry.ref_count.store(1, std::memory_order_release);
    entry.gpu_direct_mode = false;

    require(
        release_worker_entry_to_recycle(recycle_queue, &entry),
        "final release should recycle");
    require(entry.ref_count.load(std::memory_order_acquire) == 0, "ref count should decrement to zero");
    WORKER_ENTRY* recycled = nullptr;
    require(recycle_queue.pop(recycled), "recycle queue should receive the entry");
    require(recycled == &entry, "recycled pointer should match released entry");
    require(!recycle_queue.pop(recycled), "entry should recycle exactly once");
    require(
        worker_entry_release_double_release_count().load(std::memory_order_acquire) == 0,
        "normal final release should not count as double release");
    require(
        worker_entry_release_underflow_count().load(std::memory_order_acquire) == 0,
        "normal final release should not count as underflow");
}

void test_last_reference_without_recycle_queue()
{
    reset_worker_entry_release_diagnostics_for_tests();
    WORKER_ENTRY entry{};
    entry.ref_count.store(1, std::memory_order_release);
    entry.gpu_direct_mode = false;

    require(
        release_worker_entry_to_recycle(nullptr, &entry),
        "final release without recycle queue should still report last reference");
    require(entry.ref_count.load(std::memory_order_acquire) == 0, "ref count should decrement to zero");
}

void test_double_release_after_final_release_does_not_enqueue_again()
{
    reset_worker_entry_release_diagnostics_for_tests();
    SafeQueue<WORKER_ENTRY*> recycle_queue;
    WORKER_ENTRY entry{};
    entry.ref_count.store(1, std::memory_order_release);
    entry.frame_id = 123;
    entry.recording_frame_id = 45;
    entry.camera_frame_id = 67;
    entry.gpu_direct_mode = false;

    require(
        release_worker_entry_to_recycle(recycle_queue, &entry),
        "first final release should recycle");
    require(
        !release_worker_entry_to_recycle(recycle_queue, &entry),
        "double release should not recycle");
    require(entry.ref_count.load(std::memory_order_acquire) == 0, "double release should leave ref count at zero");

    WORKER_ENTRY* recycled = nullptr;
    require(recycle_queue.pop(recycled), "first release should enqueue once");
    require(recycled == &entry, "recycled pointer should match entry");
    require(!recycle_queue.pop(recycled), "double release should not enqueue a second copy");
    require(
        worker_entry_release_double_release_count().load(std::memory_order_acquire) == 1,
        "double release counter should increment");
    require(
        worker_entry_release_underflow_count().load(std::memory_order_acquire) == 0,
        "double release at zero should not count as negative underflow");
}

void test_context_release_diagnostics_keep_counter_behavior()
{
    reset_worker_entry_release_diagnostics_for_tests();
    SafeQueue<WORKER_ENTRY*> recycle_queue;
    WORKER_ENTRY entry{};
    entry.ref_count.store(0, std::memory_order_release);
    entry.frame_id = 200;
    entry.recording_frame_id = 100;
    entry.camera_frame_id = 55;
    entry.gpu_direct_mode = false;

    require(
        !release_worker_entry_to_recycle(
            recycle_queue,
            &entry,
            WorkerEntryReleaseContext{"2010096", "unit_test_release"}),
        "context-bearing zero ref count release should not recycle");
    require(entry.ref_count.load(std::memory_order_acquire) == 0, "context release should not decrement zero");

    WORKER_ENTRY* recycled = nullptr;
    require(!recycle_queue.pop(recycled), "context-bearing double release should not enqueue");
    require(
        worker_entry_release_double_release_count().load(std::memory_order_acquire) == 1,
        "context-bearing double release should increment double release counter");
    require(
        worker_entry_release_underflow_count().load(std::memory_order_acquire) == 0,
        "context-bearing double release should not increment underflow counter");
}

void test_ref_count_zero_release_is_detected_without_decrement()
{
    reset_worker_entry_release_diagnostics_for_tests();
    SafeQueue<WORKER_ENTRY*> recycle_queue;
    WORKER_ENTRY entry{};
    entry.ref_count.store(0, std::memory_order_release);
    entry.gpu_direct_mode = false;

    require(
        !release_worker_entry_to_recycle(recycle_queue, &entry),
        "zero ref count release should not recycle");
    require(entry.ref_count.load(std::memory_order_acquire) == 0, "zero ref count should not decrement");

    WORKER_ENTRY* recycled = nullptr;
    require(!recycle_queue.pop(recycled), "zero ref count release should not enqueue");
    require(
        worker_entry_release_double_release_count().load(std::memory_order_acquire) == 1,
        "zero ref count release should count as double release");
    require(
        worker_entry_release_underflow_count().load(std::memory_order_acquire) == 0,
        "zero ref count release should not count as negative underflow");
}

void test_negative_ref_count_release_is_detected_without_decrement()
{
    reset_worker_entry_release_diagnostics_for_tests();
    SafeQueue<WORKER_ENTRY*> recycle_queue;
    WORKER_ENTRY entry{};
    entry.ref_count.store(-3, std::memory_order_release);
    entry.gpu_direct_mode = false;

    require(
        !release_worker_entry_to_recycle(recycle_queue, &entry),
        "negative ref count release should not recycle");
    require(entry.ref_count.load(std::memory_order_acquire) == -3, "negative ref count should not decrement further");

    WORKER_ENTRY* recycled = nullptr;
    require(!recycle_queue.pop(recycled), "negative ref count release should not enqueue");
    require(
        worker_entry_release_underflow_count().load(std::memory_order_acquire) == 1,
        "negative ref count release should count as underflow");
    require(
        worker_entry_release_double_release_count().load(std::memory_order_acquire) == 0,
        "negative ref count release should not count as zero double release");
}

}  // namespace

int main()
{
    try {
        test_retain_from_positive_ref_count_increments();
        test_retain_from_zero_ref_count_fails_without_increment();
        test_retain_from_negative_ref_count_fails_without_increment();
        test_retain_and_enqueue_succeeds_when_worker_accepts();
        test_retain_and_enqueue_releases_when_worker_rejects();
        test_null_entry_is_noop();
        test_non_final_release_decrements_without_recycling();
        test_final_release_recycles_exactly_once();
        test_last_reference_without_recycle_queue();
        test_double_release_after_final_release_does_not_enqueue_again();
        test_context_release_diagnostics_keep_counter_behavior();
        test_ref_count_zero_release_is_detected_without_decrement();
        test_negative_ref_count_release_is_detected_without_decrement();
    } catch (const std::exception& e) {
        std::cerr << "worker_entry_release_tests failed: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "worker_entry_release_tests passed" << std::endl;
    return 0;
}
