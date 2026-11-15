// Per-camera empty-dish background burst (docs/empty_background_capture_design.md,
// "Burst"; Jeremy 2026-11-15, option 2).
//
//  - Reserve(), once at stream start and never mid-stream: a 17-frame staging area
//    on the camera's landing die (346 MB at 4512 x 4512, 137 MB at 4512 x 1792),
//    the median buffer and a pinned host copy of it. A camera whose reservation
//    failed refuses every burst, with the reason; the stream itself is unaffected.
//  - Request() (GUI thread): the next 17 consecutive frames. Refused while a
//    recording is armed, recording or finalizing (the caller checks) and while a
//    burst is outstanding.
//  - Acquisition thread, per frame, only while WantsFrame(): OfferFrame() retains
//    the pool entry and queues it here (TryPutObjectToQueueIn, never blocking).
//    Nothing else changes on the frame path: the GPUDirect ring buffer is
//    returned exactly as it is without a burst.
//  - This worker (its own thread): waits for the frame's owned copy (the late
//    owned copy after detect on the analytics path, d_analytics_image after
//    analytics_ready_event; the pool ring copy d_image_pool after event_ptr when
//    detection is off), copies it device to device into the next staging slot on
//    its own lowest-priority stream, waits for that copy and releases the entry.
//    It never reads the camera's GPUDirect buffer. After frame 17 the median
//    kernel runs on the die and only the median (one frame) is copied to host.
//  - PopResult() (GUI thread): the median, its saturated count and the 17 source
//    frames' identities, plus the lowest free pool entry / readiness event counts
//    the acquisition thread saw during the burst (rig check evidence). PNG,
//    statistics and the descriptor are written by the caller off this thread and
//    off the acquisition, YOLO and pose threads.
#pragma once

#include "empty_background.h"
#include "threadworker.h"
#include "video_capture.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct EmptyBackgroundBurstResult {
    std::uint64_t request_id = 0;
    std::string camera_serial;
    bool ok = false;
    std::string failure_reason;  // set when !ok
    int width = 0;
    int height = 0;
    int gpu_id = -1;
    std::vector<orange::empty_background::SourceFrame> source_frames;  // in stream order
    orange::empty_background::BurstMedian median;                       // when ok
    // Lowest free counts the acquisition thread saw when it handed a burst frame
    // over (-1 when no frame was offered). The pool must never run dry because of
    // a burst lease (orange, 2026-11-15: presets use acquire_work_entries 64).
    int min_free_entries = -1;
    int min_free_events = -1;
    std::uint32_t max_queue_depth = 0;  // frames waiting here when one was queued
    double burst_ms = 0.0;   // first frame offered -> last slot copy done
    double median_ms = 0.0;  // median kernel + device-to-host copy of the median
};

class EmptyBackgroundBurstWorker : public CThreadWorker<WORKER_ENTRY> {
public:
    static constexpr std::uint32_t kFrames = static_cast<std::uint32_t>(orange::empty_background::kFrameCount);
    // Frames may wait here at most this deep; the 18th would be refused and fail the burst.
    static constexpr std::size_t kMaxQueuedFrames = kFrames;

    EmptyBackgroundBurstWorker(const char* name, CameraParams* camera_params,
                               SafeQueue<WORKER_ENTRY*>& recycle_queue);
    ~EmptyBackgroundBurstWorker() override;

    EmptyBackgroundBurstWorker(const EmptyBackgroundBurstWorker&) = delete;
    EmptyBackgroundBurstWorker& operator=(const EmptyBackgroundBurstWorker&) = delete;

    // Stream start, before StartThread: allocate on camera_params->gpu_id for its
    // configured width x height Mono8. False with the reason (also kept in
    // reserve_error()) when the raster is unsupported or the allocation fails.
    bool Reserve(std::string* error_out);
    // Stream teardown, after the thread stopped. Also run by the destructor.
    void ReleaseReservation() noexcept;
    bool reserved() const noexcept { return reserved_; }
    std::string reserve_error() const;
    std::uint64_t reserved_bytes() const noexcept { return reserved_bytes_; }

    // GUI thread. False with the reason when not reserved or a burst is outstanding.
    bool Request(std::uint64_t* request_id_out, std::string* error_out);
    // GUI thread: ends an outstanding burst as failed (timeout, stream stop).
    void Abort(const std::string& reason);
    bool Busy() const;
    bool PopResult(EmptyBackgroundBurstResult* out);

    // Acquisition thread. WantsFrame() is one relaxed atomic load.
    bool WantsFrame() const noexcept { return claiming_.load(std::memory_order_acquire); }
    // Hands this frame to the burst. `recording` true ends the burst as failed
    // instead (a burst never overlaps a recording). Never blocks.
    void OfferFrame(WORKER_ENTRY* entry, bool recording, int free_entries_available,
                    int free_events_available);

protected:
    bool WorkerFunction(WORKER_ENTRY* entry) override;
    void OnFlushTick() override {}

private:
    void fail_locked(const std::string& reason);
    void complete_locked();
    bool copy_into_slot(const WORKER_ENTRY& entry, std::uint32_t slot, std::string* error);
    bool run_median(std::uint64_t request_id, std::string* error);

    CameraParams* camera_params_ = nullptr;
    SafeQueue<WORKER_ENTRY*>* recycle_queue_ = nullptr;

    // Reservation (stream start / teardown only).
    bool reserved_ = false;
    std::string reserve_error_;
    int gpu_id_ = -1;
    int width_ = 0;
    int height_ = 0;
    std::size_t frame_bytes_ = 0;
    std::uint64_t reserved_bytes_ = 0;
    std::uint8_t* d_slots_ = nullptr;     // kFrames * frame_bytes_
    std::uint8_t* d_median_ = nullptr;    // frame_bytes_
    unsigned long long* d_saturated_ = nullptr;
    std::uint8_t* h_median_ = nullptr;    // pinned, frame_bytes_
    cudaStream_t stream_ = nullptr;
    cudaEvent_t copy_done_ = nullptr;

    // Burst state. The acquisition thread touches only the atomics.
    std::atomic<bool> claiming_{false};
    std::atomic<std::uint32_t> claimed_{0};
    std::atomic<std::uint32_t> outstanding_{0};  // queued, not yet released here
    std::atomic<int> min_free_entries_{-1};
    std::atomic<int> min_free_events_{-1};
    std::atomic<std::uint32_t> max_queue_depth_{0};
    std::atomic<std::uint64_t> first_offer_ns_{0};
    std::atomic<bool> offer_failed_{false};
    std::string offer_failure_;  // written under mutex_ by the acquisition thread only on failure

    mutable std::mutex mutex_;
    std::uint64_t next_request_id_ = 0;
    std::uint64_t active_request_id_ = 0;  // 0: idle
    bool active_failed_ = false;
    bool median_running_ = false;  // the kernel and its copy are on the stream
    std::uint32_t copied_ = 0;
    std::uint64_t last_stream_frame_id_ = 0;
    EmptyBackgroundBurstResult building_;
    bool has_result_ = false;
    EmptyBackgroundBurstResult result_;
};
