#pragma once

// Producer-side media content digests ("receipts") for the receipt-backed
// sealer (Citrus >= 2.1.0, approved 2026-10-09).
//
// After a video file is finalized (trailer written, playback-intent patch
// applied, finalization sidecar persisted) the recorder hands its path to a
// ContentDigestHasher. The hasher runs on one SCHED_IDLE / nice 19 thread per
// recorder process, reads the closed file back in chunks with plain read()
// (no mmap, no fadvise) into a streaming SHA-256 and writes
// `<video>.content_digest.json` beside the file via temp + rename, in the
// closed format of schema orange.media_content_digest version 1.
//
// Hard rule: receipts never delay or disturb capture or writing. try_enqueue
// never blocks (a full queue abandons that file's receipt); the writer never
// waits for a digest; finish() waits a bounded time after the recorder's own
// finalization and abandons the rest; every abandonment is a counted, normal
// outcome. A missing receipt only means the sealer hashes that file itself.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace orange::media_digest {

struct ReceiptRequest {
    std::string video_path;        // absolute path of the finalized video
    std::string recording_root;    // run folder holding recording_session.json
    std::string session_id;        // recording_session.json session_id
};

struct ReceiptCounters {
    uint64_t requested = 0;   // try_enqueue calls
    uint64_t written = 0;     // receipt files in place
    uint64_t abandoned = 0;   // queue full, finish() timeout, or shutdown
    uint64_t failed = 0;      // hashing / writing error (no receipt left behind)
};

inline std::string receipt_path_for(const std::string& video_path)
{
    return video_path + ".content_digest.json";
}

// Hash the file and write its receipt synchronously (the hasher thread calls
// this; tests call it directly). Never leaves a partial receipt.
bool write_content_digest_receipt(const ReceiptRequest& request,
                                  std::string* sha256_out,
                                  std::string* error_out);

class ContentDigestHasher {
public:
    explicit ContentDigestHasher(size_t queue_capacity = 64);
    ~ContentDigestHasher();

    ContentDigestHasher(const ContentDigestHasher&) = delete;
    ContentDigestHasher& operator=(const ContentDigestHasher&) = delete;

    // Starts the idle-priority thread. Idempotent.
    void start();

    // Never blocks. Returns false (and counts an abandonment) when the queue
    // is full or the hasher is finishing / not started.
    bool try_enqueue(ReceiptRequest request);

    // Stops accepting work, waits up to `bounded_wait` for the queue to drain,
    // abandons whatever is left, joins the thread. Idempotent.
    void finish(std::chrono::milliseconds bounded_wait);

    ReceiptCounters counters() const;
    size_t queue_capacity() const { return queue_capacity_; }
    bool started() const { return started_.load(std::memory_order_acquire); }

private:
    void run();

    const size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ReceiptRequest> queue_;
    std::thread thread_;
    bool accepting_ = false;
    bool stop_ = false;
    bool busy_ = false;
    std::atomic<bool> started_{false};
    bool finished_ = false;
    ReceiptCounters counters_;
};

}  // namespace orange::media_digest
