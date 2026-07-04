# Error Handling Convention

One rule above all: **library code never calls `exit()`, `abort()`, or
`LOG(FATAL)`-that-exits. Process exit decisions belong to `main()`.**
Everything below follows from where the code sits in the pipeline lifecycle.

## 1. Construction / configuration time: throw

Anything that runs before the realtime loop starts — constructors, `Initialize()`
methods, config parsing, file/socket/encoder session opening, CUDA
stream/event/pool allocation — reports failure by **throwing a typed
exception** (`std::runtime_error` or a subclass such as `cuda_error` in
`src/common.hpp`). Do not print-and-continue, do not return `void` with a
latent "not open" flag. A half-constructed object must not escape: clean up
partial state, then throw. `FFmpegWriter`'s constructor and
`SharedRecordingOutput::open_if_needed()` are the reference examples.

## 2. Realtime loop (per-frame): error codes + counters + rate-limited logs

Inside a hot loop (acquisition, preprocess, encode, write), an *expected*
per-frame condition — queue overflow, dropped frame, late packet — must
**never throw**. Instead:

- return `false`/an error code so the caller can recycle the frame,
- bump an atomic counter (`std::atomic<uint64_t>`) so the condition is
  observable (`FFmpegWriter::queue_overflow_events()` is the pattern),
- log **rate-limited** (first occurrence, then every Nth, or "suppressing
  further reports" like `COffThreadMachine::NoteWorkerException`).

Truly unexpected failures (a CUDA call failing mid-loop) may throw — the
worker thread boundary below is designed to absorb exactly that.

## 3. Worker thread boundaries: catch / latch / drain (already implemented)

Every thread entry point catches everything, logs once (rate-limited), latches
a fatal flag, and lets the pipeline drain instead of dying. An exception
escaping a thread would `std::terminate` the whole process — no NVENC flush,
no MP4 trailer. Reference implementations:

- `CThreadWorker::ThreadRunning` (`src/threadworker.h`) — per-item catch around
  `WorkerFunction`/`OnFlushTick`, latches via `NoteWorkerException`.
- `COffThreadMachine::MachineThread` (`src/offthreadmachine.cpp`) — outer
  last-line-of-defence catch, latches `fatalError` and stops the thread.
- `FFmpegWriter::write_thread` (`src/FFmpegWriter.cpp`) — latches
  `writer_thread_error_`, owner can still finalize the container.
- The split-harvest thread in `EncoderHwWorker`, and the `acquire_frames`
  camera thread wrapper (`src/acquire_frames.cpp`).

If you spawn a raw `std::thread`, you own giving it this boundary.

## 4. Process exit: only `main()`

`main()` (in `orange.cpp` / `orange_headless_client.cpp`) validates arguments
and environment and may `return EXIT_FAILURE` or `std::exit()`. Nothing else
may. The one exception: a **forked child** between `fork()` and a failed
`exec()` must call `_exit(127)` — throwing or returning there would run the
parent's code twice (see `external_recorder_supervisor.cpp`, `project.cpp`).

Vendored third-party code (`src/NvEncoder/`, `src/json.hpp`) is patched only
where its fatal path would kill the process (`Logger.h` FATAL now throws);
keep such diffs surgical.

## How to add error handling to new code

```cpp
class FrameResizer {
public:
    // Construction: throw on anything that prevents a usable object.
    explicit FrameResizer(int width, int height) {
        if (width <= 0 || height <= 0) {
            throw std::runtime_error("FrameResizer: invalid dimensions");
        }
        CHECK(cudaMalloc(&d_scratch_, buffer_bytes(width, height))); // throws cuda_error
    }

    // Realtime path: report per-frame failure via return value + counter.
    // Never throw for an expected condition.
    bool resize(const Frame& in, Frame* out) noexcept {
        if (in.empty()) {
            const uint64_t n = empty_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n == 1 || n % 1000 == 0) {                 // rate-limited log
                std::cerr << "[FrameResizer] empty input frames: " << n << "\n";
            }
            return false;                                   // caller recycles the frame
        }
        // ... kernel launch; a failing CUDA call throws and is absorbed by
        // the worker thread boundary (catch/latch/drain), not by this code.
        return true;
    }

    uint64_t empty_frames() const { return empty_frames_.load(std::memory_order_relaxed); }

private:
    void* d_scratch_ = nullptr;
    std::atomic<uint64_t> empty_frames_{0};
};
```

Wiring it in: construct at configuration time inside the existing worker init
(throw propagates to the worker boundary or to `main()`'s catch); call
`resize()` per frame and treat `false` as "drop and recycle"; surface the
counter in the perf/status reporting if operators need to see it.
