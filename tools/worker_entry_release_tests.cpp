#include "worker_entry_release.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
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
        test_null_entry_is_noop();
        test_non_final_release_decrements_without_recycling();
        test_final_release_recycles_exactly_once();
        test_last_reference_without_recycle_queue();
        test_double_release_after_final_release_does_not_enqueue_again();
        test_ref_count_zero_release_is_detected_without_decrement();
        test_negative_ref_count_release_is_detected_without_decrement();
    } catch (const std::exception& e) {
        std::cerr << "worker_entry_release_tests failed: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "worker_entry_release_tests passed" << std::endl;
    return 0;
}
