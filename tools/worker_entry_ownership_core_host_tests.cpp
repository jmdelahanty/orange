#include "worker_entry_ownership_core.h"

#include <atomic>
#include <iostream>
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
        test_ref_guard_releases_on_exception_and_recycles_after_last_ref();
    } catch (const std::exception& e) {
        std::cerr << "worker_entry_ownership_core_host_tests failed: "
                  << e.what() << std::endl;
        return 1;
    }

    std::cout << "worker_entry_ownership_core_host_tests passed" << std::endl;
    return 0;
}
