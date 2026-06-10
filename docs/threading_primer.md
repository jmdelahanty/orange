# Threading Primer — concurrency concepts as they appear in this codebase

A teaching document, not a reference manual. Every concept here is illustrated
with code that actually ships in this repo, including two real bugs we found
and fixed. Companion docs: `thread_sanitizer_runbook.md` (how to run TSan),
`CODE_REVIEW_2026-06-09.md` (where these issues were first identified).

---

## 1. The golden rule

> **Every variable touched by more than one thread is protected by a mutex,
> is a `std::atomic`, or is a bug.** There is no fourth category.

The C++ memory model defines a *data race* as: two threads access the same
memory location, at least one access is a write, and no synchronization orders
them. A data race is undefined behavior — not "you might read a stale value"
but "the compiler is allowed to assume this never happens and optimize
accordingly." Intuition imported from Python or Java ("it's just an int,
torn reads are harmless") does not transfer; in C++ the program is wrong even
if it appears to work.

**The local case study.** `CThreadWorker`'s tracing counters were plain
`int`s, incremented under `mutexQueueIn` by the worker thread but read with no
lock by drain checks (`recording_ingress.cpp`) and GUI instrumentation. That
is a textbook race — writer synchronized, reader not — and it survived in this
codebase (and upstream, where it still exists) for years because it almost
never misbehaves visibly. ThreadSanitizer flagged it on the first run of a
300 ms stress test. The fix made the counters `std::atomic<int>`
(`src/threadworker.h:86-92`). Details and the actual TSan report:
`thread_sanitizer_runbook.md`.

## 2. Happens-before: the one relationship that matters

All synchronization reduces to one question: *does the write happen-before the
read?* The tools below exist to create happens-before edges:

| Tool | Edge it creates | Use it for |
| --- | --- | --- |
| `std::mutex` + `lock_guard` | unlock in thread A → next lock in thread B | protecting a group of related variables / multi-step invariants |
| `std::atomic` store/load | release-store in A → acquire-load in B (that reads it) | single flags, counters, refcounts, publish pointers |
| `std::thread::join()` | everything in the joined thread → everything after join | shutdown, collecting results |
| condition variable | (built on its mutex) | waiting for a state change without spinning |

If you can't name which row creates the edge between a write and a read, you
have the bug from §1.

**Group rule of thumb:** a mutex protects *invariants spanning several
variables* ("queue contents and its count agree"); an atomic protects *one
value that is individually meaningful*. Making five variables individually
atomic does not make a five-variable invariant safe — between your atomic
reads, another thread may have changed two of them.

## 3. Condition variables, done correctly

`src/threadworker.h` is the house example of the correct pattern:

```cpp
queueInNotEmptyCv.wait(lock, [this]() {
    return !queueIn.empty() || stopRequested;   // the predicate
});
```

Three rules, all visible in that snippet:

1. **Always wait with a predicate.** CVs can wake spuriously (no one
   notified); the predicate re-check makes that harmless. A bare `wait()` is
   almost always a bug.
2. **The predicate includes the shutdown flag.** Otherwise a thread blocked on
   an empty queue can never be stopped — `DoStopThread()` sets
   `stopRequested` and notifies precisely so waiters re-evaluate and exit.
3. **Mutate the predicate's variables under the same mutex the CV uses.**
   The CV's guarantee is only as good as the mutex coupling.

Contrast with the upstream ancestor (`~/orange/src/threadworker.cpp`), which
has no CV at all — it polls with `usleep(interval)`. That costs latency
(up to one sleep interval per item) and CPU, which is why the fork's rewrite
replaced it.

**Ordered flush markers (in-band end-of-stream).** The recording drain
cascade sends a marker *through the worker queues* (`EnqueueFlushTick()`,
`threadworker.h`): because the marker rides the FIFO queue, a worker
processes every frame ahead of it first, then runs its flush housekeeping
(`OnFlushTick()` — e.g. PoseWorker closes its event log). This is the
standard stream-processing way to say "finish what you have, then finalize"
— an out-of-band flag could fire before queued frames were processed.
Historically the marker was a raw `nullptr` delivered to
`WorkerFunction`, which every subclass had to remember to null-check (a
footgun a hammer test tripped over); the explicit `OnFlushTick()` hook now
carries that contract, though several drain-coupled legacy workers
(crop_producer, recording_ingress, the encoder pair, crop_and_encode,
gpu_video_encoder) still use the nullptr convention via the
backward-compatible default.

## 4. Memory ordering: what the `std::memory_order_*` arguments mean

Every atomic operation defaults to `memory_order_seq_cst` — globally
sequentially consistent, always correct, occasionally slower than necessary.
The weaker orderings are *contracts about what other memory the operation
vouches for*:

- **relaxed** — the atomic value itself is exact, but the operation says
  *nothing* about any other memory. Safe for pure statistics counters.
- **release** (on a store) — "everything I wrote before this store is visible
  to whoever acquire-reads it."
- **acquire** (on a load) — "after this load, I can see everything the
  releasing thread wrote before its store."
- **acq_rel** (on read-modify-write) — both at once.

**The trap that makes ordering matter — counter right, payload wrong.**
Imagine a refcount decremented with `relaxed`. The count is still perfectly
accurate (atomics never lose updates regardless of ordering). But the thread
that sees the count hit zero and recycles the frame has *no guarantee it can
see the pixel data other threads wrote while holding their references*. Every
counter in the system reads correct; the frame is torn. This is why
`std::shared_ptr`'s control block uses acquire-release on its decrement, and
it is the subtlest idea in this document: **memory ordering bugs hide behind
correct-looking counts.**

**The local worked example.** `src/worker_entry_ownership_core.h` gets this
right:

```cpp
// release_worker_entry_ref, line ~375
if (entry->ref_count.compare_exchange_weak(
        current, desired,
        std::memory_order_acq_rel,      // success ordering
        std::memory_order_acquire)) {   // failure ordering
    if (desired != 0) return false;
    break;
}
...
recycle_final(entry);                    // line ~387
```

Because every decrement is `acq_rel`, the decrements form a release sequence,
and the *final* decrement (the one that wins the race to zero) happens-after
every earlier holder's writes. That is the precise property that makes it
legal for `recycle_final` to read and reuse the entry. If those orderings
were `relaxed`, the counts would still be exact and the code would still
"pass" every functional test — only TSan (or a corrupted recording at 2am)
would tell you.

## 5. CAS loops: lock-free read-modify-write

`compare_exchange_weak(expected, desired)` atomically performs: "if the value
still equals `expected`, set it to `desired`; otherwise write the actual value
into `expected` and report failure." The standard loop, from
`retain_worker_entry_ref` (`worker_entry_ownership_core.h:294-321`):

```cpp
int current = entry->ref_count.load(std::memory_order_acquire);
while (true) {
    if (current <= 0) { /* refuse: entry already dead */ return false; }
    if (entry->ref_count.compare_exchange_weak(current, current + 1, ...))
        return true;
    // CAS failed: another thread won; `current` was refreshed — loop retries.
}
```

Two teaching points:

1. **Why a loop instead of `fetch_add`?** Because the update is conditional —
   retain must *refuse* when the count is already ≤ 0 (the "resurrection
   guard": you may not revive an entry whose last reference died, since the
   recycler may already be reusing it). `fetch_add` can't express
   check-then-modify atomically; CAS can.
2. **`weak` vs `strong`:** `weak` may fail spuriously (report failure even
   when the value matched) but is cheaper on some architectures. Inside a
   retry loop spurious failure is harmless — you loop again — so `weak` is
   the idiomatic choice. Use `strong` only for one-shot, non-looping CAS.

A related gotcha you will hit writing atomics code: `std::atomic` **deletes
copy-assignment between atomics** (`a = b` would be two separate atomic
operations with a gap — not one atomic action). Hence
`countQueueInMax = countQueueIn.load();` in `threadworker.h`. The compile
error is C++ forcing the code to say what actually happens.

## 6. Refcounting: the four invariants

Reference counting (our `WORKER_ENTRY` lifecycle, `shared_ptr`, Python
objects) promises four distinct things. They fail in different ways, so name
them separately:

1. **Count accuracy** — K retains + K releases return the count to baseline.
   No update is ever lost, regardless of contention.
2. **Unique zero-transition** — exactly one thread observes the final
   decrement, and only that thread recycles. (Two winners = frame returned to
   the camera pool twice.)
3. **No touching after your own release** — your release may have been the
   last; the entry may already hold the next frame. Read-after-release is a
   use-after-free that usually reads *plausible* data.
4. **Payload visibility at recycle** — the recycling thread sees every write
   every holder made (§4's ordering trap).

`worker_entry_ownership_core.h` additionally turns violations into
*observable diagnostics* rather than silent corruption: global atomic counters
for `double_release`, `release_underflow`, and `retain_after_release`
(lines 35-51), with deduplicated logging (log at counts 1..16, then powers of
two — line 172). RAII wrappers (`WorkerEntryRefGuardCore`, line 409) make
"every retain has exactly one release" structural: the destructor releases
unless `Dismiss()` recorded an ownership transfer. Prefer the guard/lease API
over raw retain/release in new code — it converts a discipline into a type.

## 7. Testing concurrent code: scenarios vs hammers

A *scenario test* checks behavior: "retain then release recycles the entry."
Necessary, nowhere near sufficient — races live in the interleavings, and a
scenario test exercises one.

A *hammer test* pounds an invariant from many threads simultaneously to
manufacture collisions: N threads × M iterations of retain → touch payload →
release on shared entries, then verify the invariants arithmetically, run
under TSan. Each §6 invariant has a distinct failure signature:

| Invariant broken | What the hammer shows |
| --- | --- |
| count accuracy | final counts ≠ baseline |
| unique zero-transition | recycle count > cycle count |
| touch-after-release | TSan race report on the payload (or corrupted check sums) |
| payload visibility | TSan race report on payload *at the recycle site*, counters all correct |

The two halves are complementary: the hammer generates the collisions; TSan
proves whether the synchronization structure survives them. TSan checks the
happens-before *structure*, so it does not need the unlucky timing to actually
occur — one hammer run under TSan beats a thousand without it. (See
`thread_sanitizer_runbook.md`; the hammer for the ownership core lives in
`tools/worker_entry_ownership_tsan_hammer.cpp`.)

## 8. Checklist for new concurrent code in this repo

Before merging code that adds a thread, a shared variable, or a queue:

- [ ] Every shared variable: mutex, atomic, or justified in a comment why
      neither (there is almost never a valid justification).
- [ ] Every CV wait has a predicate; the predicate includes the stop flag;
      the worker's `DoStopThread()` wakes every blocking point it adds.
- [ ] New `CThreadWorker` subclasses override `OnFlushTick()` (even if empty)
      for drain/shutdown housekeeping — with the override in place,
      `WorkerFunction` never receives nullptr. Do not null-check your way
      around it; that is the legacy convention being retired (§3).
- [ ] Cross-thread handoffs use the guard/lease API, not raw retain/release.
- [ ] Bounded queues only; define and count the overflow policy (see
      `CODE_REVIEW_2026-06-09.md` §3.2-3.3 — unbounded growth and silent
      drops are both data-integrity bugs for an instrument).
- [ ] If it has cross-thread interaction, it has a test that exercises that
      interaction concurrently, and the test runs under the `debug_tsan`
      preset.
- [ ] No `cudaDeviceSynchronize`/blocking I/O while holding a lock another
      thread needs.

## 9. Further study

- *C++ Concurrency in Action* (Anthony Williams) — the standard text; ch. 5
  is the memory model explained properly.
- Herb Sutter, *atomic<> Weapons* (talk, 2 parts) — the best deep dive on
  acquire/release and why the hardware makes it this way.
- Boost's `shared_ptr` documentation on the control-block decrement — the
  canonical statement of §4's trap.
- The upstream repo's `epoch/seq` idempotency protocol
  (`COMPARATIVE_REVIEW_2026-06-09.md` §3.1) — the same happens-before
  thinking applied across machines instead of threads.
