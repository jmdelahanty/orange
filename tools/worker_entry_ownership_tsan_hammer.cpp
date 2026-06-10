// Concurrent retain/release hammer for worker_entry_ownership_core.h.
//
// A hammer test does not check a scenario; it pounds an invariant from many
// threads at once to manufacture the interleavings where races live (see
// docs/threading_primer.md §7). This one drives N threads through tight
// retain → touch payload → release loops over a small shared entry pool,
// with a claim-based "consume" protocol so entries continually cycle through
// the zero-transition → recycle → reseed path under contention.
//
// Invariants verified (numbering from docs/threading_primer.md §6):
//   1. Count accuracy        — every entry back to baseline after join
//   2. Unique zero-transition — recycle cycles == successful consume claims
//   3. No touch-after-release / 4. payload visibility — non-atomic per-thread
//      payload slots written while holding a ref and read by the recycler;
//      ThreadSanitizer proves the ordering (a too-weak refcount ordering
//      reports here even though every counter stays correct)
//   Plus: the core's own diagnostics become *expected* values — failed
//   retains during the dead window must equal retain_after_release exactly,
//   and double_release/underflow must be zero.
//
// Normal usage (see docs/thread_sanitizer_runbook.md):
//   cmake --preset debug_tsan && cmake --build --preset debug_tsan
//   ctest --test-dir targets/debug_tsan -R worker_entry_ownership_tsan_hammer
//
// Note: the core logs "retain_after_release" lines to stderr at counts
// 1..16 and powers of two. In this test those rejections are an expected
// part of the protocol (retry window between final release and reseed),
// not failures.

#include "worker_entry_ownership_core.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr int kThreads = 8;
constexpr int kIterationsPerThread = 50000;
constexpr int kEntries = 4;          // small pool -> high contention
constexpr int kConsumeEvery = 16;    // try to consume on every Nth success
constexpr int kLeaseEvery = 4;       // exercise the lease API on every Nth try

struct HammerEntry {
    std::atomic<int> ref_count{0};

    // Optional debug fields the core's logging picks up via SFINAE.
    uint64_t frame_id = 0;
    uint64_t recording_frame_id = 0;
    uint64_t camera_frame_id = 0;

    // One consume (extra release of the alive token) per cycle; the atomic
    // claim guarantees a unique consumer so the protocol itself never
    // double-releases.
    std::atomic<bool> consume_claimed{false};

    // Non-atomic payload, deliberately. Each holder writes only its own
    // slot (no holder/holder race); the recycler reads all slots. The
    // refcount's acq_rel ordering is the only thing making that legal —
    // exactly what TSan is here to check.
    std::array<uint64_t, kThreads> marks{};

    uint64_t cycles = 0;  // written only by the unique zero-transition winner
};

struct ThreadTally {
    uint64_t marks_written = 0;
    uint64_t consumes_won = 0;
    uint64_t failed_retains = 0;
};

std::atomic<uint64_t> g_harvested_marks{0};

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

// Runs in whichever thread performed the final release. Reads the payload
// (invariant 4), records the cycle (its exclusivity is invariant 2), then
// reseeds the entry so retains can succeed again.
void recycle_and_reseed(HammerEntry* entry)
{
    uint64_t sum = 0;
    for (auto& slot : entry->marks) {
        sum += slot;
        slot = 0;
    }
    g_harvested_marks.fetch_add(sum, std::memory_order_relaxed);
    entry->cycles += 1;
    entry->consume_claimed.store(false, std::memory_order_relaxed);
    // Reseed last: this release-store publishes everything above to the
    // next thread whose retain CAS acquires it.
    entry->ref_count.store(1, std::memory_order_release);
}

void hammer_thread(int tid, std::array<HammerEntry, kEntries>* pool,
                   ThreadTally* tally)
{
    const WorkerEntryReleaseContext ctx{"hammer-cam", "HammerThread"};
    const auto release_fn = [](HammerEntry* e,
                               const WorkerEntryReleaseContext& c) {
        release_worker_entry_ref(e, c, recycle_and_reseed);
    };

    uint32_t rng = 0x9e3779b9u ^ static_cast<uint32_t>(tid);
    for (int i = 0; i < kIterationsPerThread; ++i) {
        rng = rng * 1664525u + 1013904223u;  // LCG; no shared rand() state
        HammerEntry* entry = &(*pool)[rng % kEntries];

        const bool via_lease = (i % kLeaseEvery) == 0;
        bool held = false;
        if (via_lease) {
            auto lease =
                try_retain_worker_entry_ref_lease(entry, ctx, release_fn);
            if (lease) {
                held = true;
                entry->marks[tid] += 1;  // payload write while holding a ref
                tally->marks_written += 1;
                // lease destructor releases (guard RAII path)
            }
        } else if (retain_worker_entry_ref(entry, ctx)) {
            held = true;
            entry->marks[tid] += 1;
            tally->marks_written += 1;

            if ((tally->marks_written % kConsumeEvery) == 0 &&
                !entry->consume_claimed.exchange(true,
                                                 std::memory_order_acq_rel)) {
                // Unique consumer this cycle: drop the alive token. We still
                // hold our own ref, so the count stays >= 1 here; the final
                // holder (us or another thread) triggers the recycle.
                release_worker_entry_ref(entry, ctx, recycle_and_reseed);
                tally->consumes_won += 1;
            }

            release_worker_entry_ref(entry, ctx, recycle_and_reseed);
        }

        if (!held) {
            // Retain refused: the entry was in its dead window (count == 0
            // between final release and reseed). The core counts this as
            // retain_after_release — we tally it and assert exact agreement.
            tally->failed_retains += 1;
        }
    }
}

}  // namespace

int main()
{
    reset_worker_entry_release_diagnostics_for_tests();

    static std::array<HammerEntry, kEntries> pool;
    for (size_t i = 0; i < pool.size(); ++i) {
        pool[i].frame_id = i;
        pool[i].ref_count.store(1, std::memory_order_release);  // alive token
    }

    std::array<ThreadTally, kThreads> tallies{};
    {
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back(hammer_thread, t, &pool, &tallies[t]);
        }
        for (auto& th : threads) {
            th.join();
        }
    }

    uint64_t total_marks = 0;
    uint64_t total_consumes = 0;
    uint64_t total_failed_retains = 0;
    for (const auto& tally : tallies) {
        total_marks += tally.marks_written;
        total_consumes += tally.consumes_won;
        total_failed_retains += tally.failed_retains;
    }

    // Invariant 1: count accuracy — every entry back to baseline.
    uint64_t residual_marks = 0;
    uint64_t total_cycles = 0;
    for (auto& entry : pool) {
        check(entry.ref_count.load() == 1, "entry refcount at baseline",
              static_cast<uint64_t>(entry.ref_count.load()), 1);
        check(!entry.consume_claimed.load(), "consume claim cleared",
              entry.consume_claimed.load() ? 1 : 0, 0);
        for (uint64_t m : entry.marks) {
            residual_marks += m;
        }
        total_cycles += entry.cycles;
    }

    // Invariant 2: unique zero-transition — one recycle per consume, no
    // extras, none lost.
    check(total_cycles == total_consumes, "cycles == consumes won",
          total_cycles, total_consumes);

    // Invariant 4 arithmetic: every payload write is either harvested by a
    // recycle or still present in an entry. (TSan separately proves the
    // visibility ordering on the marks themselves.)
    const uint64_t harvested = g_harvested_marks.load();
    check(harvested + residual_marks == total_marks,
          "marks harvested + residual == written",
          harvested + residual_marks, total_marks);

    // The core's diagnostics as *expected* values: the protocol never
    // misuses the API, so the only diagnostic that may fire is
    // retain_after_release — exactly once per failed retain.
    check(worker_entry_release_double_release_count().load() == 0,
          "no double releases",
          worker_entry_release_double_release_count().load(), 0);
    check(worker_entry_release_underflow_count().load() == 0,
          "no release underflows",
          worker_entry_release_underflow_count().load(), 0);
    check(worker_entry_retain_after_release_count().load() ==
              total_failed_retains,
          "retain_after_release == failed retains",
          worker_entry_retain_after_release_count().load(),
          total_failed_retains);

    std::printf(
        "hammer: threads=%d iters=%d entries=%d | marks=%llu cycles=%llu "
        "consumes=%llu failed_retains=%llu harvested=%llu residual=%llu\n",
        kThreads, kIterationsPerThread, kEntries,
        static_cast<unsigned long long>(total_marks),
        static_cast<unsigned long long>(total_cycles),
        static_cast<unsigned long long>(total_consumes),
        static_cast<unsigned long long>(total_failed_retains),
        static_cast<unsigned long long>(harvested),
        static_cast<unsigned long long>(residual_marks));

    if (g_failures != 0) {
        std::fprintf(stderr, "worker_entry_ownership_tsan_hammer: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::printf("worker_entry_ownership_tsan_hammer: all invariants held\n");
    return 0;
}
