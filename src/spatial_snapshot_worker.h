#ifndef ORANGE_SPATIAL_SNAPSHOT_WORKER_H
#define ORANGE_SPATIAL_SNAPSHOT_WORKER_H

#include "threadworker.h"
#include "video_capture.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct SpatialSnapshotResult {
    bool ok = false;
    uint64_t request_id = 0;
    std::string operation_id;
    std::string camera_serial;
    std::string capture_mode = "full_resolution_stream_snapshot";
    std::string source_array_role = "images_full";
    std::string error;
    int width = 0;
    int height = 0;
    int pixel_format = 0;
    uint64_t local_frame_id = 0;
    uint64_t camera_frame_id = 0;
    uint64_t recording_frame_id = 0;
    uint64_t camera_timestamp_ns = 0;
    uint64_t timestamp_sys_ns = 0;
    std::vector<unsigned char> rgba;
};

class SpatialSnapshotWorker : public CThreadWorker<WORKER_ENTRY> {
public:
    SpatialSnapshotWorker(
        const char* name,
        CameraParams* camera_params,
        SafeQueue<WORKER_ENTRY*>& recycle_queue);
    ~SpatialSnapshotWorker() override = default;

    SpatialSnapshotWorker(const SpatialSnapshotWorker&) = delete;
    SpatialSnapshotWorker& operator=(const SpatialSnapshotWorker&) = delete;

    bool RequestSnapshot(
        const std::string& operation_id,
        uint64_t* request_id_out,
        std::string* error_out);
    bool HasPendingRequest() const;
    bool TryClaimNextFrame();
    void CompleteClaimedRequestWithError(const std::string& error);
    bool PopCompletedSnapshot(SpatialSnapshotResult* result_out);

    uint64_t request_count() const { return request_count_.load(std::memory_order_relaxed); }
    uint64_t completed_count() const { return completed_count_.load(std::memory_order_relaxed); }
    uint64_t failed_count() const { return failed_count_.load(std::memory_order_relaxed); }
    uint64_t enqueue_rejected_count() const { return enqueue_rejected_count_.load(std::memory_order_relaxed); }

protected:
    bool WorkerFunction(WORKER_ENTRY* entry) override;

private:
    struct ClaimedRequest {
        uint64_t request_id = 0;
        std::string operation_id;
    };

    ClaimedRequest current_claimed_request_locked() const;
    void complete_result(SpatialSnapshotResult result);
    bool copy_entry_to_rgba(
        const WORKER_ENTRY& entry,
        SpatialSnapshotResult* result,
        std::string* error_out);

    CameraParams* camera_params_ = nullptr;
    SafeQueue<WORKER_ENTRY*>* recycle_queue_ = nullptr;

    mutable std::mutex state_mutex_;
    bool pending_ = false;
    bool in_flight_ = false;
    uint64_t next_request_id_ = 0;
    ClaimedRequest pending_request_;
    ClaimedRequest in_flight_request_;
    bool has_completed_result_ = false;
    SpatialSnapshotResult completed_result_;

    std::atomic<uint64_t> request_count_{0};
    std::atomic<uint64_t> completed_count_{0};
    std::atomic<uint64_t> failed_count_{0};
    std::atomic<uint64_t> enqueue_rejected_count_{0};
};

#endif
