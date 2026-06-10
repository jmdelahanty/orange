// Concurrent handoff hammer: retain_and_enqueue_worker_entry_ref across a
// real CThreadWorker queue boundary.
//
// The ownership hammer (worker_entry_ownership_tsan_hammer.cpp) proves the
// refcount core under contention. This test proves the *handoff*: the
// pattern the acquisition thread uses to distribute a frame to consumers —
// retain a ref, enqueue to a bounded worker queue, Dismiss the guard on
// success so the consumer inherits the ref, auto-release on rejection.
//
// Topology: 8 producers fan into one HandoffWorker through a 4-slot bounded
// queue (heavy blocking/wakeup traffic on both condition variables), worker
// releases the inherited ref per item. Two phases:
//   Phase 1 — normal flow. PutObjectToQueueIn BLOCKS when full (backpressure,
//     not rejection), so every handoff should be accepted; producers then
//     drain the queue completely before stopping the worker. Draining before
//     StopThread matters: items still queued at stop are dropped along with
//     their retained refs (a leak) — the same reason the real pipeline's
//     drain checks (recording_ingress IsDrained) must read accurate queue
//     counts.
//   Phase 2 — post-stop. Every handoff must be rejected with
//     enqueue_rejected=true, and the guard must auto-release the retained
//     ref so nothing leaks.
//
// Invariants checked arithmetically: accepted == processed, post-stop
// handoffs all rejected, every entry's refcount back to baseline (exact
// retain/release balance across ~160k queue crossings), and all ownership
// diagnostics zero. TSan watches the queue mutex/CV machinery and the guard
// paths for ordering holes.
//
// Normal usage (see docs/thread_sanitizer_runbook.md):
//   cmake --preset debug_tsan && cmake --build --preset debug_tsan
//   ctest --test-dir targets/debug_tsan -R worker_entry_handoff_tsan_hammer

#include "threadworker.h"
#include "worker_entry_ownership_core.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr int kProducers = 8;
constexpr int kIterationsPerProducer = 20000;
constexpr int kEntries = 4;
constexpr int kQueueCapacity = 4;     // small -> constant blocking/wakeups
constexpr int kPostStopAttempts = 64;

struct HandoffEntry {
    std::atomic<int> ref_count{0};

    // Optional debug fields the core's logging picks up via SFINAE.
    uint64_t frame_id = 0;
    uint64_t recording_frame_id = 0;
    uint64_t camera_frame_id = 0;
};

const WorkerEntryReleaseContext kProducerCtx{"hammer-cam", "HandoffProducer"};
const WorkerEntryReleaseContext kWorkerCtx{"hammer-cam", "HandoffWorker"};

// Guard release path used when an enqueue is rejected: just drop the ref.
// Entries hold a permanent publisher token (count >= 1), so no recycle
// callback is needed — the count never reaches zero in this test.
void guard_release(HandoffEntry* entry, const WorkerEntryReleaseContext& ctx)
{
    release_worker_entry_ref(entry, ctx);
}

class HandoffWorker : public CThreadWorker<HandoffEntry> {
public:
    HandoffWorker() : CThreadWorker<HandoffEntry>("HandoffWorker") {}

    std::atomic<uint64_t> processed{0};

protected:
    bool WorkerFunction(HandoffEntry* entry) override
    {
        // ThreadRunning calls WorkerFunction(nullptr) at shutdown as a final
        // housekeeping tick (threadworker.h:340). Every worker subclass must
        // handle it — this hammer's first run counted the phantom call and
        // failed accepted == processed by exactly one.
        if (!entry) {
            return false;
        }
        // Consumer side of the handoff: we inherited the producer's ref via
        // the guard Dismiss; releasing it here completes the contract.
        release_worker_entry_ref(entry, kWorkerCtx);
        processed.fetch_add(1, std::memory_order_relaxed);
        return false;  // nothing goes to queueOut
    }
};

struct ProducerTally {
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t retain_failed = 0;
};

int g_failures = 0;

void check(bool ok, const char* what, uint64_t got, uint64_t want)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s (got=%llu want=%llu)\n", what,
                     static_cast<unsigned long long>(got),
                     static_cast<unsigned long long>(want));
        ++g_failures;
    }
}

void producer_thread(int tid, HandoffWorker* worker,
                     std::array<HandoffEntry, kEntries>* pool,
                     ProducerTally* tally)
{
    uint32_t rng = 0x6c078965u ^ static_cast<uint32_t>(tid);
    for (int i = 0; i < kIterationsPerProducer; ++i) {
        rng = rng * 1664525u + 1013904223u;
        HandoffEntry* entry = &(*pool)[rng % kEntries];

        bool rejected = false;
        if (retain_and_enqueue_worker_entry_ref(worker, entry, kProducerCtx,
                                                guard_release, &rejected)) {
            tally->accepted += 1;
        } else if (rejected) {
            tally->rejected += 1;
        } else {
            tally->retain_failed += 1;  // count never hits 0: must stay 0
        }
    }
}

}  // namespace

int main()
{
    reset_worker_entry_release_diagnostics_for_tests();

    static std::array<HandoffEntry, kEntries> pool;
    for (size_t i = 0; i < pool.size(); ++i) {
        pool[i].frame_id = i;
        // Permanent publisher token: entries stay alive for the whole run.
        pool[i].ref_count.store(1, std::memory_order_release);
    }

    HandoffWorker worker;
    worker.SetMaxQueueSize(kQueueCapacity);
    worker.StartThread("HandoffWorker");

    // ---- Phase 1: normal flow under backpressure ----
    std::array<ProducerTally, kProducers> tallies{};
    {
        std::vector<std::thread> producers;
        producers.reserve(kProducers);
        for (int t = 0; t < kProducers; ++t) {
            producers.emplace_back(producer_thread, t, &worker, &pool,
                                   &tallies[t]);
        }
        for (auto& th : producers) {
            th.join();
        }
    }

    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t retain_failed = 0;
    for (const auto& tally : tallies) {
        accepted += tally.accepted;
        rejected += tally.rejected;
        retain_failed += tally.retain_failed;
    }

    // Drain BEFORE StopThread: queued items dropped at stop leak their refs.
    while (worker.GetCountQueueIn() > 0 ||
           worker.processed.load() < accepted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.StopThread();

    // ---- Phase 2: every post-stop handoff is rejected and auto-released ----
    uint64_t post_stop_rejected = 0;
    for (int i = 0; i < kPostStopAttempts; ++i) {
        bool was_rejected = false;
        const bool ok = retain_and_enqueue_worker_entry_ref(
            &worker, &pool[i % kEntries], kProducerCtx, guard_release,
            &was_rejected);
        if (!ok && was_rejected) {
            post_stop_rejected += 1;
        }
    }

    // ---- Invariants ----
    check(accepted == worker.processed.load(), "accepted == processed",
          accepted, worker.processed.load());
    check(rejected == 0, "no rejections while running (queue blocks instead)",
          rejected, 0);
    check(retain_failed == 0, "no retain failures (entries never die)",
          retain_failed, 0);
    check(post_stop_rejected == kPostStopAttempts,
          "all post-stop handoffs rejected",
          post_stop_rejected, kPostStopAttempts);

    // Exact retain/release balance across every queue crossing: each entry
    // ends at its baseline publisher token. A single leaked or doubled
    // release anywhere in ~160k handoffs shows up here.
    for (auto& entry : pool) {
        check(entry.ref_count.load() == 1, "entry refcount at baseline",
              static_cast<uint64_t>(entry.ref_count.load()), 1);
    }

    check(worker_entry_release_double_release_count().load() == 0,
          "no double releases",
          worker_entry_release_double_release_count().load(), 0);
    check(worker_entry_release_underflow_count().load() == 0,
          "no release underflows",
          worker_entry_release_underflow_count().load(), 0);
    check(worker_entry_retain_after_release_count().load() == 0,
          "no retain_after_release",
          worker_entry_retain_after_release_count().load(), 0);

    std::printf(
        "handoff hammer: producers=%d iters=%d queue=%d | accepted=%llu "
        "processed=%llu post_stop_rejected=%llu\n",
        kProducers, kIterationsPerProducer, kQueueCapacity,
        static_cast<unsigned long long>(accepted),
        static_cast<unsigned long long>(worker.processed.load()),
        static_cast<unsigned long long>(post_stop_rejected));

    if (g_failures != 0) {
        std::fprintf(stderr, "worker_entry_handoff_tsan_hammer: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::printf("worker_entry_handoff_tsan_hammer: all invariants held\n");
    return 0;
}
