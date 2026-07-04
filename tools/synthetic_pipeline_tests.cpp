// Synthetic pipeline integration tests: the first end-to-end test of the
// worker CHAIN without a camera.
//
// The contract/config layers and the ownership primitive each have their own
// tests (threadworker_tests, worker_entry_ownership_*), but nothing drives
// entries through MULTIPLE CThreadWorker stages with the retain/release
// handoff discipline the real pipeline uses. This test builds a synthetic
// hot path:
//
//   producer (test thread)
//       -> StageA (CThreadWorker, retain_and_enqueue handoff)
//           -> StageB (CThreadWorker, retain_and_enqueue handoff)
//               -> SinkC (CThreadWorker, terminal consumer OR bounded
//                         output queue with a stalled external consumer)
//
// backed by a BoundedObjectPool of refcounted SyntheticFrame entries. Every
// hop uses the exact production pattern from worker_entry_ownership_core.h:
// retain_and_enqueue_worker_entry_ref() on enqueue, a WorkerEntryRefGuardCore
// around each stage's inherited ref (so the exception path releases too), and
// release-to-pool on the final ref drop.
//
// Scenarios:
//   1. steady state       - thousands of frames, all delivered in order, pool
//                           fully recovered after the flush-tick drain cascade
//   2. backpressure       - slow StageB; bounded input queues throttle the
//                           producer (proved by a conservation-law time bound,
//                           not by timing luck); no losses, pool recovers
//   3. forced overflow    - SinkC publishes to a bounded output queue nobody
//                           consumes; drops are counted exactly, dropped
//                           entries recycle to the pool, survivors stay FIFO
//   4. mid-run failure    - StageB's WorkerFunction throws after K frames;
//                           fatal-error latch set, chain still drains and
//                           stops cleanly, pool recovers (exception path)
//   5. flush-tick drain   - frames enqueued then drain requested immediately;
//                           the FIFO tick cascade observes every frame at the
//                           sink before finalization
//
// Determinism: no sleep is used as synchronization. Sleeps only shape load
// (a slow stage); all waits are CV/counter-based with generous timeouts, and
// the backpressure assertion is a conservation bound (the producer cannot run
// more than pool-capacity frames ahead of the bottleneck stage), which holds
// on any scheduler.
//
// Normal usage:
//   ctest --test-dir targets/release -R synthetic_pipeline_tests
// Also in the TSan preset (see docs/thread_sanitizer_runbook.md):
//   cmake --preset debug_tsan && cmake --build --preset debug_tsan
//   ctest --test-dir targets/debug_tsan -R synthetic_pipeline_tests
//
// Standalone build (no CUDA/SDK deps):
//   g++ -std=c++17 -Isrc tools/synthetic_pipeline_tests.cpp
//       src/offthreadmachine.cpp -o /tmp/synthetic_pipeline_tests -lpthread

#include "bounded_object_pool.h"
#include "threadworker.h"
#include "worker_entry_ownership_core.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Generous: the TSan build runs everything several times slower.
constexpr auto kWaitTimeout = std::chrono::seconds(120);

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// ---------------------------------------------------------------------------
// Pool-backed refcounted entry, shaped like the real WORKER_ENTRY ownership
// model: an atomic refcount plus the debug fields the ownership core's
// logging picks up via SFINAE.
// ---------------------------------------------------------------------------

struct SyntheticFrame {
    std::atomic<int> ref_count{0};
    uint64_t frame_id = 0;
    uint64_t recording_frame_id = 0;
    uint64_t camera_frame_id = 0;
};

// std::atomic is not copy-assignable, so the pool needs an explicit reset.
struct SyntheticFrameReset {
    void operator()(SyntheticFrame& frame) const
    {
        frame.ref_count.store(0, std::memory_order_release);
        frame.frame_id = 0;
        frame.recording_frame_id = 0;
        frame.camera_frame_id = 0;
    }
};

using FramePool = BoundedObjectPool<SyntheticFrame, SyntheticFrameReset>;

// Release one ref; the FINAL release recycles the entry to the pool. This is
// the recycle discipline every owner in the chain (producer, stages, sink,
// drop paths) must share, exactly like release_worker_entry_to_recycle in
// the real pipeline.
struct ReleaseToPool {
    FramePool* pool = nullptr;

    void operator()(SyntheticFrame* frame,
                    const WorkerEntryReleaseContext& context) const
    {
        FramePool* target = pool;
        release_worker_entry_ref(
            frame,
            context,
            [target](SyntheticFrame* final_frame) {
                target->Return(final_frame);
            });
    }
};

// ---------------------------------------------------------------------------
// A pipeline stage. Three roles, selected by configuration:
//   - forwarding stage: hands the entry to `next_` via retain_and_enqueue
//   - terminal sink: records the frame id, final release recycles to pool
//   - bounded-output sink: transfers its ref to the (bounded) output queue,
//     mirroring a display/preview worker whose consumer pulls from queueOut
// ---------------------------------------------------------------------------

class SyntheticStage final : public CThreadWorker<SyntheticFrame> {
public:
    SyntheticStage(const char* name, FramePool* pool)
        : CThreadWorker<SyntheticFrame>(name),
          pool_(pool),
          ctx_{"synthetic-cam", name}
    {
    }

    ~SyntheticStage() override { StopThread(); }

    void SetNext(SyntheticStage* next) { next_ = next; }

    // Load shaping only (never synchronization): makes this stage the
    // bottleneck so bounded queues upstream actually fill.
    void SetProcessingDelay(std::chrono::microseconds delay)
    {
        delay_us_ = static_cast<int64_t>(delay.count());
    }

    // Fail injection: WorkerFunction throws for every frame after the first
    // `frames` frames (mimicking a CHECK() macro surfacing a CUDA error).
    void SetThrowAfter(int frames) { throw_after_ = frames; }

    // Turns this stage into a bounded-output-queue publisher: entries go to
    // queueOut (transferring this stage's ref) instead of being recycled.
    void EnableBoundedOutputSink(int capacity)
    {
        publish_to_queue_out_ = true;
        SetMaxQueueOutSize(capacity);
    }

    // Releases an output-queue entry exactly as the external consumer would.
    void ReleaseAsConsumer(SyntheticFrame* frame)
    {
        ReleaseToPool{pool_}(frame, ctx_);
    }

    std::vector<uint64_t> ReceivedFrames() const
    {
        std::lock_guard<std::mutex> lock(received_mutex_);
        return received_;
    }

    // Sink-side observation of the drain guarantee: the number of frames the
    // sink had received at the instant each flush tick was delivered.
    std::vector<uint64_t> FlushTickSnapshots() const
    {
        std::lock_guard<std::mutex> lock(received_mutex_);
        return flush_snapshots_;
    }

    // Waits (CV, not polling) until this stage has observed at least `target`
    // flush ticks. Only meaningful on the terminal stage of a chain.
    //
    // Deliberately a system_clock wait_until rather than wait_for: a
    // steady_clock deadline makes libstdc++ call pthread_cond_clockwait,
    // which the GCC 11 libtsan does not intercept - TSan then misses the
    // unlock inside the wait and reports bogus "double lock" / data races
    // on tick_mutex_. pthread_cond_timedwait (system_clock) is intercepted.
    bool WaitForFlushTicks(uint64_t target, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::system_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(tick_mutex_);
        return tick_cv_.wait_until(lock, deadline, [&]() {
            return ticks_seen_ >= target;
        });
    }

    uint64_t ForwardFailures() const
    {
        return forward_failures_.load(std::memory_order_acquire);
    }

    int Processed() const { return processed_.load(std::memory_order_acquire); }

protected:
    bool WorkerFunction(SyntheticFrame* frame) override
    {
        // This stage inherited exactly one ref from the upstream handoff.
        // The guard releases it on EVERY exit path, including the throw
        // below - that is what keeps the pool leak-free through scenario 4.
        auto guard = make_worker_entry_ref_guard(
            frame, ctx_, ReleaseToPool{pool_}, true);

        if (delay_us_ > 0) {
            // Load shaping only: correctness never depends on this duration.
            std::this_thread::sleep_for(std::chrono::microseconds(delay_us_));
        }

        const int seen = processed_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (throw_after_ >= 0 && seen > throw_after_) {
            throw std::runtime_error("synthetic stage failure injection");
        }

        if (next_) {
            bool rejected = false;
            if (!retain_and_enqueue_worker_entry_ref(
                    next_, frame, ctx_, ReleaseToPool{pool_}, &rejected)) {
                forward_failures_.fetch_add(1, std::memory_order_acq_rel);
            }
            return false;  // guard releases this stage's ref
        }

        {
            std::lock_guard<std::mutex> lock(received_mutex_);
            received_.push_back(frame->frame_id);
        }

        if (publish_to_queue_out_) {
            // Transfer this stage's ref to the output queue: the external
            // consumer (or the drop path) now owns it.
            guard.Dismiss();
            return true;
        }
        return false;  // terminal sink: guard's release recycles to the pool
    }

    void OnFlushTick() override
    {
        if (next_) {
            // Drain cascade: forward the marker so downstream stages flush
            // AFTER everything this stage already forwarded (FIFO). Rejected
            // when downstream already stopped, which is fine at shutdown.
            (void)next_->EnqueueFlushTick();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(received_mutex_);
            flush_snapshots_.push_back(static_cast<uint64_t>(received_.size()));
        }
        {
            std::lock_guard<std::mutex> lock(tick_mutex_);
            ++ticks_seen_;
        }
        tick_cv_.notify_all();
    }

    // Bounded-output drop path: release the evicted entry's ref exactly as
    // the consumer would have, or the pool leaks (see threadworker.h).
    void ReleaseDroppedQueueOutEntry(SyntheticFrame* frame) override
    {
        ReleaseToPool{pool_}(frame, ctx_);
    }

private:
    FramePool* pool_ = nullptr;
    WorkerEntryReleaseContext ctx_{};
    SyntheticStage* next_ = nullptr;
    int64_t delay_us_ = 0;
    int throw_after_ = -1;
    bool publish_to_queue_out_ = false;

    std::atomic<int> processed_{0};
    std::atomic<uint64_t> forward_failures_{0};

    mutable std::mutex received_mutex_;
    std::vector<uint64_t> received_;
    std::vector<uint64_t> flush_snapshots_;

    std::mutex tick_mutex_;
    std::condition_variable tick_cv_;
    uint64_t ticks_seen_ = 0;  // guarded by tick_mutex_
};

// ---------------------------------------------------------------------------
// Chain harness: pool + producer -> StageA -> StageB -> SinkC.
// ---------------------------------------------------------------------------

struct ChainConfig {
    size_t pool_capacity = 64;
    int queue_a = 8;
    int queue_b = 8;
    int queue_c = 8;
};

struct Chain {
    // Declaration order matters: destruction runs in reverse, so stages are
    // torn down upstream-first (A, then B, then C, then the pool). If a test
    // fails before Stop(), each destructor's StopThread() therefore never
    // leaves a live stage forwarding into an already-destroyed downstream
    // worker.
    FramePool pool;
    SyntheticStage sink_c;
    SyntheticStage stage_b;
    SyntheticStage stage_a;

    explicit Chain(const ChainConfig& config)
        : pool("synthetic-frames", config.pool_capacity),
          sink_c("SinkC", &pool),
          stage_b("StageB", &pool),
          stage_a("StageA", &pool)
    {
        stage_a.SetNext(&stage_b);
        stage_b.SetNext(&sink_c);
        stage_a.SetMaxQueueSize(config.queue_a);
        stage_b.SetMaxQueueSize(config.queue_b);
        sink_c.SetMaxQueueSize(config.queue_c);
    }

    void Start()
    {
        // Downstream first so no stage ever forwards into a worker that has
        // not been started yet.
        require(sink_c.StartThread() == 0, "SinkC should start");
        require(stage_b.StartThread() == 0, "StageB should start");
        require(stage_a.StartThread() == 0, "StageA should start");
    }

    // Recording drain cascade: an ordered flush marker rides every input
    // queue in turn, so when the sink reports the tick, every frame enqueued
    // before the drain request has fully traversed the chain.
    void Drain(uint64_t generation)
    {
        require(stage_a.EnqueueFlushTick(), "drain tick should enqueue");
        require(sink_c.WaitForFlushTicks(
                    generation,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        kWaitTimeout)),
                "drain tick should cascade to the sink before timeout");
    }

    void Stop()
    {
        // Upstream first so each stage's shutdown drain forwards into a
        // still-running downstream worker.
        stage_a.StopThread();
        stage_b.StopThread();
        sink_c.StopThread();
    }
};

const WorkerEntryReleaseContext kProducerCtx{"synthetic-cam", "Producer"};

SyntheticFrame* borrow_with_deadline(FramePool& pool, Clock::time_point deadline)
{
    while (true) {
        SyntheticFrame* frame = pool.Borrow();
        if (frame) {
            return frame;
        }
        if (Clock::now() >= deadline) {
            return nullptr;  // let the caller fail the test instead of hanging
        }
        // Bounded retry while the pool refills; correctness does not depend
        // on this interval, only liveness within the deadline.
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

struct ProducerResult {
    uint64_t produced = 0;
    uint64_t handoff_failures = 0;
    bool borrow_timeout = false;
    std::chrono::milliseconds elapsed{0};
};

// Emulates the acquisition thread: borrow from the pool, stamp ids, hand off
// with retain_and_enqueue, then drop the producer's own token.
ProducerResult produce_frames(FramePool& pool, SyntheticStage& first, uint64_t n)
{
    ProducerResult result;
    const auto start = Clock::now();
    const auto deadline = start + kWaitTimeout;

    for (uint64_t i = 0; i < n; ++i) {
        SyntheticFrame* frame = borrow_with_deadline(pool, deadline);
        if (!frame) {
            result.borrow_timeout = true;
            break;
        }
        frame->frame_id = i;
        frame->recording_frame_id = i;
        frame->camera_frame_id = i;
        // Producer token: the entry is live from here until every owner has
        // released (mirrors the acquisition base ref).
        frame->ref_count.store(1, std::memory_order_release);

        bool rejected = false;
        const bool accepted = retain_and_enqueue_worker_entry_ref(
            &first, frame, kProducerCtx, ReleaseToPool{&pool}, &rejected);
        // Hand-off complete (or compensated): drop the producer token. On a
        // rejected enqueue this is the final release and recycles the frame.
        ReleaseToPool{&pool}(frame, kProducerCtx);

        if (accepted) {
            ++result.produced;
        } else {
            ++result.handoff_failures;
        }
    }

    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start);
    return result;
}

std::string pool_stats_to_string(const FramePool::Stats& stats)
{
    std::ostringstream out;
    out << "capacity=" << stats.capacity << " available=" << stats.available
        << " active=" << stats.active << " high_water=" << stats.high_water
        << " borrow_misses=" << stats.borrow_misses
        << " invalid_returns=" << stats.invalid_returns
        << " double_returns=" << stats.double_returns;
    return out.str();
}

// Every entry back in the pool, refcounts balanced, zero ownership
// diagnostics: the invariant every scenario must restore.
void require_pool_recovered(const FramePool& pool, const char* where)
{
    const auto stats = pool.GetStats();
    require(stats.active == 0 && stats.available == stats.capacity,
            std::string(where) + ": pool should be fully recovered (" +
                pool_stats_to_string(stats) + ")");
    require(stats.invalid_returns == 0 && stats.double_returns == 0,
            std::string(where) + ": pool should see no bad returns (" +
                pool_stats_to_string(stats) + ")");
}

void require_zero_ownership_diagnostics(const char* where)
{
    require(worker_entry_release_underflow_count().load() == 0,
            std::string(where) + ": no release underflows expected");
    require(worker_entry_release_double_release_count().load() == 0,
            std::string(where) + ": no double releases expected");
    require(worker_entry_retain_after_release_count().load() == 0,
            std::string(where) + ": no retain-after-release expected");
}

void require_in_order(const std::vector<uint64_t>& received,
                      uint64_t expected_count,
                      const char* where)
{
    require(received.size() == expected_count,
            std::string(where) + ": sink should receive every frame (got " +
                std::to_string(received.size()) + ", want " +
                std::to_string(expected_count) + ")");
    for (uint64_t i = 0; i < expected_count; ++i) {
        if (received[static_cast<size_t>(i)] != i) {
            throw std::runtime_error(
                std::string(where) + ": out-of-order frame at index " +
                std::to_string(i) + " (got " +
                std::to_string(received[static_cast<size_t>(i)]) + ")");
        }
    }
}

// ---------------------------------------------------------------------------
// Scenario 1: steady-state correctness.
// ---------------------------------------------------------------------------

void test_steady_state_delivery_and_pool_recovery()
{
    reset_worker_entry_release_diagnostics_for_tests();
    constexpr uint64_t kFrames = 5000;

    ChainConfig config;
    config.pool_capacity = 64;
    Chain chain(config);
    chain.Start();

    const ProducerResult produced =
        produce_frames(chain.pool, chain.stage_a, kFrames);
    require(!produced.borrow_timeout, "steady: pool should keep recycling");
    require(produced.handoff_failures == 0, "steady: no handoff should fail");
    require(produced.produced == kFrames, "steady: every frame should enqueue");

    chain.Drain(1);

    require_in_order(chain.sink_c.ReceivedFrames(), kFrames, "steady");
    require(chain.stage_a.ForwardFailures() == 0 &&
                chain.stage_b.ForwardFailures() == 0,
            "steady: no stage should fail to forward");
    require(chain.stage_a.GetCountQueueOutDropped() == 0 &&
                chain.stage_b.GetCountQueueOutDropped() == 0 &&
                chain.sink_c.GetCountQueueOutDropped() == 0,
            "steady: nothing should be dropped anywhere");

    // The drain tick reached the sink AFTER every frame finished the chain,
    // so every ref is already released: full recovery before stop.
    require_pool_recovered(chain.pool, "steady (before stop)");

    chain.Stop();
    require_pool_recovered(chain.pool, "steady (after stop)");
    require_zero_ownership_diagnostics("steady");
}

// ---------------------------------------------------------------------------
// Scenario 2: backpressure through bounded input queues.
// ---------------------------------------------------------------------------

void test_backpressure_blocks_producer_without_loss()
{
    reset_worker_entry_release_diagnostics_for_tests();
    constexpr uint64_t kFrames = 150;
    constexpr auto kStageBDelay = std::chrono::milliseconds(2);

    ChainConfig config;
    config.pool_capacity = 16;
    config.queue_a = 4;
    config.queue_b = 4;
    config.queue_c = 8;
    Chain chain(config);
    chain.stage_b.SetProcessingDelay(kStageBDelay);  // bottleneck (load only)
    chain.Start();

    const ProducerResult produced =
        produce_frames(chain.pool, chain.stage_a, kFrames);
    require(!produced.borrow_timeout, "backpressure: pool should recycle");
    require(produced.handoff_failures == 0 && produced.produced == kFrames,
            "backpressure: every frame should enqueue");

    // Conservation bound, deterministic on any scheduler: at most
    // pool_capacity frames can be borrowed-but-not-recycled at once, and a
    // frame cannot recycle before the bottleneck stage processed it (StageB
    // holds a ref until then). So the producer's Nth borrow must wait for
    // StageB to have processed at least N - pool_capacity frames, each taking
    // >= kStageBDelay. Anything faster means backpressure did not happen.
    const auto min_elapsed =
        kStageBDelay * static_cast<int64_t>(kFrames - config.pool_capacity);
    require(produced.elapsed >= min_elapsed,
            "backpressure: producer should be throttled by the chain (took " +
                std::to_string(produced.elapsed.count()) + "ms, expected >= " +
                std::to_string(std::chrono::duration_cast<
                                   std::chrono::milliseconds>(min_elapsed)
                                   .count()) +
                "ms)");

    // Bounded growth: the input queues never exceeded their caps, and the
    // bottleneck's queue actually filled (the producer had to block).
    require(chain.stage_a.GetCountQueueInMax() <= config.queue_a &&
                chain.stage_b.GetCountQueueInMax() <= config.queue_b &&
                chain.sink_c.GetCountQueueInMax() <= config.queue_c,
            "backpressure: no input queue may exceed its bound");
    require(chain.stage_b.GetCountQueueInMax() == config.queue_b,
            "backpressure: the bottleneck's input queue should fill to its cap");

    const auto pool_stats = chain.pool.GetStats();
    require(pool_stats.high_water <= pool_stats.capacity,
            "backpressure: pool high water within capacity");

    chain.Drain(1);
    require_in_order(chain.sink_c.ReceivedFrames(), kFrames, "backpressure");
    require(chain.stage_a.GetCountQueueOutDropped() == 0 &&
                chain.stage_b.GetCountQueueOutDropped() == 0 &&
                chain.sink_c.GetCountQueueOutDropped() == 0,
            "backpressure: no losses");
    require_pool_recovered(chain.pool, "backpressure");

    chain.Stop();
    require_pool_recovered(chain.pool, "backpressure (after stop)");
    require_zero_ownership_diagnostics("backpressure");
}

// ---------------------------------------------------------------------------
// Scenario 3: forced overflow on a bounded output queue with a stalled sink
// consumer.
// ---------------------------------------------------------------------------

void test_forced_overflow_counts_drops_and_recycles()
{
    reset_worker_entry_release_diagnostics_for_tests();
    constexpr uint64_t kFrames = 300;
    constexpr int kOutCapacity = 4;

    ChainConfig config;
    config.pool_capacity = 32;
    Chain chain(config);
    // SinkC publishes to a bounded output queue; the external consumer never
    // pulls during the run (a wedged display), so overflow must drop-oldest.
    chain.sink_c.EnableBoundedOutputSink(kOutCapacity);
    chain.Start();

    const ProducerResult produced =
        produce_frames(chain.pool, chain.stage_a, kFrames);
    // Producer liveness despite the stalled consumer is itself part of the
    // contract: dropped entries MUST return to the pool or borrowing starves.
    require(!produced.borrow_timeout,
            "overflow: dropped entries must recycle so the producer stays live");
    require(produced.handoff_failures == 0 && produced.produced == kFrames,
            "overflow: every frame should enqueue");

    chain.Drain(1);

    // The sink processed everything; the drop accounting must be exact.
    require_in_order(chain.sink_c.ReceivedFrames(), kFrames, "overflow");
    require(chain.sink_c.GetCountQueueOutDropped() ==
                kFrames - static_cast<uint64_t>(kOutCapacity),
            "overflow: drop counter should equal frames minus capacity (got " +
                std::to_string(chain.sink_c.GetCountQueueOutDropped()) + ")");
    require(chain.sink_c.GetCountQueueOutSize() == kOutCapacity,
            "overflow: output backlog should sit exactly at the cap");

    // Only the survivors still hold pool entries; everything dropped is back.
    const auto mid_stats = chain.pool.GetStats();
    require(mid_stats.active == static_cast<size_t>(kOutCapacity),
            "overflow: only the queued survivors should hold pool entries (" +
                pool_stats_to_string(mid_stats) + ")");

    // Drop-oldest keeps the newest frames, still in FIFO order.
    std::vector<SyntheticFrame*> survivors;
    chain.sink_c.GetObjectsFromQueueOut(survivors);
    require(survivors.size() == static_cast<size_t>(kOutCapacity),
            "overflow: survivor count should match the cap");
    for (size_t i = 0; i < survivors.size(); ++i) {
        const uint64_t want = kFrames - static_cast<uint64_t>(kOutCapacity) +
                              static_cast<uint64_t>(i);
        require(survivors[i]->frame_id == want,
                "overflow: survivors should be the newest frames in order");
        chain.sink_c.ReleaseAsConsumer(survivors[i]);
    }

    require_pool_recovered(chain.pool, "overflow");
    chain.Stop();
    require_pool_recovered(chain.pool, "overflow (after stop)");
    require_zero_ownership_diagnostics("overflow");
}

// ---------------------------------------------------------------------------
// Scenario 4: mid-run worker failure (the CHECK-macro exception path).
// ---------------------------------------------------------------------------

void test_midrun_worker_failure_latches_and_drains()
{
    reset_worker_entry_release_diagnostics_for_tests();
    constexpr uint64_t kFrames = 200;
    constexpr int kThrowAfter = 50;  // frames StageB processes successfully

    ChainConfig config;
    config.pool_capacity = 16;
    Chain chain(config);
    chain.stage_b.SetThrowAfter(kThrowAfter);
    chain.Start();

    std::cout << "(scenario 4 injects " << (kFrames - kThrowAfter)
              << " worker exceptions; the [StageB] reports below are expected)"
              << std::endl;

    const ProducerResult produced =
        produce_frames(chain.pool, chain.stage_a, kFrames);
    // The failing stage must keep consuming (and recycling, via the RAII
    // guard's exception path) or the producer would starve here.
    require(!produced.borrow_timeout,
            "failure: pool must keep recycling through the exception path");
    require(produced.handoff_failures == 0 && produced.produced == kFrames,
            "failure: every frame should enqueue");

    chain.Drain(1);

    require(chain.stage_b.HasFatalError(),
            "failure: StageB's fatal-error latch should be set");
    require(!chain.stage_a.HasFatalError() && !chain.sink_c.HasFatalError(),
            "failure: healthy stages should not latch");
    require(chain.stage_b.IsMachineOn(),
            "failure: the failed worker should survive to drain, not die");
    require(chain.stage_b.Processed() == static_cast<int>(kFrames),
            "failure: StageB should still consume every frame while failing");

    // Only the frames processed before the injection point reach the sink,
    // and they are still in order.
    require_in_order(chain.sink_c.ReceivedFrames(),
                     static_cast<uint64_t>(kThrowAfter), "failure");

    // Every frame - forwarded or thrown - must be back in the pool.
    require_pool_recovered(chain.pool, "failure");

    chain.Stop();
    require_pool_recovered(chain.pool, "failure (after stop)");
    require_zero_ownership_diagnostics("failure");
}

// ---------------------------------------------------------------------------
// Scenario 5: flush-tick FIFO drain guarantee.
// ---------------------------------------------------------------------------

void test_flush_tick_drains_all_queued_frames_first()
{
    reset_worker_entry_release_diagnostics_for_tests();
    constexpr uint64_t kFrames = 100;

    ChainConfig config;
    config.pool_capacity = 128;  // producer never blocks on the pool
    config.queue_a = 16;
    config.queue_b = 16;
    config.queue_c = 16;
    Chain chain(config);
    // Slow every stage a little (load shaping) so frames are genuinely still
    // queued when the drain is requested; the FIFO guarantee being verified
    // must hold regardless.
    chain.stage_a.SetProcessingDelay(std::chrono::microseconds(200));
    chain.stage_b.SetProcessingDelay(std::chrono::microseconds(200));
    chain.sink_c.SetProcessingDelay(std::chrono::microseconds(200));
    chain.Start();

    const ProducerResult produced =
        produce_frames(chain.pool, chain.stage_a, kFrames);
    require(!produced.borrow_timeout && produced.handoff_failures == 0 &&
                produced.produced == kFrames,
            "flush: every frame should enqueue");

    // Request the drain immediately: the tick rides the same FIFO queues
    // behind the frames, so it must not overtake a single one of them.
    chain.Drain(1);

    const auto snapshots = chain.sink_c.FlushTickSnapshots();
    require(!snapshots.empty(), "flush: sink should observe the drain tick");
    require(snapshots.front() == kFrames,
            "flush: every queued frame must complete before finalization "
            "(sink had " + std::to_string(snapshots.front()) + " of " +
                std::to_string(kFrames) + " at tick delivery)");
    require_in_order(chain.sink_c.ReceivedFrames(), kFrames, "flush");
    require_pool_recovered(chain.pool, "flush");

    chain.Stop();
    require_pool_recovered(chain.pool, "flush (after stop)");
    require_zero_ownership_diagnostics("flush");
}

}  // namespace

int main()
{
    struct NamedTest {
        const char* name;
        void (*fn)();
    };
    const NamedTest tests[] = {
        {"steady_state_delivery_and_pool_recovery",
         test_steady_state_delivery_and_pool_recovery},
        {"backpressure_blocks_producer_without_loss",
         test_backpressure_blocks_producer_without_loss},
        {"forced_overflow_counts_drops_and_recycles",
         test_forced_overflow_counts_drops_and_recycles},
        {"midrun_worker_failure_latches_and_drains",
         test_midrun_worker_failure_latches_and_drains},
        {"flush_tick_drains_all_queued_frames_first",
         test_flush_tick_drains_all_queued_frames_first},
    };

    for (const NamedTest& test : tests) {
        try {
            test.fn();
            std::cout << "PASS " << test.name << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "FAIL " << test.name << ": " << e.what() << std::endl;
            return 1;
        }
    }

    std::cout << "synthetic_pipeline_tests passed" << std::endl;
    return 0;
}
