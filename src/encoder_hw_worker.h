// src/encoder_hw_worker.h

#ifndef ENCODER_HW_WORKER_H
#define ENCODER_HW_WORKER_H

#include "threadworker.h"
#include "video_capture.h"
#include "gpu_video_encoder.h"
#include <chrono>
#include <deque>
#include <fstream>
#include "encoder_pipeline.h"
#include "json.hpp"
#include "pre_encoder_reference_writer.h"

class EncoderPreprocessWorker; // Forward declaration

struct RecordingBitrateEstimate {
    uint32_t average_bitrate = 0;
    uint32_t max_bitrate = 0;
    double target_bpp = 0.0;
    bool average_clamped_to_min = false;
    bool average_clamped_to_max = false;
    bool max_clamped_to_max = false;
};

RecordingBitrateEstimate estimate_recording_bitrate(const CameraParams& camera_params,
                                                    const RecordingOutputConfig& recording_output_config);
int sanitize_recording_gop_length(int requested_gop_length);
uint32_t resolve_recording_gop_length(const CameraParams& camera_params,
                                      const std::string& tuning,
                                      int requested_gop_length);

class EncoderHwWorker : public CThreadWorker<ENCODER_WORKER_ENTRY>
{
public:
    EncoderHwWorker(
        const char* name,
        CameraParams* camera_params,
        const RecordingOutputConfig& recording_output_config,
        const std::string& codec,
        const std::string& preset,
        const std::string& tuning,
        const std::string& rate_control_mode,
        int quality_value,
        int gop_length,
        std::string folder_name,
        EncoderPreprocessWorker* prep_worker,
        CameraControl* camera_control,
        const PreEncoderReferenceCaptureConfig& pre_encoder_reference_capture_config
    );
    ~EncoderHwWorker() override;

    void SetPreprocessWorker(EncoderPreprocessWorker* prep_worker);
    void flush_and_close();

    double get_fps() const { return current_fps_.load(std::memory_order_relaxed); }
    uint64_t get_total_packets() const { return total_packets_.load(); }
    uint64_t get_encode_failures() const { return encode_failures_.load(); }
    uint64_t get_slow_frames() const { return slow_frames_.load(); }
    int get_queue_depth() const { return const_cast<EncoderHwWorker*>(this)->GetCountQueueInSize(); }
    bool direct_input_enabled() const { return direct_input_enabled_; }
    int encoder_input_pitch() const { return encoder_input_pitch_; }
    int encoder_buffer_count() const { return encoder_buffer_count_; }

    EncoderContext encoder_;
    EncoderPreprocessWorker* m_prep_worker_;

protected:
    bool WorkerFunction(ENCODER_WORKER_ENTRY* f) override;

private:
    void finalize_recording();
    bool drain_ready();
    nlohmann::json build_encoder_snapshot_json() const;
    void initialize_pre_encoder_reference_capture();
    void finalize_pre_encoder_reference_capture();
    bool begin_pre_encoder_reference_capture(ENCODER_WORKER_ENTRY* entry,
                                             size_t* staging_slot_out,
                                             size_t* frame_size_out);
    void poll_pre_encoder_reference_captures(bool wait_for_all);
    void recycle_encoder_entry(ENCODER_WORKER_ENTRY* entry,
                               const std::vector<uint32_t>& retired_slots);
    void release_pre_encoder_reference_capture_resources();
    bool ensure_pre_encoder_reference_staging_slots(size_t frame_size, std::string* error_out);

    struct ReferenceCaptureStagingSlot {
        unsigned char* host_buffer = nullptr;
        cudaEvent_t copy_complete_event = nullptr;
        size_t buffer_size = 0;
        bool in_use = false;
    };

    struct PendingReferenceCapture {
        ENCODER_WORKER_ENTRY* entry = nullptr;
        size_t staging_slot = 0;
        size_t frame_size = 0;
        std::vector<uint32_t> retired_slots;
    };

    struct EncoderSnapshotInfo {
        std::string backend;
        std::string path;
        std::string codec;
        std::string preset;
        std::string tuning;
        std::string rc_strategy;
        std::string output_mode;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t source_width = 0;
        uint32_t source_height = 0;
        uint32_t fps = 0;
        uint32_t downsample_factor = 1;
        uint32_t requested_output_width = 0;
        uint32_t requested_output_height = 0;
        uint32_t gop_length = 0;
        uint32_t frame_interval_p = 0;
        uint32_t idr_period = 0;
        uint32_t max_num_ref_frames = 0;
        uint32_t max_num_ref_frames_in_dpb = 0;
        uint32_t rc_mode = 0;
        uint32_t average_bitrate = 0;
        uint32_t max_bitrate = 0;
        uint32_t vbv_buffer_size = 0;
        uint32_t target_quality = 0;
        uint32_t target_quality_lsb = 0;
        uint32_t const_qp_inter_p = 0;
        uint32_t const_qp_inter_b = 0;
        uint32_t const_qp_intra = 0;
        uint32_t enable_aq = 0;
        uint32_t enable_temporal_aq = 0;
        uint32_t enable_lookahead = 0;
        uint32_t low_delay_keyframe_scale = 0;
        uint32_t strict_gop_target = 0;
        uint32_t enable_non_ref_p = 0;
        uint32_t repeat_sps_pps = 0;
        uint32_t enable_ptd = 0;
        int gpu_id = -1;
        nlohmann::json gpu = nlohmann::json::object();
        bool color = false;
    };

    CameraParams* camera_params_;
    RecordingOutputConfig recording_output_config_;
    std::string base_folder_name_;
    std::string codec_;
    std::string preset_;
    std::string tuning_;
    std::string rate_control_mode_;
    PreEncoderReferenceCaptureConfig pre_encoder_reference_capture_config_;
    bool direct_input_enabled_ = false;
    bool direct_input_registered_ = false;
    int encoder_input_pitch_ = 0;
    int encoder_buffer_count_ = 0;
    bool pre_encoder_reference_async_enabled_ = false;
    int quality_value_ = 20;
    int gop_length_ = 0;
    Writer writer_;
    PreEncoderReferenceWriter pre_encoder_reference_writer_;
    std::vector<ReferenceCaptureStagingSlot> pre_encoder_reference_staging_slots_;
    std::deque<PendingReferenceCapture> pending_pre_encoder_reference_captures_;
    std::string active_recording_folder_;
    cudaStream_t m_stream = nullptr;
    cudaStream_t pre_encoder_reference_stream_ = nullptr;
    CameraControl* camera_control_;

    bool is_recording_ = false; // Tracks the local recording state of this worker
    bool encoder_snapshot_valid_ = false;
    EncoderSnapshotInfo encoder_snapshot_;

    uint64_t last_recording_frame_id_ = 0;
    std::chrono::steady_clock::time_point last_fps_update_time_;
    int frame_counter_;
    std::atomic<double> current_fps_{0.0};
    std::atomic<uint64_t> slow_frames_{0};
    std::atomic<uint64_t> total_packets_{0};
    std::atomic<uint64_t> encode_failures_{0};
};

#endif // ENCODER_HW_WORKER_H
