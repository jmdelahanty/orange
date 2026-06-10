# ThreadSanitizer Runbook

How to build and run the host concurrency tests under ThreadSanitizer (TSan),
why we do it, and a worked example of a real race it caught in this codebase.

## What TSan is

A data race in C++ is two threads accessing the same memory location, at least
one writing, with no happens-before relationship ordering them (no mutex, no
atomic, no thread join connecting the two accesses). The standard makes this
undefined behavior, and races are effectively invisible to normal testing: the
torn read may be harmless for months and corrupt a recording on someone else's
machine.

`-fsanitize=thread` recompiles the code so every memory access is instrumented.
At runtime TSan keeps shadow state for each memory word (which thread last
touched it, read or write, logical timestamp) and tracks happens-before edges
created by every synchronization operation (mutex lock/unlock, atomic
acquire/release, thread create/join). On each access it checks: has another
thread touched this location, at least one of us writing, with no
happens-before path between us? If so, it reports — with both stack traces and
the synchronization each thread held.

The key property: **TSan does not need the race to strike.** The two accesses
can be seconds apart in the actual run; it reports because the synchronization
*structure* is missing, not because the timing happened to collide. One
ordinary test run under TSan beats a thousand stress runs without it. The
corollary: it only analyzes code that executes, so coverage of concurrent
paths is what makes it useful — that is what `threadworker_tsan_stress` is for.

## Quick start

```bash
cmake --preset debug_tsan
cmake --build --preset debug_tsan -j8
ctest --test-dir targets/debug_tsan -R 'threadworker|worker_entry_ownership' --output-on-failure
```

A race produces a `WARNING: ThreadSanitizer: data race` report on stderr and a
failing test. A clean run is the absence of warnings.

The `debug_tsan` build preset builds **only the instrumented host test
targets** (no CUDA build):

| Target | What it exercises |
| --- | --- |
| `threadworker_tests` | `CThreadWorker` functional behavior, now race-checked |
| `threadworker_tsan_stress` | producer + worker + lock-free monitor reader, the real pipeline's cross-thread topology (see `tools/threadworker_tsan_stress.cpp`) |
| `worker_entry_ownership_core_host_tests` | refcounting semantics (single-threaded) |
| `worker_entry_ownership_tsan_hammer` | concurrent retain/release hammer: 8 threads × 50k iterations over a 4-entry pool, claim-based consume cycles through the zero-transition/recycle path; verifies the four refcounting invariants arithmetically (see `docs/threading_primer.md` §6-7) |
| `worker_entry_handoff_tsan_hammer` | retain_and_enqueue handoff across a real bounded `CThreadWorker` queue: 8 producers → 1 worker through a 4-slot queue; verifies accepted == processed, post-stop rejection auto-release, and exact ref balance over ~160k queue crossings. Its first run caught the legacy nullptr flush-tick contract, which led to the explicit `OnFlushTick()` API (see `threading_primer.md` §3) |

The concrete GPU-direct release path (`worker_entry_release.h`, which calls
`EVT_CameraQueueFrame` and pulls in the camera SDK) stays outside the TSan
suite deliberately — the SDK is an uninstrumented binary. Its generic
ownership logic is the same core the hammers above already cover.

The `ORANGE_ENABLE_TSAN` CMake option applies `-fsanitize=thread -g -O1` to
exactly these targets. CUDA- and SDK-linked targets are deliberately excluded:
TSan cannot instrument device code, and uninstrumented libraries produce noisy
or false reports.

## The ASLR gotcha (kernel >= 6.5)

On this machine's kernel, TSan dies at startup with:

```
FATAL: ThreadSanitizer: unexpected memory mapping 0x5de47ff4c000-...
```

Newer kernels randomize address space with more entropy than TSan's
shadow-memory layout tolerates. The fix is to disable ASLR for the test
process only, which the CTest registrations already do by launching tests
through `setarch <arch> -R`. If you run a binary by hand, do the same:

```bash
setarch $(uname -m) -R ./targets/debug_tsan/threadworker_tsan_stress
```

## Worked example: the `countQueueIn` race (found and fixed 2026-06-10)

The code reviews (`CODE_REVIEW_2026-06-09.md` §3.1) identified by inspection
that `CThreadWorker`'s tracing counters were plain `int`s mutated under
`mutexQueueIn` by the worker thread but read lock-free from other threads
(drain detection in `recording_ingress.cpp`, GUI instrumentation). The first
run of `threadworker_tsan_stress` proved it:

```
WARNING: ThreadSanitizer: data race
  Read of size 4 at 0x7fffffffda74 by thread T3:
    #0 GetCountQueueIn src/threadworker.h:41
  Previous write of size 4 by thread T1 (mutexes: write M10):
    #0 WaitForObjectFromQueueIn src/threadworker.h:304
SUMMARY: ThreadSanitizer: data race src/threadworker.h:41 in GetCountQueueIn
```

How to read it: the writer held mutex M10 (`mutexQueueIn` — TSan traces where
the mutex was created); the reader held nothing. That asymmetry is the entire
diagnosis. TSan also reported a second race we had not flagged in review:
`GetCountQueueInMax` at `threadworker.h:45`.

The fix: the five tracing counters became `std::atomic<int>`. They are still
mutated under the queue mutexes as before; atomics make the lock-free readers
legal and tear-free. One wrinkle worth remembering: `std::atomic` deletes
atomic-to-atomic copy assignment (`a = b` would be two separate atomic
operations, not one), so the high-water update reads explicitly:

```cpp
countQueueInMax = countQueueIn.load();
```

After the fix, the same stress run is clean: ~84k items processed while a
monitor thread read the counters ~12M times, zero reports.

Upstream note: `JohnsonLabJanelia/orange` has the same two races in its
`threadworker.cpp`. This fix plus the TSan before/after is a candidate first
PR (see `UPSTREAMING_PATH_2026-06-09.md` §3).

## Limitations

- **Coverage-bound**: TSan only sees executed code. A race on a path no test
  exercises goes unreported. New cross-thread interactions need a stress test
  that drives both sides concurrently.
- **Host only**: no instrumentation of CUDA device code or GPU-side races;
  the CUDA runtime itself can generate noise. Keep TSan targets GPU-free.
- **Understands `std::atomic`**: correctly-synchronized lock-free code (e.g.
  the CAS loops in `worker_entry_ownership_core.h`) does not false-positive.
  Hand-rolled synchronization TSan cannot see (inline-asm fences,
  synchronization via fd/pipes) can.
- **Cost**: roughly 5–15x slowdown and several times the memory. Fine for the
  small host tests; do not enable for the full application build.
- **Incompatible with ASan** in the same binary; a future AddressSanitizer
  preset would be a separate job.

## Roadmap

1. **CI job** — run the `debug_tsan` preset + ctest on push so new code in the
  instrumented components is race-checked automatically (CI itself is still
  pending; see `UPSTREAMING_PATH_2026-06-09.md` §4).
2. Extend the instrumented target list as more pipeline components grow
  host-only tests (recording_ingress drain logic is the next candidate).
