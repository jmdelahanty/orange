// src/acquire_frames.cpp

#include "acquire_frames.h"
#include "nvtx_profiling.h"
#include "NvEncoder/NvCodecUtils.h"
#include "image_processing.h"
#include "kernel.cuh"
#include <chrono>
#include <deque>
#include <cuda_runtime.h>
#include "global.h"
#include "thread.h"
#include "opengldisplay.h"
#include "gpu_video_encoder.h"
#include "yolo_worker.h"
#include "image_writer_worker.h"
#include "crop_and_encode_worker.h"
#include "recording_ingress.h"
#include "cuda_context_debug.h"
#include "frame_ipc_manager.h"
#include "fsuid_guard.h"
#include "project.h"
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>

#ifndef PIPELINE_PROFILE
#if defined(YOLO_PROFILE) && YOLO_PROFILE
#define PIPELINE_PROFILE 1
#else
#define PIPELINE_PROFILE 0
#endif
#endif
#if PIPELINE_PROFILE
static constexpr int kCopyProfileLogEvery = 60;
#endif
static constexpr int64_t kPtpStaleReceiveThresholdNs = 50LL * 1000LL * 1000LL;
static constexpr size_t kPtpReceiveHistoryLimit = 256;
static constexpr size_t kRecordingSubmitHistoryLimit = 256;

namespace {
struct RunningInt64Stats {
    int64_t min = std::numeric_limits<int64_t>::max();
    int64_t max = std::numeric_limits<int64_t>::min();
    long double sum = 0.0;
    uint64_t samples = 0;
    int64_t last = 0;

    void add(int64_t value) {
        if (value < min) min = value;
        if (value > max) max = value;
        sum += static_cast<long double>(value);
        last = value;
        ++samples;
    }

    void reset() {
        min = std::numeric_limits<int64_t>::max();
        max = std::numeric_limits<int64_t>::min();
        sum = 0.0;
        samples = 0;
        last = 0;
    }

    nlohmann::json to_json() const {
        nlohmann::json out = nlohmann::json::object();
        out["samples"] = samples;
        if (samples == 0) {
            return out;
        }
        out["min"] = min;
        out["max"] = max;
        out["last"] = last;
        out["mean"] = static_cast<double>(sum / static_cast<long double>(samples));
        return out;
    }
};

struct RunningDoubleStats {
    double min = std::numeric_limits<double>::infinity();
    double max = -std::numeric_limits<double>::infinity();
    long double sum = 0.0;
    uint64_t samples = 0;
    double last = 0.0;

    void add(double value) {
        if (value < min) min = value;
        if (value > max) max = value;
        sum += static_cast<long double>(value);
        last = value;
        ++samples;
    }

    void reset() {
        min = std::numeric_limits<double>::infinity();
        max = -std::numeric_limits<double>::infinity();
        sum = 0.0;
        samples = 0;
        last = 0.0;
    }

    nlohmann::json to_json() const {
        nlohmann::json out = nlohmann::json::object();
        out["samples"] = samples;
        if (samples == 0) {
            return out;
        }
        out["min"] = min;
        out["max"] = max;
        out["last"] = last;
        out["mean"] = static_cast<double>(sum / static_cast<long double>(samples));
        return out;
    }
};

struct PipelinePerfSample {
    std::string timestamp_utc;
    uint64_t frame_id = 0;
    uint64_t recording_frame_id = 0;
    double acquisition_fps = 0.0;
    double preprocess_fps = 0.0;
    double preprocess_fps_primary = 0.0;
    double preprocess_fps_helpers = 0.0;
    double encode_fps = 0.0;
    double encode_fps_primary = 0.0;
    double encode_fps_helpers = 0.0;
    int display_queue_depth = -1;
    int yolo_queue_depth = -1;
    int preprocess_queue_depth = -1;
    int encode_queue_depth = -1;
    int free_entries_available = -1;
    int free_entries_low_watermark = -1;
    int free_events_available = -1;
    int free_events_low_watermark = -1;
    int yolo_events_available = -1;
    int yolo_events_low_watermark = -1;
    int pending_requeues = -1;
    uint64_t acquisition_resource_starvations = 0;
    int preprocess_buffers_available = -1;
    int preprocess_events_available = -1;
    uint64_t preprocess_resource_waits = 0;
    uint64_t preprocess_frames_dropped = 0;
    uint64_t encode_failures = 0;
    uint64_t encode_slow_frames = 0;
    uint64_t submitted_frames = 0;
    uint64_t primary_routed_frames = 0;
    uint64_t helper_requested_frames = 0;
    uint64_t helper_fallback_frames = 0;
    uint64_t helper_dispatched_frames = 0;
    int last_target_gpu_id = -1;
    std::string last_route_mode = "primary";
    uint64_t camera_dropped_frames = 0;
    uint64_t gpu_direct_frames = 0;
    uint64_t gpu_ring_copy_frames = 0;
    uint64_t gpu_copy_frames = 0;
};

struct PtpReceiveHistoryEntry {
    uint64_t local_frame_id = 0;
    uint64_t camera_frame_id = 0;
    uint64_t frame_timestamp_ns = 0;
    uint64_t timestamp_sys_ns = 0;
    int64_t latch_minus_frame_ns = 0;
    uint64_t frame_delta_ns = 0;
    uint64_t latch_delta_ns = 0;
    uint64_t camera_dropped_frames = 0;
    int dispatch_count = 0;
    int free_entries_available = -1;
    int free_events_available = -1;
    int yolo_events_available = -1;
    size_t pending_requeues = 0;
    uint64_t acquisition_resource_starvations = 0;
    bool will_display = false;
    bool will_record = false;
    bool will_yolo = false;
    bool use_direct_pointer = false;
    bool use_ring_copy = false;
};

struct RecordingSubmitHistoryEntry {
    uint64_t local_frame_id = 0;
    uint64_t recording_frame_id = 0;
    uint64_t camera_frame_id = 0;
    int dispatch_count = 0;
    bool use_direct_pointer = false;
    bool use_ring_copy = false;
    RecordingIngressStats ingress_stats;
};

void append_ptp_receive_history(std::deque<PtpReceiveHistoryEntry>* history,
                                const PtpReceiveHistoryEntry& entry) {
    if (!history) {
        return;
    }
    history->push_back(entry);
    while (history->size() > kPtpReceiveHistoryLimit) {
        history->pop_front();
    }
}

void dump_ptp_receive_history(const CameraParams* camera_params,
                              const CameraControl* camera_control,
                              const std::deque<PtpReceiveHistoryEntry>& history,
                              const PtpReceiveHistoryEntry& trigger_entry) {
    const std::string serial =
        camera_params ? camera_params->camera_serial : std::string("<unknown>");
    std::cout << "[PTP_STALE_DUMP] cam=" << serial
              << " threshold_ns=" << kPtpStaleReceiveThresholdNs
              << " trigger_local_frame=" << trigger_entry.local_frame_id
              << " trigger_camera_frame_id=" << trigger_entry.camera_frame_id
              << " trigger_latch_minus_frame_ns=" << trigger_entry.latch_minus_frame_ns
              << " gate_offset_ns="
              << (camera_params ? camera_params->ptp_gate_offset_ns : 0ULL)
              << " ptp_gate_acquisition_mode="
              << (camera_params ? camera_params->ptp_gate_acquisition_mode : std::string("<unknown>"))
              << " sync_camera="
              << (camera_control && camera_control->sync_camera ? 1 : 0)
              << " history_entries=" << history.size()
              << std::endl;

    size_t index = 0;
    for (const PtpReceiveHistoryEntry& entry : history) {
        std::cout << "  [PTP_STALE_DUMP][" << index << "]"
                  << " local_frame=" << entry.local_frame_id
                  << " camera_frame_id=" << entry.camera_frame_id
                  << " frame_ts_ns=" << entry.frame_timestamp_ns
                  << " timestamp_sys_ns=" << entry.timestamp_sys_ns
                  << " latch_minus_frame_ns=" << entry.latch_minus_frame_ns
                  << " frame_delta_ns=" << entry.frame_delta_ns
                  << " latch_delta_ns=" << entry.latch_delta_ns
                  << " camera_dropped_frames=" << entry.camera_dropped_frames
                  << " dispatch_count=" << entry.dispatch_count
                  << " free_entries=" << entry.free_entries_available
                  << " free_events=" << entry.free_events_available
                  << " yolo_events=" << entry.yolo_events_available
                  << " pending_requeues=" << entry.pending_requeues
                  << " acq_starve=" << entry.acquisition_resource_starvations
                  << " will_display=" << (entry.will_display ? 1 : 0)
                  << " will_record=" << (entry.will_record ? 1 : 0)
                  << " will_yolo=" << (entry.will_yolo ? 1 : 0)
                  << " direct=" << (entry.use_direct_pointer ? 1 : 0)
                  << " ring_copy=" << (entry.use_ring_copy ? 1 : 0)
                  << std::endl;
        ++index;
    }
}

void append_recording_submit_history(std::deque<RecordingSubmitHistoryEntry>* history,
                                     const RecordingSubmitHistoryEntry& entry)
{
    if (!history) {
        return;
    }
    history->push_back(entry);
    while (history->size() > kRecordingSubmitHistoryLimit) {
        history->pop_front();
    }
}

void dump_recording_submit_history(const CameraParams* camera_params,
                                   const std::deque<RecordingSubmitHistoryEntry>& history)
{
    const std::string serial =
        camera_params ? camera_params->camera_serial : std::string("<unknown>");
    std::cout << "[PTP_STALE_DUMP][RECORDING] cam=" << serial
              << " history_entries=" << history.size()
              << std::endl;

    size_t index = 0;
    for (const RecordingSubmitHistoryEntry& entry : history) {
        std::cout << "  [PTP_STALE_DUMP][RECORDING][" << index << "]"
                  << " local_frame=" << entry.local_frame_id
                  << " recording_frame=" << entry.recording_frame_id
                  << " camera_frame_id=" << entry.camera_frame_id
                  << " dispatch_count=" << entry.dispatch_count
                  << " direct=" << (entry.use_direct_pointer ? 1 : 0)
                  << " ring_copy=" << (entry.use_ring_copy ? 1 : 0)
                  << " submitted=" << entry.ingress_stats.submitted_frames
                  << " primary_routed=" << entry.ingress_stats.primary_routed_frames
                  << " helper_requested=" << entry.ingress_stats.helper_requested_frames
                  << " helper_fallback=" << entry.ingress_stats.helper_fallback_frames
                  << " helper_dispatched=" << entry.ingress_stats.helper_dispatched_frames
                  << " pre_q=" << entry.ingress_stats.preprocess_queue_depth
                  << " enc_q=" << entry.ingress_stats.encode_queue_depth
                  << " pre_buf=" << entry.ingress_stats.preprocess_buffers_available
                  << " pre_evt=" << entry.ingress_stats.preprocess_events_available
                  << " pre_waits=" << entry.ingress_stats.preprocess_resource_waits
                  << " pre_drops=" << entry.ingress_stats.preprocess_frames_dropped
                  << " enc_fail=" << entry.ingress_stats.encode_failures
                  << " enc_slow=" << entry.ingress_stats.encode_slow_frames
                  << " last_target_gpu_id=" << entry.ingress_stats.last_target_gpu_id
                  << " last_route_mode=" << entry.ingress_stats.last_route_mode
                  << std::endl;
        ++index;
    }
}

class PipelinePerfRecorder {
public:
    explicit PipelinePerfRecorder(const CameraParams* camera_params)
        : camera_params_(camera_params) {}

    ~PipelinePerfRecorder() {
        Close();
    }

    const std::string& current_folder() const {
        return current_folder_;
    }

    void Rotate(const std::string& folder) {
        if (folder == current_folder_) {
            return;
        }
        CloseCurrent();
        if (!folder.empty()) {
            OpenFile(folder);
        }
    }

    void Record(const PipelinePerfSample& sample) {
        if (!file_.is_open()) {
            return;
        }

        file_ << sample.timestamp_utc << ","
              << sample.frame_id << ","
              << sample.recording_frame_id << ","
              << sample.acquisition_fps << ","
              << sample.preprocess_fps << ","
              << sample.preprocess_fps_primary << ","
              << sample.preprocess_fps_helpers << ","
              << sample.encode_fps << ","
              << sample.encode_fps_primary << ","
              << sample.encode_fps_helpers << ","
              << sample.display_queue_depth << ","
              << sample.yolo_queue_depth << ","
              << sample.preprocess_queue_depth << ","
              << sample.encode_queue_depth << ","
              << sample.free_entries_available << ","
              << sample.free_entries_low_watermark << ","
              << sample.free_events_available << ","
              << sample.free_events_low_watermark << ","
              << sample.yolo_events_available << ","
              << sample.yolo_events_low_watermark << ","
              << sample.pending_requeues << ","
              << sample.acquisition_resource_starvations << ","
              << sample.preprocess_buffers_available << ","
              << sample.preprocess_events_available << ","
              << sample.preprocess_resource_waits << ","
              << sample.preprocess_frames_dropped << ","
              << sample.encode_failures << ","
              << sample.encode_slow_frames << ","
              << sample.submitted_frames << ","
              << sample.primary_routed_frames << ","
              << sample.helper_requested_frames << ","
              << sample.helper_fallback_frames << ","
              << sample.helper_dispatched_frames << ","
              << sample.last_target_gpu_id << ","
              << sample.last_route_mode << ","
              << sample.camera_dropped_frames << ","
              << sample.gpu_direct_frames << ","
              << sample.gpu_ring_copy_frames << ","
              << sample.gpu_copy_frames << "\n";
        file_.flush();

        acquisition_fps_.add(sample.acquisition_fps);
        preprocess_fps_.add(sample.preprocess_fps);
        preprocess_fps_primary_.add(sample.preprocess_fps_primary);
        preprocess_fps_helpers_.add(sample.preprocess_fps_helpers);
        encode_fps_.add(sample.encode_fps);
        encode_fps_primary_.add(sample.encode_fps_primary);
        encode_fps_helpers_.add(sample.encode_fps_helpers);
        display_queue_depth_.add(sample.display_queue_depth);
        yolo_queue_depth_.add(sample.yolo_queue_depth);
        preprocess_queue_depth_.add(sample.preprocess_queue_depth);
        encode_queue_depth_.add(sample.encode_queue_depth);
        free_entries_available_.add(sample.free_entries_available);
        free_entries_low_watermark_.add(sample.free_entries_low_watermark);
        free_events_available_.add(sample.free_events_available);
        free_events_low_watermark_.add(sample.free_events_low_watermark);
        yolo_events_available_.add(sample.yolo_events_available);
        yolo_events_low_watermark_.add(sample.yolo_events_low_watermark);
        pending_requeues_.add(sample.pending_requeues);
        preprocess_buffers_available_.add(sample.preprocess_buffers_available);
        preprocess_events_available_.add(sample.preprocess_events_available);
        last_sample_ = sample;
        have_last_sample_ = true;
        ++samples_;
    }

    void Close() {
        CloseCurrent();
    }

private:
    void OpenFile(const std::string& folder) {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        make_folder(folder);

        current_folder_ = folder;
        const std::string serial = camera_params_ ? camera_params_->camera_serial : "unknown";
        file_path_ = (std::filesystem::path(current_folder_) /
                      ("Cam" + serial + "_pipeline_perf.csv")).string();
        file_.open(file_path_, std::ios::out | std::ios::trunc);
        if (!file_) {
            std::cerr << "[PIPELINE] Cam " << serial
                      << " failed to open " << file_path_ << std::endl;
            current_folder_.clear();
            file_path_.clear();
            return;
        }
        ResetStats();
        file_ << "timestamp_utc,frame_id,recording_frame_id,acq_fps,pre_fps,pre_fps_primary,pre_fps_helpers,enc_fps,enc_fps_primary,enc_fps_helpers,"
                 "display_q,yolo_q,pre_q,enc_q,"
                 "acq_free_entries,acq_free_entries_low,acq_free_events,acq_free_events_low,"
                 "yolo_events,yolo_events_low,pending_requeues,"
                 "acq_starve,pre_buffers,pre_events,pre_waits,pre_drops,enc_fail,enc_slow,"
                 "submitted_frames,primary_routed_frames,helper_requested_frames,helper_fallback_frames,helper_dispatched_frames,last_target_gpu_id,last_route_mode,"
                 "camera_dropped_frames,"
                 "gpu_direct,gpu_ring,gpu_copy\n";
        file_ << std::fixed << std::setprecision(6);
        std::cout << "[PIPELINE] Cam " << serial
                  << " logging to " << file_path_ << std::endl;
    }

    void CloseCurrent() {
        if (!current_folder_.empty()) {
            PersistSummary();
        }
        if (file_.is_open()) {
            file_.close();
        }
        current_folder_.clear();
        file_path_.clear();
        ResetStats();
    }

    void ResetStats() {
        samples_ = 0;
        have_last_sample_ = false;
        last_sample_ = PipelinePerfSample{};
        acquisition_fps_.reset();
        preprocess_fps_.reset();
        preprocess_fps_primary_.reset();
        preprocess_fps_helpers_.reset();
        encode_fps_.reset();
        encode_fps_primary_.reset();
        encode_fps_helpers_.reset();
        display_queue_depth_.reset();
        yolo_queue_depth_.reset();
        preprocess_queue_depth_.reset();
        encode_queue_depth_.reset();
        free_entries_available_.reset();
        free_entries_low_watermark_.reset();
        free_events_available_.reset();
        free_events_low_watermark_.reset();
        yolo_events_available_.reset();
        yolo_events_low_watermark_.reset();
        pending_requeues_.reset();
        preprocess_buffers_available_.reset();
        preprocess_events_available_.reset();
    }

    void PersistSummary() {
        if (current_folder_.empty()) {
            return;
        }

        nlohmann::json summary = nlohmann::json::object();
        summary["schema_version"] = 1;
        summary["camera_serial"] = camera_params_ ? camera_params_->camera_serial : "";
        summary["camera_id"] = camera_params_ ? camera_params_->camera_id : -1;
        summary["gpu_id"] = camera_params_ ? camera_params_->gpu_id : -1;
        summary["gpu"] = build_gpu_runtime_info(camera_params_ ? camera_params_->gpu_id : -1);
        summary["updated_at_utc"] = get_current_utc_timestamp();
        summary["artifact_path"] = file_path_;
        summary["period_seconds"] = 1;
        summary["samples"] = samples_;
        summary["finalized"] = true;

        if (have_last_sample_) {
            summary["last_sample_at_utc"] = last_sample_.timestamp_utc;
            summary["last_frame_id"] = last_sample_.frame_id;
            summary["last_recording_frame_id"] = last_sample_.recording_frame_id;
        }

        summary["fps"] = {
            {"acquisition", acquisition_fps_.to_json()},
            {"preprocess", preprocess_fps_.to_json()},
            {"preprocess_primary", preprocess_fps_primary_.to_json()},
            {"preprocess_helpers", preprocess_fps_helpers_.to_json()},
            {"encode", encode_fps_.to_json()},
            {"encode_primary", encode_fps_primary_.to_json()},
            {"encode_helpers", encode_fps_helpers_.to_json()},
        };
        summary["queue_depth"] = {
            {"display", display_queue_depth_.to_json()},
            {"yolo", yolo_queue_depth_.to_json()},
            {"preprocess", preprocess_queue_depth_.to_json()},
            {"encode", encode_queue_depth_.to_json()},
            {"pending_requeues", pending_requeues_.to_json()},
        };
        summary["resource_availability"] = {
            {"acquire_entries", free_entries_available_.to_json()},
            {"acquire_entries_low_watermark", free_entries_low_watermark_.to_json()},
            {"acquire_events", free_events_available_.to_json()},
            {"acquire_events_low_watermark", free_events_low_watermark_.to_json()},
            {"yolo_events", yolo_events_available_.to_json()},
            {"yolo_events_low_watermark", yolo_events_low_watermark_.to_json()},
            {"preprocess_buffers", preprocess_buffers_available_.to_json()},
            {"preprocess_events", preprocess_events_available_.to_json()},
        };

        nlohmann::json totals = nlohmann::json::object();
        if (have_last_sample_) {
            totals["acquisition_resource_starvations"] = last_sample_.acquisition_resource_starvations;
            totals["preprocess_resource_waits"] = last_sample_.preprocess_resource_waits;
            totals["preprocess_frames_dropped"] = last_sample_.preprocess_frames_dropped;
            totals["encode_failures"] = last_sample_.encode_failures;
            totals["encode_slow_frames"] = last_sample_.encode_slow_frames;
            totals["submitted_frames"] = last_sample_.submitted_frames;
            totals["primary_routed_frames"] = last_sample_.primary_routed_frames;
            totals["helper_requested_frames"] = last_sample_.helper_requested_frames;
            totals["helper_fallback_frames"] = last_sample_.helper_fallback_frames;
            totals["helper_dispatched_frames"] = last_sample_.helper_dispatched_frames;
            totals["last_target_gpu_id"] = last_sample_.last_target_gpu_id;
            totals["last_route_mode"] = last_sample_.last_route_mode;
            totals["camera_dropped_frames"] = last_sample_.camera_dropped_frames;
            totals["gpu_direct_frames"] = last_sample_.gpu_direct_frames;
            totals["gpu_ring_copy_frames"] = last_sample_.gpu_ring_copy_frames;
            totals["gpu_copy_frames"] = last_sample_.gpu_copy_frames;
        }
        summary["totals"] = totals;
        if (have_last_sample_) {
            summary["routing"] = {
                {"submitted_frames", last_sample_.submitted_frames},
                {"primary_routed_frames", last_sample_.primary_routed_frames},
                {"helper_requested_frames", last_sample_.helper_requested_frames},
                {"helper_fallback_frames", last_sample_.helper_fallback_frames},
                {"helper_dispatched_frames", last_sample_.helper_dispatched_frames},
                {"last_target_gpu_id", last_sample_.last_target_gpu_id},
                {"last_route_mode", last_sample_.last_route_mode}
            };
        }

        if (!update_recording_snapshot_pipeline_metrics(
                current_folder_,
                camera_params_ ? camera_params_->camera_serial : "",
                summary)) {
            std::cerr << "[PIPELINE] Cam "
                      << (camera_params_ ? camera_params_->camera_serial : "unknown")
                      << " failed to update recording snapshot with pipeline metrics"
                      << std::endl;
        }
    }

    const CameraParams* camera_params_ = nullptr;
    std::string current_folder_;
    std::string file_path_;
    std::ofstream file_;
    uint64_t samples_ = 0;
    bool have_last_sample_ = false;
    PipelinePerfSample last_sample_;
    RunningDoubleStats acquisition_fps_{};
    RunningDoubleStats preprocess_fps_{};
    RunningDoubleStats preprocess_fps_primary_{};
    RunningDoubleStats preprocess_fps_helpers_{};
    RunningDoubleStats encode_fps_{};
    RunningDoubleStats encode_fps_primary_{};
    RunningDoubleStats encode_fps_helpers_{};
    RunningInt64Stats display_queue_depth_{};
    RunningInt64Stats yolo_queue_depth_{};
    RunningInt64Stats preprocess_queue_depth_{};
    RunningInt64Stats encode_queue_depth_{};
    RunningInt64Stats free_entries_available_{};
    RunningInt64Stats free_entries_low_watermark_{};
    RunningInt64Stats free_events_available_{};
    RunningInt64Stats free_events_low_watermark_{};
    RunningInt64Stats yolo_events_available_{};
    RunningInt64Stats yolo_events_low_watermark_{};
    RunningInt64Stats pending_requeues_{};
    RunningInt64Stats preprocess_buffers_available_{};
    RunningInt64Stats preprocess_events_available_{};
};

std::string current_recording_folder(CameraControl* camera_control) {
    std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
    return camera_control->recording_folder;
}
}

static inline void PTP_timestamp_checking(PTPState *ptp_state,
                                          CameraEmergent *ecam,
                                          CameraState *camera_state,
                                          CameraParams *camera_params){
    NVTX_RANGE("PTP_Timestamp_Check");
    EVT_CameraExecuteCommand(&ecam->camera, "GevTimestampControlLatch");
    EVT_CameraGetUInt32Param(&ecam->camera, "GevTimestampValueHigh", &ptp_state->ptp_time_high);
    EVT_CameraGetUInt32Param(&ecam->camera, "GevTimestampValueLow", &ptp_state->ptp_time_low);
    ptp_state->ptp_time = (((unsigned long long)(ptp_state->ptp_time_high)) << 32) | ((unsigned long long)(ptp_state->ptp_time_low));
    ptp_state->frame_ts = ecam->frame_recv.timestamp;
    if (camera_state->frame_count != 0) {
        ptp_state->ptp_time_delta = ptp_state->ptp_time - ptp_state->ptp_time_prev;
        ptp_state->ptp_time_delta_sum += ptp_state->ptp_time_delta;
        ptp_state->frame_ts_delta = ptp_state->frame_ts - ptp_state->frame_ts_prev;
        ptp_state->frame_ts_delta_sum += ptp_state->frame_ts_delta;
    }
    ptp_state->ptp_time_prev = ptp_state->ptp_time;
    ptp_state->frame_ts_prev = ptp_state->frame_ts;

    const uint64_t next_frame_index = static_cast<uint64_t>(camera_state->frame_count) + 1;
    if (next_frame_index <= 5) {
        const int64_t latch_minus_frame_ns =
            static_cast<int64_t>(ptp_state->ptp_time) - static_cast<int64_t>(ptp_state->frame_ts);
        std::cout << "[PTP_FIRST_FRAMES]"
                  << " cam=" << (camera_params ? camera_params->camera_serial : std::string("<unknown>"))
                  << " frame=" << next_frame_index
                  << " frame_ts_ns=" << ptp_state->frame_ts
                  << " latched_ptp_ns=" << ptp_state->ptp_time
                  << " latch_minus_frame_ns=" << latch_minus_frame_ns
                  << " frame_delta_ns=" << ptp_state->frame_ts_delta
                  << " latch_delta_ns=" << ptp_state->ptp_time_delta
                  << std::endl;
    }
}

void acquire_frames(
    CameraEmergent *ecam,
    CameraParams *camera_params,
    CameraEachSelect* camera_select,
    CameraControl* camera_control,
    PTPParams* ptp_params,
    INDIGOSignalBuilder* indigo_signal_builder,
    COpenGLDisplay* openGLDisplay,
    RecordingIngress* recording_ingress,
    YOLOv8Worker* yolo_worker,
    ImageWriterWorker* image_writer,
    CameraResources* resources,
    FrameIPCManager* frame_ipc_manager
){
    ck(cudaSetDevice(camera_params->gpu_id));
    NVTX_CAMERA("AcquireFrames_Main");
    std::cout << "Starting acquisition loop for camera " << camera_params->camera_serial << std::endl;

    {
        NVTX_RANGE("CUDA_Context_Setup");
        CUDA_CTX_LOG("=== ACQUIRE FRAMES START ===");
        dumpCudaState("Acquire frames startup");
        ck(cudaSetDevice(camera_params->gpu_id));
        CUDA_RT_LOG("Set device to " + std::to_string(camera_params->gpu_id));
    }

    cudaStream_t stream;
    FrameProcess frame_process_save;

    {
        NVTX_RANGE("Stream_and_Buffer_Init");
        ck(cudaStreamCreate(&stream));
        CUDA_STREAM_LOG("Created acquisition stream", stream);

        initalize_gpu_frame(&frame_process_save.frame_original, camera_params);
        initialize_gpu_debayer(&frame_process_save.debayer, camera_params);
        initialize_cpu_frame(&frame_process_save.frame_cpu, camera_params);
        ck(cudaMalloc((void **)&frame_process_save.d_convert, (size_t)camera_params->width * camera_params->height * 3));
    }

    FrameIPCManager* ipc_manager = frame_ipc_manager;
    if (ipc_manager && !ipc_manager->isEnabled()) {
        ipc_manager = nullptr;
    }

    CameraState camera_state{};
    // std::cout << "[DEBUG] Camera " << camera_params->camera_serial 
    //       << " - camera_state address: " << &camera_state 
    //       << ", frame_count address: " << &camera_state.frame_count << std::endl;
    PTPState ptp_state{};
    RunningInt64Stats ptp_offset_stats{};
    RunningInt64Stats latch_minus_frame_stats{};
    RunningInt64Stats frame_delta_stats{};
    RunningInt64Stats latch_delta_stats{};
    std::string ptp_summary_recording_folder;
    StopWatch w;
    auto last_fps_update_time = std::chrono::steady_clock::now();
    int frame_counter_for_fps = 0;
    double current_acquisition_fps = 0.0;
    uint64_t local_recording_frame_count = 0;
    uint64_t last_recording_frame_count = 0;
    uint64_t acquisition_resource_starvations = 0;
    uint64_t gpu_direct_frames = 0;
    uint64_t gpu_ring_copy_frames = 0;
    uint64_t gpu_copy_frames = 0;
    uint64_t gpu_direct_frames_total = 0;
    uint64_t gpu_ring_copy_frames_total = 0;
    uint64_t gpu_copy_frames_total = 0;
    uint64_t gpu_direct_attr_errors = 0;
    uint64_t gpu_direct_non_device = 0;
    uint64_t gpu_direct_wrong_device = 0;
    auto last_gpu_direct_log_time = std::chrono::steady_clock::now();
    int free_entries_available = resources ? resources->acquire_work_entries_max : 0;
    int free_events_available = resources ? static_cast<int>(resources->event_pool.size()) : 0;
    int yolo_events_available =
        (resources && resources->yolo_events_queue) ? static_cast<int>(resources->yolo_event_pool.size()) : 0;
    int free_entries_low = free_entries_available;
    int free_events_low = free_events_available;
    int yolo_events_low = yolo_events_available;
    struct PendingRequeue {
        Emergent::CEmergentCamera* camera;
        Emergent::CEmergentFrame* frame;
        cudaEvent_t* event;
    };
    std::deque<PendingRequeue> pending_requeues;
    int yolo_decimate = 1;
    const char* yolo_decimate_env = std::getenv("ORANGE_YOLO_DECIMATE");
    if (yolo_decimate_env && *yolo_decimate_env) {
        char* end = nullptr;
        long parsed = std::strtol(yolo_decimate_env, &end, 10);
        if (end != yolo_decimate_env && parsed > 1 && parsed < 10000) {
            yolo_decimate = static_cast<int>(parsed);
        }
    }
    uint64_t yolo_decimate_counter = 0;
    bool last_yolo_enabled = false;
    PipelinePerfRecorder pipeline_perf_recorder(camera_params);
    std::deque<PtpReceiveHistoryEntry> ptp_receive_history;
    std::deque<RecordingSubmitHistoryEntry> recording_submit_history;
    bool ptp_stale_history_dumped = false;
    if (yolo_decimate > 1) {
        std::cout << "[YOLO] Cam " << camera_params->camera_serial
                  << " decimate=1/" << yolo_decimate << std::endl;
    }

    auto reset_ptp_summary_stats = [&]() {
        ptp_offset_stats.reset();
        latch_minus_frame_stats.reset();
        frame_delta_stats.reset();
        latch_delta_stats.reset();
    };

    auto build_ptp_camera_summary_json = [&](bool finalized) {
        nlohmann::json summary = nlohmann::json::object();
        summary["camera_serial"] = camera_params->camera_serial;
        summary["camera_id"] = camera_params->camera_id;
        summary["gpu_id"] = camera_params->gpu_id;
        summary["sync_camera_enabled"] = camera_control->sync_camera;
        summary["finalized"] = finalized;
        summary["updated_at_utc"] = get_current_utc_timestamp();
        summary["frame_count"] = camera_state.frame_count;
        summary["frames_received"] = camera_state.frames_recd;
        summary["dropped_frames"] = camera_state.dropped_frames;
        summary["last_frame_timestamp_ns"] = ptp_state.frame_ts;
        summary["last_latched_ptp_time_ns"] = ptp_state.ptp_time;
        summary["ptp_offset_ns"] = ptp_offset_stats.to_json();
        summary["latch_minus_frame_ns"] = latch_minus_frame_stats.to_json();
        summary["frame_delta_ns"] = frame_delta_stats.to_json();
        summary["latch_delta_ns"] = latch_delta_stats.to_json();
        const uint64_t delta_samples = (camera_state.frame_count > 1) ? (camera_state.frame_count - 1) : 0;
        summary["delta_samples"] = delta_samples;
        if (delta_samples > 0) {
            summary["avg_frame_delta_ns_running"] = ptp_state.frame_ts_delta_sum / delta_samples;
            summary["avg_latch_delta_ns_running"] = ptp_state.ptp_time_delta_sum / delta_samples;
        }
        return summary;
    };

    auto build_pipeline_perf_sample = [&]() {
        const RecordingIngressStats recording_stats =
            recording_ingress ? recording_ingress->GetStats() : RecordingIngressStats{};
        PipelinePerfSample sample;
        sample.timestamp_utc = get_current_utc_timestamp();
        sample.frame_id = camera_state.frame_count;
        sample.recording_frame_id = last_recording_frame_count;
        sample.acquisition_fps = current_acquisition_fps;
        sample.preprocess_fps = recording_stats.preprocess_fps;
        sample.preprocess_fps_primary = recording_stats.preprocess_fps_primary;
        sample.preprocess_fps_helpers = recording_stats.preprocess_fps_helpers;
        sample.encode_fps = recording_stats.encode_fps;
        sample.encode_fps_primary = recording_stats.encode_fps_primary;
        sample.encode_fps_helpers = recording_stats.encode_fps_helpers;
        sample.display_queue_depth = openGLDisplay ? openGLDisplay->GetCountQueueInSize() : -1;
        sample.yolo_queue_depth = yolo_worker ? yolo_worker->GetCountQueueInSize() : -1;
        sample.preprocess_queue_depth = recording_stats.preprocess_queue_depth;
        sample.encode_queue_depth = recording_stats.encode_queue_depth;
        sample.free_entries_available = free_entries_available;
        sample.free_entries_low_watermark = free_entries_low;
        sample.free_events_available = free_events_available;
        sample.free_events_low_watermark = free_events_low;
        sample.yolo_events_available = yolo_events_available;
        sample.yolo_events_low_watermark = yolo_events_low;
        sample.pending_requeues = static_cast<int>(pending_requeues.size());
        sample.acquisition_resource_starvations = acquisition_resource_starvations;
        sample.preprocess_buffers_available = recording_stats.preprocess_buffers_available;
        sample.preprocess_events_available = recording_stats.preprocess_events_available;
        sample.preprocess_resource_waits = recording_stats.preprocess_resource_waits;
        sample.preprocess_frames_dropped = recording_stats.preprocess_frames_dropped;
        sample.encode_failures = recording_stats.encode_failures;
        sample.encode_slow_frames = recording_stats.encode_slow_frames;
        sample.submitted_frames = recording_stats.submitted_frames;
        sample.primary_routed_frames = recording_stats.primary_routed_frames;
        sample.helper_requested_frames = recording_stats.helper_requested_frames;
        sample.helper_fallback_frames = recording_stats.helper_fallback_frames;
        sample.helper_dispatched_frames = recording_stats.helper_dispatched_frames;
        sample.last_target_gpu_id = recording_stats.last_target_gpu_id;
        sample.last_route_mode = recording_stats.last_route_mode;
        sample.camera_dropped_frames = camera_state.dropped_frames;
        sample.gpu_direct_frames = gpu_direct_frames_total;
        sample.gpu_ring_copy_frames = gpu_ring_copy_frames_total;
        sample.gpu_copy_frames = gpu_copy_frames_total;
        return sample;
    };

    {
        NVTX_RANGE("Camera_Initialization");
        if (camera_control->sync_camera) {
            NVTX_RANGE_PUSH("PTP_Sync_Setup");
            show_ptp_offset(&ptp_state, ecam);
            start_ptp_sync(&ptp_state, ptp_params, camera_params, ecam, 3);
            NVTX_RANGE_POP();
        }

        NVTX_CAMERA("Camera_Acquisition_Start");
        check_camera_errors(EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStart"), camera_params->camera_serial.c_str());

        if (camera_control->sync_camera) {
            NVTX_RANGE_PUSH("PTP_Countdown");
            grab_frames_after_countdown(&ptp_state, ecam);
            NVTX_RANGE_POP();
        } else {
            try_start_timer();
        }
    }

    ptp_params->ptp_start_reached = true;
    w.Start();

#if PIPELINE_PROFILE
    struct CopyProfileEvents {
        cudaEvent_t start{};
        cudaEvent_t end{};
        int device = -1;
        bool initialized = false;

        void Init(int gpu_id) {
            if (initialized) {
                return;
            }
            device = gpu_id;
            ck(cudaSetDevice(device));
            ck(cudaEventCreate(&start));
            ck(cudaEventCreate(&end));
            initialized = true;
        }

        ~CopyProfileEvents() {
            if (!initialized) {
                return;
            }
            if (device >= 0) {
                cudaSetDevice(device);
            }
            cudaEventDestroy(start);
            cudaEventDestroy(end);
        }
    };

    static thread_local CopyProfileEvents copy_prof_events;
    static thread_local bool copy_prof_inflight = false;
    static thread_local int copy_prof_count = 0;
#endif

    while (camera_control->subscribe) {
        NVTX_RANGE_PUSH("Frame_Processing_Loop");

#if PIPELINE_PROFILE
        copy_prof_events.Init(camera_params->gpu_id);
        if (copy_prof_inflight) {
            cudaError_t copy_status = cudaEventQuery(copy_prof_events.end);
            if (copy_status == cudaSuccess) {
                float copy_ms = 0.0f;
                ck(cudaEventElapsedTime(&copy_ms, copy_prof_events.start, copy_prof_events.end));
                std::cout << "[COPY_TIME] Cam " << camera_params->camera_serial
                          << " GPU " << camera_params->gpu_id
                          << " ring_ms=" << copy_ms << std::endl;
                copy_prof_inflight = false;
            } else if (copy_status != cudaErrorNotReady) {
                std::cerr << "[COPY_TIME] Cam " << camera_params->camera_serial
                          << " event query failed: " << cudaGetErrorString(copy_status)
                          << std::endl;
                copy_prof_inflight = false;
            }
        }
#endif

        for (auto it = pending_requeues.begin(); it != pending_requeues.end(); ) {
            cudaError_t status = cudaEventQuery(*it->event);
            if (status == cudaSuccess) {
                EVT_CameraQueueFrame(it->camera, it->frame);
                it = pending_requeues.erase(it);
                continue;
            }
            if (status == cudaErrorNotReady) {
                ++it;
                continue;
            }
            std::cerr << "[GPU_DIRECT] Requeue event query failed: "
                      << cudaGetErrorString(status) << std::endl;
            EVT_CameraQueueFrame(it->camera, it->frame);
            it = pending_requeues.erase(it);
        }

        WORKER_ENTRY* recycled_entry = nullptr;
        while(resources->recycle_queue->pop(recycled_entry)) {
            if (recycled_entry) {
                if (recycled_entry->event_ptr) {
                    resources->free_events_queue->push(recycled_entry->event_ptr);
                    free_events_available++;
                }
                if (recycled_entry->yolo_completion_event) {
                    if (resources->yolo_events_queue) {
                        resources->yolo_events_queue->push(recycled_entry->yolo_completion_event);
                        yolo_events_available++;
                    }
                }
                resources->free_entries_queue->push(recycled_entry);
                free_entries_available++;
            }
        }

        WORKER_ENTRY* current_entry = nullptr;
        cudaEvent_t* current_event = nullptr;
        cudaEvent_t* yolo_event = nullptr;

        bool got_entry = resources->free_entries_queue->pop(current_entry);
        if (got_entry) {
            free_entries_available--;
            if (free_entries_available < free_entries_low) free_entries_low = free_entries_available;
        }
        bool got_event = resources->free_events_queue->pop(current_event);
        if (got_event) {
            free_events_available--;
            if (free_events_available < free_events_low) free_events_low = free_events_available;
        }

        if (!got_entry || !got_event) {
            acquisition_resource_starvations++;
            if (got_entry) {
                resources->free_entries_queue->push(current_entry);
                free_entries_available++;
            }
            if (got_event) {
                resources->free_events_queue->push(current_event);
                free_events_available++;
            }
            NVTX_RANGE_POP();
            usleep(100);
            continue;
        }

            camera_state.camera_return = EVT_CameraGetFrame(&ecam->camera, &ecam->frame_recv, 1000);

            if (camera_state.camera_return == EVT_SUCCESS) {
                if (camera_control->sync_camera) {
                PTP_timestamp_checking(&ptp_state, ecam, &camera_state, camera_params);
                }

            struct timespec ts_rt1;
            clock_gettime(CLOCK_REALTIME, &ts_rt1);
            uint64_t real_time = (ts_rt1.tv_sec * 1000000000LL) + ts_rt1.tv_nsec;
            camera_state.dropped_frames += count_camera_frame_id_gaps(
                camera_state.id_prev,
                ecam->frame_recv.frame_id);
            camera_state.id_prev = next_camera_frame_id_prev(ecam->frame_recv.frame_id);
            camera_state.frames_recd++;
            camera_state.frame_count++;
            current_entry->frame_id = camera_state.frame_count; // Assign absolute frame ID

        //     std::cout << "[DEBUG] Camera " << camera_params->camera_serial 
        //   << " incremented to frame_count=" << camera_state.frame_count 
        //   << " (entry->frame_id=" << current_entry->frame_id << ")" << std::endl;

            if (current_entry->d_image_pool) {
                current_entry->d_image = current_entry->d_image_pool;
            }
            current_entry->gpu_direct_mode = false;
            current_entry->owns_memory = true;
            current_entry->camera_buffer_ptr = nullptr;
            current_entry->camera_instance = nullptr;
            current_entry->camera_frame_struct = nullptr;

            bool will_display = (camera_select->stream_on && openGLDisplay);
            bool will_record = (camera_control->record_video && recording_ingress);
            bool yolo_enabled = (camera_select->yolo && yolo_worker);
            if (yolo_enabled && !last_yolo_enabled) {
                yolo_decimate_counter = 0;
            }
            last_yolo_enabled = yolo_enabled;
            bool will_yolo = yolo_enabled;
            if (yolo_enabled && yolo_decimate > 1) {
                if ((yolo_decimate_counter++ % static_cast<uint64_t>(yolo_decimate)) != 0) {
                    will_yolo = false;
                }
            }

            if (will_yolo) {
                if (!resources->yolo_events_queue) {
                    acquisition_resource_starvations++;
                    EVT_CameraQueueFrame(&ecam->camera, &ecam->frame_recv);
                    resources->free_events_queue->push(current_event);
                    resources->free_entries_queue->push(current_entry);
                    free_events_available++;
                    free_entries_available++;
                    NVTX_RANGE_POP();
                    usleep(100);
                    continue;
                }
                const bool got_yolo_event = resources->yolo_events_queue->pop(yolo_event);
                if (!got_yolo_event) {
                    acquisition_resource_starvations++;
                    EVT_CameraQueueFrame(&ecam->camera, &ecam->frame_recv);
                    resources->free_events_queue->push(current_event);
                    resources->free_entries_queue->push(current_entry);
                    free_events_available++;
                    free_entries_available++;
                    NVTX_RANGE_POP();
                    usleep(100);
                    continue;
                }
                yolo_events_available--;
                if (yolo_events_available < yolo_events_low) {
                    yolo_events_low = yolo_events_available;
                }
            }

            // If recording is active, increment and assign the recording-specific frame ID
            if (camera_control->record_video) {
                current_entry->recording_frame_id = ++local_recording_frame_count;
                last_recording_frame_count = current_entry->recording_frame_id;
            } else {
                current_entry->recording_frame_id = 0;
                local_recording_frame_count = 0;
            }

            const std::string live_recording_folder = current_recording_folder(camera_control);
            pipeline_perf_recorder.Rotate(live_recording_folder);
            if (live_recording_folder != ptp_summary_recording_folder) {
                if (!ptp_summary_recording_folder.empty() && camera_control->sync_camera) {
                    update_ptp_sync_summary_camera(
                        ptp_summary_recording_folder,
                        camera_params->camera_serial,
                        build_ptp_camera_summary_json(true));
                }
                ptp_summary_recording_folder = live_recording_folder;
                reset_ptp_summary_stats();
            }

            int dispatch_count = 0;
            if (will_display) dispatch_count++;
            if (will_record) dispatch_count++;
            if (will_yolo) dispatch_count++;

            cudaPointerAttributes attrs{};
            cudaError_t attr_status = cudaPointerGetAttributes(&attrs, ecam->frame_recv.imagePtr);
            if (attr_status != cudaSuccess) {
                gpu_direct_attr_errors++;
                cudaGetLastError(); // Clear error so we can continue.
            }
            bool use_direct_pointer = (attr_status == cudaSuccess &&
                                       attrs.type == cudaMemoryTypeDevice &&
                                       attrs.device == camera_params->gpu_id);
            bool use_ring_copy = (use_direct_pointer && dispatch_count > 1);
            if (use_direct_pointer &&
                camera_params &&
                camera_params->acquisition_buffer_mode == "force_ring_copy") {
                use_ring_copy = true;
            }

            if (attr_status == cudaSuccess) {
                if (attrs.type != cudaMemoryTypeDevice) {
                    gpu_direct_non_device++;
                } else if (attrs.device != camera_params->gpu_id) {
                    gpu_direct_wrong_device++;
                }
            }

            if (use_direct_pointer && !use_ring_copy) {
                gpu_direct_frames++;
                gpu_direct_frames_total++;
                current_entry->d_image = static_cast<unsigned char*>(ecam->frame_recv.imagePtr);
                current_entry->gpu_direct_mode = true;
                current_entry->owns_memory = false;
            } else {
#if PIPELINE_PROFILE
                bool sample_copy = false;
                if (use_ring_copy && !copy_prof_inflight) {
                    copy_prof_count++;
                    if (copy_prof_count % kCopyProfileLogEvery == 0) {
                        sample_copy = true;
                        ck(cudaEventRecord(copy_prof_events.start, stream));
                    }
                }
#endif
                if (use_ring_copy) {
                    gpu_ring_copy_frames++;
                    gpu_ring_copy_frames_total++;
                } else {
                    gpu_copy_frames++;
                    gpu_copy_frames_total++;
                }
                ck(cudaMemcpyAsync(current_entry->d_image, ecam->frame_recv.imagePtr, ecam->frame_recv.bufferSize, cudaMemcpyDeviceToDevice, stream));
#if PIPELINE_PROFILE
                if (sample_copy) {
                    ck(cudaEventRecord(copy_prof_events.end, stream));
                    copy_prof_inflight = true;
                }
#endif
                current_entry->gpu_direct_mode = false;
                current_entry->owns_memory = true;
                current_entry->camera_buffer_ptr = nullptr;
                current_entry->camera_instance = nullptr;
                current_entry->camera_frame_struct = nullptr;
                if (!use_direct_pointer) {
                    EVT_CameraQueueFrame(&ecam->camera, &ecam->frame_recv);
                }
            }

            current_entry->event_ptr = current_event;
            current_entry->yolo_completion_event = yolo_event;
            ck(cudaEventRecord(*current_entry->event_ptr, stream));
            if (use_ring_copy) {
                pending_requeues.push_back({&ecam->camera, &ecam->frame_recv, current_entry->event_ptr});
            }

            current_entry->width = ecam->frame_recv.size_x;
            current_entry->height = ecam->frame_recv.size_y;
            current_entry->pixelFormat = ecam->frame_recv.pixel_type;
            current_entry->timestamp = ecam->frame_recv.timestamp;
            current_entry->timestamp_sys = real_time;
            current_entry->frame_id = camera_state.frame_count;
            current_entry->has_detections = will_yolo;
            current_entry->detections_ready.store(false);
            current_entry->ipc_frame_id = 0;

            if (camera_control->sync_camera) {
                const int64_t latch_minus_frame_ns =
                    static_cast<int64_t>(ptp_state.ptp_time) -
                    static_cast<int64_t>(ptp_state.frame_ts);
                PtpReceiveHistoryEntry receive_entry;
                receive_entry.local_frame_id = camera_state.frame_count;
                receive_entry.camera_frame_id = ecam->frame_recv.frame_id;
                receive_entry.frame_timestamp_ns = current_entry->timestamp;
                receive_entry.timestamp_sys_ns = current_entry->timestamp_sys;
                receive_entry.latch_minus_frame_ns = latch_minus_frame_ns;
                receive_entry.frame_delta_ns = ptp_state.frame_ts_delta;
                receive_entry.latch_delta_ns = ptp_state.ptp_time_delta;
                receive_entry.camera_dropped_frames = camera_state.dropped_frames;
                receive_entry.dispatch_count = dispatch_count;
                receive_entry.free_entries_available = free_entries_available;
                receive_entry.free_events_available = free_events_available;
                receive_entry.yolo_events_available = yolo_events_available;
                receive_entry.pending_requeues = pending_requeues.size();
                receive_entry.acquisition_resource_starvations =
                    acquisition_resource_starvations;
                receive_entry.will_display = will_display;
                receive_entry.will_record = will_record;
                receive_entry.will_yolo = will_yolo;
                receive_entry.use_direct_pointer = use_direct_pointer;
                receive_entry.use_ring_copy = use_ring_copy;
                append_ptp_receive_history(&ptp_receive_history, receive_entry);

                if (!ptp_stale_history_dumped &&
                    latch_minus_frame_ns > kPtpStaleReceiveThresholdNs) {
                    dump_ptp_receive_history(
                        camera_params,
                        camera_control,
                        ptp_receive_history,
                        receive_entry);
                    if (!recording_submit_history.empty()) {
                        dump_recording_submit_history(
                            camera_params,
                            recording_submit_history);
                    }
                    ptp_stale_history_dumped = true;
                }
            }

            // FRAME_IPC: Send frame data immediately after capture
            // This ensures EVERY frame is sent for synchronization
            if (ipc_manager) {
                // ALWAYS use recording_frame_id when it's valid (non-zero or recording active)
                // This ensures consistency - if a frame gets recorded, we use its recording ID
                // Otherwise, fall back to absolute frame_id
                uint64_t frame_id_for_ipc;
                
                if (camera_control->record_video && current_entry->recording_frame_id > 0) {
                    // Recording is active and we have a valid recording frame ID
                    frame_id_for_ipc = current_entry->recording_frame_id;
                } else {
                    // Not recording or recording just stopped - use absolute frame ID
                    // This ensures continuity even when recording toggles
                    frame_id_for_ipc = current_entry->frame_id;
                }
                
                current_entry->ipc_frame_id = frame_id_for_ipc;
                bool yolo_will_process = will_yolo;
                
                // Log the frame ID being sent (for debugging)
                static uint64_t last_logged_frame = 0;
                if (frame_id_for_ipc != last_logged_frame + 1 && last_logged_frame != 0) {
                    // IPC logging disabled: non-sequential frame ID warning.
                }
                last_logged_frame = frame_id_for_ipc;
                
                bool ipc_success = ipc_manager->sendFrame(
                    frame_id_for_ipc,
                    current_entry->timestamp,
                    yolo_will_process
                );
                
                if (!ipc_success) {
                    // IPC logging disabled: queue full warning.
                }
            }
            
            // FRAME_IPC: Store IPC manager pointer in entry for YOLO to use later
            // This allows YOLO to update the frame with detection data
            current_entry->frame_ipc_manager = ipc_manager;

            if (camera_select->frame_save_state == State_Write_New_Frame && image_writer) {
                ImageWriter_Entry* save_job = new ImageWriter_Entry();
                save_job->event_ptr = current_event;
                image_writer->PutObjectToQueueIn(save_job);
            }
            
            // Create a dispatch counter to track how many workers will process this frame
            // If the camera is set to stream, record video, or run YOLO detection,
            // we increment the dispatch count for each active worker.
            // If no workers are active, we return the frame to the free queue.
            // This allows us to efficiently manage resources and avoid unnecessary processing.
            if (dispatch_count > 0) {
                current_entry->ref_count.store(dispatch_count);

                if (use_direct_pointer && !use_ring_copy) {
                    current_entry->camera_buffer_ptr = ecam->frame_recv.imagePtr;
                    current_entry->camera_instance = &ecam->camera;
                    current_entry->camera_frame_struct = &ecam->frame_recv;
                }

                if (will_display) openGLDisplay->PutObjectToQueueIn(current_entry);
                if (will_record) {
                    recording_ingress->SubmitFrame(current_entry);
                    if (camera_control->sync_camera) {
                        RecordingSubmitHistoryEntry submit_entry;
                        submit_entry.local_frame_id = current_entry->frame_id;
                        submit_entry.recording_frame_id = current_entry->recording_frame_id;
                        submit_entry.camera_frame_id = ecam->frame_recv.frame_id;
                        submit_entry.dispatch_count = dispatch_count;
                        submit_entry.use_direct_pointer = use_direct_pointer;
                        submit_entry.use_ring_copy = use_ring_copy;
                        submit_entry.ingress_stats = recording_ingress->GetStats();
                        append_recording_submit_history(
                            &recording_submit_history,
                            submit_entry);
                    }
                }
                if (will_yolo) yolo_worker->PutObjectToQueueIn(current_entry);

            } else {
                // FRAME_IPC: Important - even if no workers are active, we still sent the frame IPC above
                // This ensures frame synchronization works even when just recording without display/YOLO
                if (use_direct_pointer && !use_ring_copy) {
                    EVT_CameraQueueFrame(&ecam->camera, &ecam->frame_recv);
                }
                resources->free_events_queue->push(current_event);
                if (yolo_event) {
                    resources->yolo_events_queue->push(yolo_event);
                    yolo_events_available++;
                }
                resources->free_entries_queue->push(current_entry);
                free_events_available++;
                free_entries_available++;
            }

            frame_counter_for_fps++;
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> elapsed = now - last_fps_update_time;
            if (elapsed.count() >= 1.0) {
                current_acquisition_fps = frame_counter_for_fps / elapsed.count();
                streaming_fps.store(current_acquisition_fps);
                frame_counter_for_fps = 0;
                last_fps_update_time = now;
            }

            std::chrono::duration<double> direct_elapsed = now - last_gpu_direct_log_time;
            if (direct_elapsed.count() >= 1.0) {
                if (camera_control->sync_camera && camera_state.frame_count > 0) {
                    const int64_t latch_minus_frame_ns =
                        static_cast<int64_t>(ptp_state.ptp_time) - static_cast<int64_t>(ptp_state.frame_ts);
                    const uint64_t delta_samples =
                        (camera_state.frame_count > 1) ? (camera_state.frame_count - 1) : 0;
                    const uint64_t avg_frame_delta_ns =
                        (delta_samples > 0) ? (ptp_state.frame_ts_delta_sum / delta_samples) : 0;
                    const uint64_t avg_latch_delta_ns =
                        (delta_samples > 0) ? (ptp_state.ptp_time_delta_sum / delta_samples) : 0;
                    int32_t current_ptp_offset = 0;
                    EVT_ERROR ptp_offset_ret = EVT_CameraGetInt32Param(&ecam->camera, "PtpOffset", &current_ptp_offset);
                    if (ptp_offset_ret == EVT_SUCCESS) {
                        ptp_offset_stats.add(current_ptp_offset);
                    }
                    latch_minus_frame_stats.add(latch_minus_frame_ns);
                    frame_delta_stats.add(static_cast<int64_t>(ptp_state.frame_ts_delta));
                    latch_delta_stats.add(static_cast<int64_t>(ptp_state.ptp_time_delta));
                    if (!ptp_summary_recording_folder.empty()) {
                        update_ptp_sync_summary_camera(
                            ptp_summary_recording_folder,
                            camera_params->camera_serial,
                            build_ptp_camera_summary_json(false));
                    }
                    if (ptp_offset_ret == EVT_SUCCESS) {
                        std::cout << "[PTP_LIVE] Cam " << camera_params->camera_serial
                                  << " frame=" << camera_state.frame_count
                                  << " ptp_offset_ns=" << current_ptp_offset
                                  << " latch_minus_frame_ns=" << latch_minus_frame_ns
                                  << " frame_delta_ns=" << ptp_state.frame_ts_delta
                                  << " latch_delta_ns=" << ptp_state.ptp_time_delta
                                  << " avg_frame_delta_ns=" << avg_frame_delta_ns
                                  << " avg_latch_delta_ns=" << avg_latch_delta_ns
                                  << std::endl;
                    } else {
                        std::cout << "[PTP_LIVE] Cam " << camera_params->camera_serial
                                  << " frame=" << camera_state.frame_count
                                  << " ptp_offset_ns=NA"
                                  << " latch_minus_frame_ns=" << latch_minus_frame_ns
                                  << " frame_delta_ns=" << ptp_state.frame_ts_delta
                                  << " latch_delta_ns=" << ptp_state.ptp_time_delta
                                  << " avg_frame_delta_ns=" << avg_frame_delta_ns
                                  << " avg_latch_delta_ns=" << avg_latch_delta_ns
                                  << std::endl;
                    }
                }
                std::cout << "[GPU_DIRECT] Cam " << camera_params->camera_serial
                          << " GPU " << camera_params->gpu_id
                          << " direct=" << gpu_direct_frames
                          << " ring=" << gpu_ring_copy_frames
                          << " copy=" << gpu_copy_frames
                          << " attr_err=" << gpu_direct_attr_errors
                          << " non_dev=" << gpu_direct_non_device
                          << " wrong_dev=" << gpu_direct_wrong_device
                          << " stream=" << (camera_select->stream_on ? "on" : "off")
                          << " yolo=" << (camera_select->yolo ? "on" : "off")
                          << " record=" << (camera_control->record_video ? "on" : "off")
                          << std::endl;
                const PipelinePerfSample pipeline_sample = build_pipeline_perf_sample();
                if (!live_recording_folder.empty()) {
                    pipeline_perf_recorder.Record(pipeline_sample);
                }
                std::cout << "[PIPELINE] Cam " << camera_params->camera_serial
                          << " acq_fps=" << pipeline_sample.acquisition_fps
                          << " pre_fps=" << pipeline_sample.preprocess_fps
                          << " enc_fps=" << pipeline_sample.encode_fps
                          << " free=" << pipeline_sample.free_entries_available
                          << "/" << pipeline_sample.free_entries_low_watermark
                          << " ev=" << pipeline_sample.free_events_available
                          << "/" << pipeline_sample.free_events_low_watermark
                          << " yev=" << pipeline_sample.yolo_events_available
                          << "/" << pipeline_sample.yolo_events_low_watermark
                          << " pend=" << pipeline_sample.pending_requeues
                          << " acq_starve=" << pipeline_sample.acquisition_resource_starvations
                          << " q(d/y/p/e)="
                          << pipeline_sample.display_queue_depth << "/"
                          << pipeline_sample.yolo_queue_depth << "/"
                          << pipeline_sample.preprocess_queue_depth << "/"
                          << pipeline_sample.encode_queue_depth
                          << " enc_buf=" << pipeline_sample.preprocess_buffers_available
                          << " enc_evt=" << pipeline_sample.preprocess_events_available
                          << " enc_waits=" << pipeline_sample.preprocess_resource_waits
                          << " pre_drop=" << pipeline_sample.preprocess_frames_dropped
                          << " enc_fail=" << pipeline_sample.encode_failures
                          << " enc_slow=" << pipeline_sample.encode_slow_frames
                          << std::endl;
                gpu_direct_frames = 0;
                gpu_ring_copy_frames = 0;
                gpu_copy_frames = 0;
                gpu_direct_attr_errors = 0;
                gpu_direct_non_device = 0;
                gpu_direct_wrong_device = 0;
                last_gpu_direct_log_time = now;
                free_entries_low = free_entries_available;
                free_events_low = free_events_available;
                yolo_events_low = yolo_events_available;
            }
            if (ptp_params->network_sync &&
                ptp_params->network_set_stop_ptp &&
                camera_control->sync_camera &&
                ptp_state.ptp_time > ptp_params->ptp_stop_time) {
                uint64_t ptp_stop_counter = sync_fetch_and_add(&ptp_params->ptp_stop_counter, 1);
                std::cout << "[PTP_STOP] Cam " << camera_params->camera_serial
                          << " reached stop gate count=" << ptp_stop_counter
                          << " target_ns=" << ptp_params->ptp_stop_time
                          << " observed_ns=" << ptp_state.ptp_time
                          << std::endl;
                while (ptp_params->ptp_stop_counter != camera_params->num_cameras) {
                    usleep(10);
                }
                ptp_params->ptp_stop_reached = true;
                camera_control->subscribe = false;
                NVTX_RANGE_POP();
                break;
            }
        } else {
            camera_state.dropped_frames++;
            std::cerr << "EVT_CameraGetFrame Error, " << camera_state.camera_return
                      << ", camera serial, " << camera_params->camera_serial << std::endl;
            if (current_event) {
                resources->free_events_queue->push(current_event);
                free_events_available++;
            }
            if (yolo_event) {
                resources->yolo_events_queue->push(yolo_event);
                yolo_events_available++;
            }
            if (current_entry) {
                resources->free_entries_queue->push(current_entry);
                free_entries_available++;
            }
        }
        NVTX_RANGE_POP();
    }

    for (const auto& pending : pending_requeues) {
        cudaEventSynchronize(*pending.event);
        EVT_CameraQueueFrame(pending.camera, pending.frame);
    }
    pending_requeues.clear();

    if (camera_control->sync_camera && !ptp_summary_recording_folder.empty()) {
        update_ptp_sync_summary_camera(
            ptp_summary_recording_folder,
            camera_params->camera_serial,
            build_ptp_camera_summary_json(true));
    }
    if (!pipeline_perf_recorder.current_folder().empty()) {
        const PipelinePerfSample final_pipeline_sample = build_pipeline_perf_sample();
        pipeline_perf_recorder.Record(final_pipeline_sample);
    }
    pipeline_perf_recorder.Close();

    // Cleanup
    {
        NVTX_RANGE("Cleanup_and_Shutdown");
        CUDA_CTX_LOG("=== ACQUIRE FRAMES CLEANUP ===");

        // FRAME_IPC: Log final statistics if IPC was active
        if (ipc_manager) {
            // IPC logging disabled: final stats.
        }

        {
            NVTX_CAMERA("Camera_Acquisition_Stop");
            check_camera_errors(EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStop"), camera_params->camera_serial.c_str());
        }

        if (!ptp_params->network_sync) {
            try_stop_timer();
        }
        double time_diff = w.Stop();
        const RecordingIngressStats final_recording_stats =
            recording_ingress ? recording_ingress->GetStats() : RecordingIngressStats{};
        report_statistics(
            camera_params,
            &camera_state,
            time_diff,
            final_recording_stats.preprocess_resource_waits,
            final_recording_stats.preprocess_frames_dropped,
            final_recording_stats.encode_failures,
            final_recording_stats.encode_slow_frames);

        {
            NVTX_RANGE("Memory_Cleanup");
            cudaFree(frame_process_save.frame_original.d_orig);
            cudaFree(frame_process_save.debayer.d_debayer);
            cudaFree(frame_process_save.d_convert);
            free(frame_process_save.frame_cpu.frame);
        }

        CUDA_STREAM_LOG("Destroying acquisition stream", stream);
        cudaStreamDestroy(stream);

        CUDA_CTX_LOG("=== ACQUIRE FRAMES END ===");
        std::cout << "Acquire frames thread finished for camera: " << camera_params->camera_serial << std::endl;

        CUcontext popped_context;
    }
}
