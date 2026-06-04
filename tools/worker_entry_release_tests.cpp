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
    SafeQueue<WORKER_ENTRY*> recycle_queue;
    require(
        !release_worker_entry_to_recycle(recycle_queue, nullptr),
        "null entry should not recycle");
}

void test_recycles_only_on_last_reference()
{
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
        release_worker_entry_to_recycle(recycle_queue, &entry),
        "final release should recycle");
    require(entry.ref_count.load(std::memory_order_acquire) == 0, "ref count should decrement to zero");
    require(recycle_queue.pop(recycled), "recycle queue should receive the entry");
    require(recycled == &entry, "recycled pointer should match released entry");
    require(!recycle_queue.pop(recycled), "entry should recycle exactly once");
}

void test_last_reference_without_recycle_queue()
{
    WORKER_ENTRY entry{};
    entry.ref_count.store(1, std::memory_order_release);
    entry.gpu_direct_mode = false;

    require(
        release_worker_entry_to_recycle(nullptr, &entry),
        "final release without recycle queue should still report last reference");
    require(entry.ref_count.load(std::memory_order_acquire) == 0, "ref count should decrement to zero");
}

}  // namespace

int main()
{
    try {
        test_null_entry_is_noop();
        test_recycles_only_on_last_reference();
        test_last_reference_without_recycle_queue();
    } catch (const std::exception& e) {
        std::cerr << "worker_entry_release_tests failed: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "worker_entry_release_tests passed" << std::endl;
    return 0;
}
