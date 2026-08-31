#include "worker_entry_ownership_core.h"
#include "thread.h"

#include <atomic>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct HostWorkerEntry {
    std::atomic<int> ref_count{0};
    uint64_t frame_id = 0;
    uint64_t recording_frame_id = 0;
    uint64_t camera_frame_id = 0;
};

struct FakeWorker {
    bool accept = true;
    bool throw_on_enqueue = false;
    int enqueue_attempts = 0;
    HostWorkerEntry* last_entry = nullptr;

    bool PutObjectToQueueIn(HostWorkerEntry* entry)
    {
        enqueue_attempts++;
        last_entry = entry;
        if (throw_on_enqueue) {
            throw std::runtime_error("synthetic enqueue failure");
        }
        return accept;
    }
};

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_no_ownership_diagnostics(const std::string& message)
{
    require(
        worker_entry_release_double_release_count().load(std::memory_order_acquire) == 0,
        message + ": double release count should be zero");
    require(
        worker_entry_release_underflow_count().load(std::memory_order_acquire) == 0,
        message + ": underflow count should be zero");
    require(
        worker_entry_retain_after_release_count().load(std::memory_order_acquire) == 0,
        message + ": retain-after-release count should be zero");
}

struct HostRecycle {
    std::vector<HostWorkerEntry*>* recycled = nullptr;

    void operator()(HostWorkerEntry* entry) const
    {
        if (recycled) {
            recycled->push_back(entry);
        }
    }
};

struct HostRelease {
    std::vector<HostWorkerEntry*>* recycled = nullptr;

    void operator()(
        HostWorkerEntry* entry,
        const WorkerEntryReleaseContext& context) const
    {
        release_worker_entry_ref(entry, context, HostRecycle{recycled});
    }
};

struct ThrowingRecycle {
    int* calls = nullptr;

    bool operator()(HostWorkerEntry*) const
    {
        if (calls) {
            ++*calls;
        }
        throw std::runtime_error("synthetic recycle allocation failure");
    }
};

struct FalseRecycle {
    int* calls = nullptr;

    bool operator()(HostWorkerEntry*) const
    {
        if (calls) {
            ++*calls;
        }
        return false;
    }
};

struct ThrowingRelease {
    void operator()(
        HostWorkerEntry*,
        const WorkerEntryReleaseContext&) const
    {
        throw std::runtime_error("synthetic release callback failure");
    }
};

struct QueueValue {
    static int copies_until_throw;
    static bool throw_on_assignment;

    int value = 0;

    QueueValue() = default;
    explicit QueueValue(int value_in) : value(value_in) {}

    QueueValue(const QueueValue& other)
    {
        if (copies_until_throw == 0) {
            throw std::runtime_error("synthetic SafeQueue push allocation failure");
        }
        if (copies_until_throw > 0) {
            --copies_until_throw;
        }
        value = other.value;
    }

    QueueValue& operator=(const QueueValue& other)
    {
        if (throw_on_assignment) {
            throw std::runtime_error("synthetic SafeQueue pop assignment failure");
        }
        value = other.value;
        return *this;
    }
};

int QueueValue::copies_until_throw = -1;
bool QueueValue::throw_on_assignment = false;

void test_retain_from_positive_ref_count_increments()
{
    reset_worker_entry_release_diagnostics_for_tests();
    HostWorkerEntry entry;
    entry.ref_count.store(1, std::memory_order_release);

    require(
        retain_worker_entry_ref(
            &entry,
            WorkerEntryReleaseContext{"2010096", "host_retain"}),
        "retain from positive ref count should succeed");
    require(entry.ref_count.load(std::memory_order_acquire) == 2, "retain should increment ref count");
    require(
        worker_entry_retain_after_release_count().load(std::memory_order_acquire) == 0,
        "normal retain should not increment retain-after-release counter");
}

void test_retain_from_zero_and_negative_fail()
{
    reset_worker_entry_release_diagnostics_for_tests();

    HostWorkerEntry zero;
    zero.ref_count.store(0, std::memory_order_release);
    zero.frame_id = 10;
    require(
        !retain_worker_entry_ref(
            &zero,
            WorkerEntryReleaseContext{"2010096", "host_retain_zero"}),
        "retain from zero should fail");
    require(zero.ref_count.load(std::memory_order_acquire) == 0, "retain from zero should not increment");

    HostWorkerEntry negative;
    negative.ref_count.store(-2, std::memory_order_release);
    require(
        !retain_worker_entry_ref(
            &negative,
            WorkerEntryReleaseContext{"2010096", "host_retain_negative"}),
        "retain from negative should fail");
    require(negative.ref_count.load(std::memory_order_acquire) == -2, "retain from negative should not increment");

    require(
        worker_entry_retain_after_release_count().load(std::memory_order_acquire) == 2,
        "failed retains should increment retain-after-release counter");
}

void test_release_non_final_and_final_recycle()
{
    reset_worker_entry_release_diagnostics_for_tests();
    std::vector<HostWorkerEntry*> recycled;
    HostWorkerEntry entry;
    entry.ref_count.store(2, std::memory_order_release);

    require(
        !release_worker_entry_ref(
            &entry,
            WorkerEntryReleaseContext{"2010096", "host_release"},
            HostRecycle{&recycled}),
        "non-final release should not recycle");
    require(entry.ref_count.load(std::memory_order_acquire) == 1, "non-final release should decrement");
    require(recycled.empty(), "non-final release should not enqueue recycle");

    require(
        release_worker_entry_ref(
            &entry,
            WorkerEntryReleaseContext{"2010096", "host_release"},
            HostRecycle{&recycled}),
        "final release should recycle");
    require(entry.ref_count.load(std::memory_order_acquire) == 0, "final release should decrement to zero");
    require(recycled.size() == 1 && recycled[0] == &entry, "final release should recycle once");
}

void test_double_release_and_underflow_are_detected()
{
    reset_worker_entry_release_diagnostics_for_tests();
    std::vector<HostWorkerEntry*> recycled;

    HostWorkerEntry zero;
    zero.ref_count.store(0, std::memory_order_release);
    require(
        !release_worker_entry_ref(
            &zero,
            WorkerEntryReleaseContext{"2010096", "host_double_release"},
            HostRecycle{&recycled}),
        "release at zero should fail");
    require(zero.ref_count.load(std::memory_order_acquire) == 0, "release at zero should not decrement");
    require(recycled.empty(), "release at zero should not recycle");

    HostWorkerEntry negative;
    negative.ref_count.store(-3, std::memory_order_release);
    require(
        !release_worker_entry_ref(
            &negative,
            WorkerEntryReleaseContext{"2010096", "host_underflow"},
            HostRecycle{&recycled}),
        "release at negative should fail");
    require(negative.ref_count.load(std::memory_order_acquire) == -3, "release at negative should not decrement");

    require(
        worker_entry_release_double_release_count().load(std::memory_order_acquire) == 1,
        "release at zero should count as double release");
    require(
        worker_entry_release_underflow_count().load(std::memory_order_acquire) == 1,
        "release at negative should count as underflow");
}

void test_context_diagnostics_are_tracked()
{
    reset_worker_entry_release_diagnostics_for_tests();
    std::vector<HostWorkerEntry*> recycled;

    HostWorkerEntry zero;
    zero.ref_count.store(0, std::memory_order_release);
    (void)release_worker_entry_ref(
        &zero,
        WorkerEntryReleaseContext{"2010096", "host_release_stage"},
        HostRecycle{&recycled});

    HostWorkerEntry negative;
    negative.ref_count.store(-1, std::memory_order_release);
    (void)release_worker_entry_ref(
        &negative,
        WorkerEntryReleaseContext{"2010096", "host_release_stage"},
        HostRecycle{&recycled});

    HostWorkerEntry retain_failed;
    retain_failed.ref_count.store(0, std::memory_order_release);
    (void)retain_worker_entry_ref(
        &retain_failed,
        WorkerEntryReleaseContext{"2010096", "host_retain_stage"});

    const WorkerEntryRefCountDiagnosticCounts release_counts =
        worker_entry_ref_count_diagnostic_counts_for_context(
            WorkerEntryReleaseContext{"2010096", "host_release_stage"});
    require(release_counts.double_releases == 1, "context should count double release");
    require(release_counts.release_underflows == 1, "context should count underflow");
    require(release_counts.retain_after_release == 0, "release context should not count retain failure");

    const WorkerEntryRefCountDiagnosticCounts camera_counts =
        worker_entry_ref_count_diagnostic_counts_for_camera("2010096");
    require(camera_counts.double_releases == 1, "camera should aggregate double releases");
    require(camera_counts.release_underflows == 1, "camera should aggregate underflows");
    require(camera_counts.retain_after_release == 1, "camera should aggregate retain failures");
}

void test_retain_and_enqueue_succeeds_when_worker_accepts()
{
    reset_worker_entry_release_diagnostics_for_tests();
    std::vector<HostWorkerEntry*> recycled;
    HostWorkerEntry entry;
    entry.ref_count.store(1, std::memory_order_release);
    FakeWorker worker;
    bool enqueue_rejected = true;

    require(
        retain_and_enqueue_worker_entry_ref(
            &worker,
            &entry,
            WorkerEntryReleaseContext{"2010096", "host_enqueue"},
            HostRelease{&recycled},
            &enqueue_rejected),
        "retain-and-enqueue should succeed when worker accepts");
    require(!enqueue_rejected, "accepted enqueue should not report rejection");
    require(worker.enqueue_attempts == 1, "accepted enqueue should call worker");
    require(worker.last_entry == &entry, "accepted enqueue should pass entry");
    require(entry.ref_count.load(std::memory_order_acquire) == 2, "accepted enqueue should keep retained ref");
    require(recycled.empty(), "accepted enqueue should not recycle");
}

void test_retain_and_enqueue_compensates_when_worker_rejects()
{
    reset_worker_entry_release_diagnostics_for_tests();
    std::vector<HostWorkerEntry*> recycled;
    HostWorkerEntry entry;
    entry.ref_count.store(1, std::memory_order_release);
    FakeWorker worker;
    worker.accept = false;
    bool enqueue_rejected = false;

    require(
        !retain_and_enqueue_worker_entry_ref(
            &worker,
            &entry,
            WorkerEntryReleaseContext{"2010096", "host_enqueue_reject"},
            HostRelease{&recycled},
            &enqueue_rejected),
        "retain-and-enqueue should fail when worker rejects");
    require(enqueue_rejected, "rejected enqueue should report rejection");
    require(worker.enqueue_attempts == 1, "rejected enqueue should call worker");
    require(entry.ref_count.load(std::memory_order_acquire) == 1, "rejected enqueue should release retained ref");
    require(recycled.empty(), "rejected enqueue should not recycle while base ref remains");
}

void test_retain_and_enqueue_compensates_when_worker_throws()
{
    reset_worker_entry_release_diagnostics_for_tests();
    std::vector<HostWorkerEntry*> recycled;
    HostWorkerEntry entry;
    entry.ref_count.store(1, std::memory_order_release);
    FakeWorker worker;
    worker.throw_on_enqueue = true;
    bool enqueue_rejected = false;

    bool caught = false;
    try {
        (void)retain_and_enqueue_worker_entry_ref(
            &worker,
            &entry,
            WorkerEntryReleaseContext{"2010096", "host_enqueue_throw"},
            HostRelease{&recycled},
            &enqueue_rejected);
    } catch (const std::runtime_error&) {
        caught = true;
    }

    require(caught, "worker enqueue exception should propagate");
    require(!enqueue_rejected, "throwing enqueue should not report explicit rejection");
    require(worker.enqueue_attempts == 1, "throwing enqueue should call worker once");
    require(worker.last_entry == &entry, "throwing enqueue should pass entry");
    require(entry.ref_count.load(std::memory_order_acquire) == 1, "throwing enqueue should release retained ref");
    require(recycled.empty(), "throwing enqueue should not recycle while base ref remains");
    require(
        worker_entry_release_double_release_count().load(std::memory_order_acquire) == 0,
        "throwing enqueue compensation should not count as double release");
    require(
        worker_entry_release_underflow_count().load(std::memory_order_acquire) == 0,
        "throwing enqueue compensation should not count as underflow");
    require(
        worker_entry_retain_after_release_count().load(std::memory_order_acquire) == 0,
        "throwing enqueue compensation should not count as retain-after-release");
}

void test_retained_lease_creation_increments_ref_count()
{
    reset_worker_entry_release_diagnostics_for_tests();
    std::vector<HostWorkerEntry*> recycled;
    HostWorkerEntry entry;
    entry.ref_count.store(1, std::memory_order_release);

    {
        auto lease = try_retain_worker_entry_ref_lease(
            &entry,
            WorkerEntryReleaseContext{"2010096", "host_lease_create"},
            HostRelease{&recycled});
        require(lease.active(), "successful retained lease should be active");
        require(static_cast<bool>(lease), "successful retained lease should convert to true");
        require(entry.ref_count.load(std::memory_order_acquire) == 2, "successful retained lease should increment");
    }

    require(entry.ref_count.load(std::memory_order_acquire) == 1, "lease destructor should release retained ref");
    require(recycled.empty(), "lease destructor should not recycle while base ref remains");
    require_no_ownership_diagnostics("successful retained lease");
}

void test_failed_retain_creates_inactive_lease()
{
    reset_worker_entry_release_diagnostics_for_tests();
    std::vector<HostWorkerEntry*> recycled;
    HostWorkerEntry entry;
    entry.ref_count.store(0, std::memory_order_release);

    {
        auto lease = try_retain_worker_entry_ref_lease(
            &entry,
            WorkerEntryReleaseContext{"2010096", "host_lease_failed_retain"},
            HostRelease{&recycled});
        require(!lease.active(), "failed retained lease should be inactive");
        require(!static_cast<bool>(lease), "failed retained lease should convert to false");
    }

    require(entry.ref_count.load(std::memory_order_acquire) == 0, "failed lease should not change ref count");
    require(recycled.empty(), "failed lease should not recycle");
    require(
        worker_entry_retain_after_release_count().load(std::memory_order_acquire) == 1,
        "failed lease retain should count retain-after-release");
    require(
        worker_entry_release_double_release_count().load(std::memory_order_acquire) == 0,
        "failed lease should not count double release");
    require(
        worker_entry_release_underflow_count().load(std::memory_order_acquire) == 0,
        "failed lease should not count underflow");
}

void test_lease_transfer_prevents_release()
{
    reset_worker_entry_release_diagnostics_for_tests();
    std::vector<HostWorkerEntry*> recycled;
    HostWorkerEntry entry;
    entry.ref_count.store(1, std::memory_order_release);
    const WorkerEntryReleaseContext context{"2010096", "host_lease_transfer"};

    {
        auto lease = try_retain_worker_entry_ref_lease(
            &entry,
            context,
            HostRelease{&recycled});
        require(lease.active(), "lease should be active before transfer");
        require(entry.ref_count.load(std::memory_order_acquire) == 2, "lease should retain before transfer");
        lease.TransferToConsumer();
        require(!lease.active(), "transferred lease should become inactive");
    }

    require(entry.ref_count.load(std::memory_order_acquire) == 2, "transferred lease should not release on destruction");
    require(recycled.empty(), "transferred lease should not recycle");
    require_no_ownership_diagnostics("transferred lease");

    require(
        !release_worker_entry_ref(&entry, context, HostRecycle{&recycled}),
        "consumer release should leave base ref active");
    require(entry.ref_count.load(std::memory_order_acquire) == 1, "consumer release should drop to base ref");
    require(
        release_worker_entry_ref(&entry, context, HostRecycle{&recycled}),
        "base release should recycle final ref");
    require(recycled.size() == 1 && recycled[0] == &entry, "final base release should recycle once");
}

void test_lease_releases_on_exception_unwind()
{
    reset_worker_entry_release_diagnostics_for_tests();
    std::vector<HostWorkerEntry*> recycled;
    HostWorkerEntry entry;
    entry.ref_count.store(1, std::memory_order_release);

    bool caught = false;
    try {
        auto lease = try_retain_worker_entry_ref_lease(
            &entry,
            WorkerEntryReleaseContext{"2010096", "host_lease_exception"},
            HostRelease{&recycled});
        require(lease.active(), "lease should be active before exception");
        require(entry.ref_count.load(std::memory_order_acquire) == 2, "lease should retain before exception");
        throw std::runtime_error("synthetic lease unwind");
    } catch (const std::runtime_error&) {
        caught = true;
    }

    require(caught, "synthetic lease exception should propagate");
    require(entry.ref_count.load(std::memory_order_acquire) == 1, "lease should release retained ref on unwind");
    require(recycled.empty(), "lease unwind should not recycle while base ref remains");
    require_no_ownership_diagnostics("lease exception unwind");
}

void test_ref_guard_releases_on_exception_and_recycles_after_last_ref()
{
    reset_worker_entry_release_diagnostics_for_tests();
    std::vector<HostWorkerEntry*> recycled;
    HostWorkerEntry entry;
    entry.ref_count.store(1, std::memory_order_release);
    const WorkerEntryReleaseContext base_context{"2010096", "host_base_ref"};
    const WorkerEntryReleaseContext worker_context{"2010096", "host_worker_ref"};

    require(retain_worker_entry_ref(&entry, worker_context), "worker retain should succeed");
    require(entry.ref_count.load(std::memory_order_acquire) == 2, "base and worker refs should be active");

    bool caught = false;
    try {
        auto guard = make_worker_entry_ref_guard(
            &entry,
            base_context,
            HostRelease{&recycled},
            true);
        throw std::runtime_error("synthetic host fanout failure");
    } catch (const std::runtime_error&) {
        caught = true;
    }

    require(caught, "synthetic exception should propagate");
    require(entry.ref_count.load(std::memory_order_acquire) == 1, "guard should release only base ref");
    require(recycled.empty(), "guard should not recycle while worker ref remains");

    require(
        release_worker_entry_ref(&entry, worker_context, HostRecycle{&recycled}),
        "worker release should recycle final ref");
    require(recycled.size() == 1 && recycled[0] == &entry, "final worker release should recycle once");
    require(!release_worker_entry_ref(&entry, worker_context, HostRecycle{&recycled}), "double release should fail");
    require(recycled.size() == 1, "double release should not recycle again");
}

void test_throwing_final_recycle_is_dropped_and_reported()
{
    reset_worker_entry_release_diagnostics_for_tests();
    HostWorkerEntry entry;
    entry.ref_count.store(1, std::memory_order_release);
    const WorkerEntryReleaseContext context{"2010096", "host_recycle_failure"};
    int recycle_calls = 0;

    bool caught = false;
    bool recycled = true;
    try {
        recycled = release_worker_entry_ref(
            &entry,
            context,
            ThrowingRecycle{&recycle_calls});
    } catch (...) {
        caught = true;
    }

    require(!caught, "final recycle exceptions must not escape the release boundary");
    require(!recycled, "a failed final recycle must return false");
    require(recycle_calls == 1, "the final recycle callback should be attempted once");
    require(entry.ref_count.load(std::memory_order_acquire) == 0,
            "a failed final recycle must leave the entry at zero references");
    require(worker_entry_recycle_failure_count().load(std::memory_order_acquire) == 1,
            "a failed final recycle should increment the global diagnostic counter");
    const WorkerEntryRefCountDiagnosticCounts counts =
        worker_entry_ref_count_diagnostic_counts_for_context(context);
    require(counts.recycle_failures == 1,
            "a failed final recycle should increment the context diagnostic counter");

    require(!release_worker_entry_ref(&entry, context, HostRecycle{}),
            "a dropped zero-ref entry must reject a later release");
}

void test_false_final_recycle_is_reported_once()
{
    reset_worker_entry_release_diagnostics_for_tests();
    HostWorkerEntry entry;
    entry.ref_count.store(1, std::memory_order_release);
    const WorkerEntryReleaseContext context{"2010096", "host_false_recycle"};
    int recycle_calls = 0;

    require(
        !release_worker_entry_ref(
            &entry,
            context,
            FalseRecycle{&recycle_calls}),
        "a recycle callback returning false must fail the final release");
    require(recycle_calls == 1, "a false recycle callback should be attempted once");
    require(entry.ref_count.load(std::memory_order_acquire) == 0,
            "a false recycle callback must leave the entry at zero references");
    require(worker_entry_recycle_failure_count().load(std::memory_order_acquire) == 1,
            "a false recycle callback must increment the failure counter exactly once");
    const WorkerEntryRefCountDiagnosticCounts counts =
        worker_entry_ref_count_diagnostic_counts_for_context(context);
    require(counts.recycle_failures == 1,
            "a false recycle callback must create one context diagnostic");
}

void test_recycle_failure_counter_saturates()
{
    reset_worker_entry_release_diagnostics_for_tests();
    worker_entry_recycle_failure_count().store(
        std::numeric_limits<uint64_t>::max(),
        std::memory_order_release);
    HostWorkerEntry entry;
    entry.ref_count.store(1, std::memory_order_release);

    require(
        !release_worker_entry_ref(&entry, {}, FalseRecycle{}),
        "a false recycle should still fail when the counter is saturated");
    require(
        worker_entry_recycle_failure_count().load(std::memory_order_acquire) ==
            std::numeric_limits<uint64_t>::max(),
        "recycle failure counter must saturate instead of wrapping");
}

void test_guard_contains_arbitrary_release_callback_exception()
{
    reset_worker_entry_release_diagnostics_for_tests();
    HostWorkerEntry entry;
    entry.ref_count.store(1, std::memory_order_release);
    const WorkerEntryReleaseContext context{"2010096", "host_throwing_guard"};

    bool caught = false;
    try {
        auto guard = make_worker_entry_ref_guard(
            &entry,
            context,
            ThrowingRelease{},
            true);
    } catch (...) {
        caught = true;
    }

    require(!caught, "guard destruction must contain arbitrary release exceptions");
    require(worker_entry_recycle_failure_count().load(std::memory_order_acquire) == 1,
            "guard release exceptions must increment the failure counter once");
    const WorkerEntryRefCountDiagnosticCounts counts =
        worker_entry_ref_count_diagnostic_counts_for_context(context);
    require(counts.recycle_failures == 1,
            "guard release exceptions must create one context diagnostic");
}

void test_safe_queue_unlocks_when_push_or_pop_throws()
{
    SafeQueue<QueueValue> queue;
    QueueValue source(42);

    QueueValue::copies_until_throw = 1;
    bool push_caught = false;
    try {
        queue.push(source);
    } catch (const std::runtime_error&) {
        push_caught = true;
    }
    QueueValue::copies_until_throw = -1;
    require(push_caught, "SafeQueue push should expose the synthetic copy failure");

    queue.push(source);
    QueueValue destination;
    QueueValue::throw_on_assignment = true;
    bool pop_caught = false;
    try {
        (void)queue.pop(destination);
    } catch (const std::runtime_error&) {
        pop_caught = true;
    }
    QueueValue::throw_on_assignment = false;
    require(pop_caught, "SafeQueue pop should expose the synthetic assignment failure");

    require(queue.pop(destination), "SafeQueue should remain usable after a throwing pop");
    require(destination.value == 42,
            "SafeQueue should preserve the queued item after a throwing pop");
}

}  // namespace

int main()
{
    try {
        test_retain_from_positive_ref_count_increments();
        test_retain_from_zero_and_negative_fail();
        test_release_non_final_and_final_recycle();
        test_double_release_and_underflow_are_detected();
        test_context_diagnostics_are_tracked();
        test_retain_and_enqueue_succeeds_when_worker_accepts();
        test_retain_and_enqueue_compensates_when_worker_rejects();
        test_retain_and_enqueue_compensates_when_worker_throws();
        test_retained_lease_creation_increments_ref_count();
        test_failed_retain_creates_inactive_lease();
        test_lease_transfer_prevents_release();
        test_lease_releases_on_exception_unwind();
        test_ref_guard_releases_on_exception_and_recycles_after_last_ref();
        test_throwing_final_recycle_is_dropped_and_reported();
        test_false_final_recycle_is_reported_once();
        test_recycle_failure_counter_saturates();
        test_guard_contains_arbitrary_release_callback_exception();
        test_safe_queue_unlocks_when_push_or_pop_throws();
    } catch (const std::exception& e) {
        std::cerr << "worker_entry_ownership_core_host_tests failed: "
                  << e.what() << std::endl;
        return 1;
    }

    std::cout << "worker_entry_ownership_core_host_tests passed" << std::endl;
    return 0;
}
