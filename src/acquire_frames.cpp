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
#include "yolo_worker.h"
#include "image_writer_worker.h"
#include "crop_and_encode_worker.h"
#include "display_preview_policy.h"
#include "recording_ingress.h"
#include "spatial_snapshot_worker.h"
#include "cuda_context_debug.h"
#include "frame_ipc_manager.h"
#include "fsuid_guard.h"
#include "latency_stats.h"
#include "project.h"
#include "worker_entry_release.h"
#include "yolo_event_log.h"
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>

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
static constexpr uint64_t kAcquisitionCadenceProbeFrameMin = 80;
static constexpr uint64_t kAcquisitionCadenceProbeFrameMax = 160;

namespace {
bool env_flag_enabled(const char* name, bool default_value)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return default_value;
    }

    std::string normalized(value);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized != "0" &&
           normalized != "false" &&
           normalized != "off" &&
           normalized != "no";
}

int env_positive_int(const char* name, int default_value, int max_value)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return default_value;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 1 || parsed > max_value) {
        std::cerr << "[ACQ] Ignoring invalid " << name << "='" << value
                  << "', using " << default_value << std::endl;
        return default_value;
    }
    return static_cast<int>(parsed);
}

int ptp_register_read_decimate()
{
    static const int decimate = []() {
        const int value = env_positive_int(
            "ORANGE_PTP_REGISTER_READ_DECIMATE",
            1,
            1000000);
        if (value > 1) {
            std::cout << "[PTP] GevTimestampValue register reads decimated 1/"
                      << value << " frames" << std::endl;
        }
        return value;
    }();
    return decimate;
}

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
    int display_preview_max_fps = -1;
    uint64_t display_preview_eligible_frames = 0;
    uint64_t display_preview_selected_frames = 0;
    uint64_t display_preview_skipped_frames = 0;
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
    uint64_t worker_enqueue_rejections = 0;
    uint64_t worker_entry_release_underflows = 0;
    uint64_t worker_entry_double_releases = 0;
    uint64_t worker_entry_retain_after_release_count = 0;
    uint64_t worker_entry_release_underflows_global = 0;
    uint64_t worker_entry_double_releases_global = 0;
    uint64_t worker_entry_retain_after_release_global = 0;
    uint64_t worker_entry_release_underflows_camera = 0;
    uint64_t worker_entry_double_releases_camera = 0;
    uint64_t worker_entry_retain_after_release_camera = 0;
    int preprocess_buffers_available = -1;
    int preprocess_events_available = -1;
    uint64_t preprocess_resource_waits = 0;
    uint64_t preprocess_frames_dropped = 0;
    uint64_t detect_priority_gated_frames = 0;
    uint64_t detect_priority_waited_frames = 0;
    uint64_t detect_priority_wait_timeouts = 0;
    uint64_t detect_priority_wait_total_ns = 0;
    uint64_t detect_priority_wait_max_ns = 0;
    uint64_t encode_failures = 0;
    uint64_t encode_slow_frames = 0;
    uint64_t external_ipc_frames_acked = 0;
    uint64_t external_ipc_failures = 0;
    uint64_t external_ipc_ack_timeouts = 0;
    uint64_t submitted_frames = 0;
    uint64_t enqueue_rejected_frames = 0;
    uint64_t primary_routed_frames = 0;
    uint64_t helper_requested_frames = 0;
    uint64_t helper_fallback_frames = 0;
    uint64_t helper_dispatched_frames = 0;
    int last_target_gpu_id = -1;
    std::string last_route_mode = "primary";
    uint64_t camera_dropped_frames = 0;
    uint64_t get_frame_errors = 0;
    int last_get_frame_error_code = 0;
    std::map<int, uint64_t> get_frame_errors_by_code;
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
    uint64_t get_frame_errors = 0;
    int last_get_frame_error_code = 0;
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

struct AcquisitionCadenceProbeSample {
    std::string timestamp_utc;
    uint64_t local_frame_id = 0;
    uint64_t recording_frame_id = 0;
    uint64_t camera_frame_id = 0;
    uint64_t camera_timestamp_ns = 0;
    uint64_t camera_timestamp_delta_ns = 0;
    uint64_t receive_host_ns = 0;
    uint64_t receive_delta_ns = 0;
    uint64_t get_frame_wait_ns = 0;
    bool ptp_active = false;
    bool ptp_register_read = false;
    int ptp_register_read_decimate = 1;
    uint64_t ptp_register_read_age_frames = 0;
    uint64_t latched_ptp_time_ns = 0;
    int64_t latch_minus_frame_ns = 0;
    uint64_t latch_delta_ns = 0;
    bool record_active = false;
    int dispatch_count = 0;
    bool will_display = false;
    int display_preview_max_fps = -1;
    uint64_t display_preview_eligible_frames = 0;
    uint64_t display_preview_selected_frames = 0;
    uint64_t display_preview_skipped_frames = 0;
    bool will_record = false;
    bool will_yolo = false;
    bool use_direct_pointer = false;
    bool use_ring_copy = false;
    int free_entries_available = -1;
    int free_entries_low_watermark = -1;
    int free_events_available = -1;
    int free_events_low_watermark = -1;
    int yolo_events_available = -1;
    int yolo_events_low_watermark = -1;
    size_t pending_requeues = 0;
    uint64_t acquisition_resource_starvations = 0;
    uint64_t worker_enqueue_rejections = 0;
    uint64_t camera_dropped_frames = 0;
    uint64_t get_frame_errors = 0;
    int last_get_frame_error_code = 0;
    uint64_t recording_submit_host_ns = 0;
    uint64_t receive_to_submit_ns = 0;
    int recording_target_gpu_id = -1;
    bool recording_helper_requested = false;
    bool recording_route_helper = false;
    int helper_enqueue_queue_depth = -1;
    int helper_enqueue_available_buffers = -1;
    int helper_enqueue_available_events = -1;
    uint64_t helper_enqueue_delay_ns = 0;
    RecordingIngressStats ingress_stats;
};

class AcquisitionCadenceProbeRecorder {
public:
    explicit AcquisitionCadenceProbeRecorder(const CameraParams* camera_params)
        : camera_params_(camera_params) {}

    ~AcquisitionCadenceProbeRecorder() {
        Close();
    }

    void Rotate(const std::string& folder) {
        if (folder == current_folder_) {
            return;
        }
        Close();
        if (!folder.empty()) {
            OpenFile(folder);
        }
    }

    void Record(const AcquisitionCadenceProbeSample& sample) {
        if (!file_.is_open() || !ShouldRecord(sample)) {
            return;
        }

        file_ << sample.timestamp_utc << ","
              << sample.local_frame_id << ","
              << sample.recording_frame_id << ","
              << sample.camera_frame_id << ","
              << sample.camera_timestamp_ns << ","
              << sample.camera_timestamp_delta_ns << ","
              << sample.receive_host_ns << ","
              << sample.receive_delta_ns << ","
              << sample.get_frame_wait_ns << ","
              << (sample.ptp_active ? 1 : 0) << ","
              << (sample.ptp_register_read ? 1 : 0) << ","
              << sample.ptp_register_read_decimate << ","
              << sample.ptp_register_read_age_frames << ","
              << sample.latched_ptp_time_ns << ","
              << sample.latch_minus_frame_ns << ","
              << sample.latch_delta_ns << ","
              << (sample.record_active ? 1 : 0) << ","
              << sample.dispatch_count << ","
              << (sample.will_display ? 1 : 0) << ","
              << sample.display_preview_max_fps << ","
              << sample.display_preview_eligible_frames << ","
              << sample.display_preview_selected_frames << ","
              << sample.display_preview_skipped_frames << ","
              << (sample.will_record ? 1 : 0) << ","
              << (sample.will_yolo ? 1 : 0) << ","
              << (sample.use_direct_pointer ? 1 : 0) << ","
              << (sample.use_ring_copy ? 1 : 0) << ","
              << sample.free_entries_available << ","
              << sample.free_entries_low_watermark << ","
              << sample.free_events_available << ","
              << sample.free_events_low_watermark << ","
              << sample.yolo_events_available << ","
              << sample.yolo_events_low_watermark << ","
              << sample.pending_requeues << ","
              << sample.acquisition_resource_starvations << ","
              << sample.worker_enqueue_rejections << ","
              << sample.camera_dropped_frames << ","
              << sample.get_frame_errors << ","
              << sample.last_get_frame_error_code << ","
              << sample.recording_submit_host_ns << ","
              << sample.receive_to_submit_ns << ","
              << sample.recording_target_gpu_id << ","
              << (sample.recording_helper_requested ? 1 : 0) << ","
              << (sample.recording_route_helper ? 1 : 0) << ","
              << sample.helper_enqueue_queue_depth << ","
              << sample.helper_enqueue_available_buffers << ","
              << sample.helper_enqueue_available_events << ","
              << sample.helper_enqueue_delay_ns << ","
              << sample.ingress_stats.submitted_frames << ","
              << sample.ingress_stats.enqueue_rejected_frames << ","
              << sample.ingress_stats.primary_routed_frames << ","
              << sample.ingress_stats.helper_requested_frames << ","
              << sample.ingress_stats.helper_fallback_frames << ","
              << sample.ingress_stats.helper_dispatched_frames << ","
              << sample.ingress_stats.last_target_gpu_id << ","
              << sample.ingress_stats.last_route_mode << ","
              << sample.ingress_stats.preprocess_queue_depth << ","
              << sample.ingress_stats.encode_queue_depth << ","
              << sample.ingress_stats.preprocess_buffers_available << ","
              << sample.ingress_stats.preprocess_events_available << ","
              << sample.ingress_stats.preprocess_resource_waits << ","
              << sample.ingress_stats.preprocess_frames_dropped << ","
              << sample.ingress_stats.encode_failures << ","
              << sample.ingress_stats.encode_slow_frames << "\n";
        file_.flush();
    }

    void Close() {
        if (file_.is_open()) {
            file_.close();
        }
        current_folder_.clear();
        file_path_.clear();
    }

private:
    bool ShouldRecord(const AcquisitionCadenceProbeSample& sample) const {
        const uint64_t probe_frame =
            sample.recording_frame_id > 0 ? sample.recording_frame_id : sample.local_frame_id;
        return probe_frame >= kAcquisitionCadenceProbeFrameMin &&
               probe_frame <= kAcquisitionCadenceProbeFrameMax;
    }

    void OpenFile(const std::string& folder) {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        make_folder(folder);

        current_folder_ = folder;
        const std::string serial = camera_params_ ? camera_params_->camera_serial : "unknown";
        file_path_ = (std::filesystem::path(current_folder_) /
                      ("Cam" + serial + "_acquisition_cadence_probe.csv")).string();
        file_.open(file_path_, std::ios::out | std::ios::trunc);
        if (!file_) {
            std::cerr << "[ACQ_CADENCE] Cam " << serial
                      << " failed to open " << file_path_ << std::endl;
            current_folder_.clear();
            file_path_.clear();
            return;
        }
        file_ << "timestamp_utc,local_frame_id,recording_frame_id,camera_frame_id,"
                 "camera_timestamp_ns,camera_timestamp_delta_ns,"
                 "receive_host_ns,receive_delta_ns,get_frame_wait_ns,"
                 "ptp_active,ptp_register_read,ptp_register_read_decimate,ptp_register_read_age_frames,latched_ptp_time_ns,latch_minus_frame_ns,latch_delta_ns,"
                 "record_active,dispatch_count,will_display,"
                 "display_preview_max_fps,display_preview_eligible,"
                 "display_preview_selected,display_preview_skipped,"
                 "will_record,will_yolo,"
                 "direct,ring_copy,"
                 "free_entries,free_entries_low,free_events,free_events_low,"
                 "yolo_events,yolo_events_low,pending_requeues,acq_starve,"
                 "worker_enqueue_rejections,"
                 "camera_dropped_frames,get_frame_errors,last_get_frame_error_code,"
                 "recording_submit_host_ns,receive_to_submit_ns,"
                 "recording_target_gpu_id,recording_helper_requested,recording_route_helper,"
                 "helper_enqueue_q,helper_enqueue_buffers,helper_enqueue_events,helper_enqueue_delay_ns,"
                 "submitted_frames,enqueue_rejected_frames,primary_routed_frames,helper_requested_frames,"
                 "helper_fallback_frames,helper_dispatched_frames,last_target_gpu_id,last_route_mode,"
                 "pre_q,enc_q,pre_buffers,pre_events,pre_waits,pre_drops,enc_fail,enc_slow\n";
        std::cout << "[ACQ_CADENCE] Cam " << serial
                  << " logging frames "
                  << kAcquisitionCadenceProbeFrameMin << "-"
                  << kAcquisitionCadenceProbeFrameMax
                  << " to " << file_path_ << std::endl;
    }

    const CameraParams* camera_params_ = nullptr;
    std::string current_folder_;
    std::string file_path_;
    std::ofstream file_;
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
                  << " get_frame_errors=" << entry.get_frame_errors
                  << " last_get_frame_error_code=" << entry.last_get_frame_error_code
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
                  << " enqueue_rejected=" << entry.ingress_stats.enqueue_rejected_frames
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

int resolve_display_preview_max_fps(const CameraEachSelect* camera_select)
{
    int configured = camera_select ? camera_select->display_preview_max_fps : 60;
    const char* env = std::getenv("ORANGE_DISPLAY_PREVIEW_MAX_FPS");
    if (!env || !*env) {
        env = std::getenv("ORANGE_DISPLAY_MAX_FPS");
    }
    if (env && *env) {
        char* end = nullptr;
        const long parsed = std::strtol(env, &end, 10);
        if (end != env && *end == '\0' && parsed >= 0 && parsed <= 10000) {
            configured = static_cast<int>(parsed);
        } else {
            std::cerr << "[DISPLAY_PREVIEW] Ignoring invalid preview fps '"
                      << env << "'" << std::endl;
        }
    }
    return configured;
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
              << sample.display_preview_max_fps << ","
              << sample.display_preview_eligible_frames << ","
              << sample.display_preview_selected_frames << ","
              << sample.display_preview_skipped_frames << ","
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
              << sample.worker_enqueue_rejections << ","
              << sample.worker_entry_release_underflows << ","
              << sample.worker_entry_double_releases << ","
              << sample.worker_entry_retain_after_release_count << ","
              << sample.worker_entry_release_underflows_global << ","
              << sample.worker_entry_double_releases_global << ","
              << sample.worker_entry_retain_after_release_global << ","
              << sample.worker_entry_release_underflows_camera << ","
              << sample.worker_entry_double_releases_camera << ","
              << sample.worker_entry_retain_after_release_camera << ","
              << sample.preprocess_buffers_available << ","
              << sample.preprocess_events_available << ","
              << sample.preprocess_resource_waits << ","
              << sample.preprocess_frames_dropped << ","
              << sample.detect_priority_gated_frames << ","
              << sample.detect_priority_waited_frames << ","
              << sample.detect_priority_wait_timeouts << ","
              << sample.detect_priority_wait_total_ns << ","
              << sample.detect_priority_wait_max_ns << ","
              << sample.encode_failures << ","
              << sample.encode_slow_frames << ","
              << sample.external_ipc_frames_acked << ","
              << sample.external_ipc_failures << ","
              << sample.external_ipc_ack_timeouts << ","
              << sample.submitted_frames << ","
              << sample.enqueue_rejected_frames << ","
              << sample.primary_routed_frames << ","
              << sample.helper_requested_frames << ","
              << sample.helper_fallback_frames << ","
              << sample.helper_dispatched_frames << ","
              << sample.last_target_gpu_id << ","
              << sample.last_route_mode << ","
              << sample.camera_dropped_frames << ","
              << sample.get_frame_errors << ","
              << sample.last_get_frame_error_code << ","
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
                 "display_q,display_preview_max_fps,display_preview_eligible,display_preview_selected,display_preview_skipped,"
                 "yolo_q,pre_q,enc_q,"
                 "acq_free_entries,acq_free_entries_low,acq_free_events,acq_free_events_low,"
                 "yolo_events,yolo_events_low,pending_requeues,"
                 "acq_starve,worker_enqueue_rejections,worker_entry_release_underflows,worker_entry_double_releases,"
                 "worker_entry_retain_after_release_count,"
                 "worker_entry_release_underflows_global,worker_entry_double_releases_global,worker_entry_retain_after_release_global,"
                 "worker_entry_release_underflows_camera,worker_entry_double_releases_camera,worker_entry_retain_after_release_camera,"
                 "pre_buffers,pre_events,pre_waits,pre_drops,"
                 "detect_priority_gated_frames,detect_priority_waited_frames,detect_priority_wait_timeouts,"
                 "detect_priority_wait_total_ns,detect_priority_wait_max_ns,"
                 "enc_fail,enc_slow,"
                 "external_ipc_frames_acked,external_ipc_failures,external_ipc_ack_timeouts,"
                 "submitted_frames,enqueue_rejected_frames,primary_routed_frames,helper_requested_frames,helper_fallback_frames,helper_dispatched_frames,last_target_gpu_id,last_route_mode,"
                 "camera_dropped_frames,get_frame_errors,last_get_frame_error_code,"
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
            totals["detect_priority_gated_frames"] = last_sample_.detect_priority_gated_frames;
            totals["detect_priority_waited_frames"] = last_sample_.detect_priority_waited_frames;
            totals["detect_priority_wait_timeouts"] = last_sample_.detect_priority_wait_timeouts;
            totals["detect_priority_wait_total_ns"] = last_sample_.detect_priority_wait_total_ns;
            totals["detect_priority_wait_max_ns"] = last_sample_.detect_priority_wait_max_ns;
            totals["encode_failures"] = last_sample_.encode_failures;
            totals["encode_slow_frames"] = last_sample_.encode_slow_frames;
            totals["external_ipc_frames_acked"] = last_sample_.external_ipc_frames_acked;
            totals["external_ipc_failures"] = last_sample_.external_ipc_failures;
            totals["external_ipc_ack_timeouts"] = last_sample_.external_ipc_ack_timeouts;
            totals["worker_enqueue_rejections"] = last_sample_.worker_enqueue_rejections;
            totals["worker_entry_release_underflows"] =
                last_sample_.worker_entry_release_underflows;
            totals["worker_entry_double_releases"] =
                last_sample_.worker_entry_double_releases;
            totals["worker_entry_retain_after_release_count"] =
                last_sample_.worker_entry_retain_after_release_count;
            totals["worker_entry_release_underflows_global"] =
                last_sample_.worker_entry_release_underflows_global;
            totals["worker_entry_double_releases_global"] =
                last_sample_.worker_entry_double_releases_global;
            totals["worker_entry_retain_after_release_global"] =
                last_sample_.worker_entry_retain_after_release_global;
            totals["worker_entry_release_underflows_camera"] =
                last_sample_.worker_entry_release_underflows_camera;
            totals["worker_entry_double_releases_camera"] =
                last_sample_.worker_entry_double_releases_camera;
            totals["worker_entry_retain_after_release_camera"] =
                last_sample_.worker_entry_retain_after_release_camera;
            totals["submitted_frames"] = last_sample_.submitted_frames;
            totals["enqueue_rejected_frames"] = last_sample_.enqueue_rejected_frames;
            totals["primary_routed_frames"] = last_sample_.primary_routed_frames;
            totals["helper_requested_frames"] = last_sample_.helper_requested_frames;
            totals["helper_fallback_frames"] = last_sample_.helper_fallback_frames;
            totals["helper_dispatched_frames"] = last_sample_.helper_dispatched_frames;
            totals["last_target_gpu_id"] = last_sample_.last_target_gpu_id;
            totals["last_route_mode"] = last_sample_.last_route_mode;
            totals["camera_dropped_frames"] = last_sample_.camera_dropped_frames;
            totals["camera_frame_id_gaps"] = last_sample_.camera_dropped_frames;
            totals["get_frame_errors"] = last_sample_.get_frame_errors;
            totals["last_get_frame_error_code"] = last_sample_.last_get_frame_error_code;
            nlohmann::json get_frame_errors_by_code = nlohmann::json::object();
            for (const auto& [code, count] : last_sample_.get_frame_errors_by_code) {
                get_frame_errors_by_code[std::to_string(code)] = count;
            }
            totals["get_frame_errors_by_code"] = get_frame_errors_by_code;
            totals["display_preview_max_fps"] = last_sample_.display_preview_max_fps;
            totals["display_preview_eligible_frames"] = last_sample_.display_preview_eligible_frames;
            totals["display_preview_selected_frames"] = last_sample_.display_preview_selected_frames;
            totals["display_preview_skipped_frames"] = last_sample_.display_preview_skipped_frames;
            totals["gpu_direct_frames"] = last_sample_.gpu_direct_frames;
            totals["gpu_ring_copy_frames"] = last_sample_.gpu_ring_copy_frames;
            totals["gpu_copy_frames"] = last_sample_.gpu_copy_frames;
        }
        summary["totals"] = totals;
        if (have_last_sample_) {
            summary["routing"] = {
                {"submitted_frames", last_sample_.submitted_frames},
                {"enqueue_rejected_frames", last_sample_.enqueue_rejected_frames},
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
                                          const Emergent::CEmergentFrame *received_frame,
                                          CameraState *camera_state,
                                          CameraParams *camera_params){
    NVTX_RANGE("PTP_Timestamp_Check");
    const uint64_t frame_index = static_cast<uint64_t>(camera_state->frame_count) + 1;
    const int register_read_decimate = ptp_register_read_decimate();
    const bool read_ptp_register =
        register_read_decimate <= 1 ||
        frame_index <= 5 ||
        (frame_index % static_cast<uint64_t>(register_read_decimate)) == 0;

    ptp_state->frame_ts = received_frame ? received_frame->timestamp : 0;
    if (camera_state->frame_count != 0) {
        ptp_state->frame_ts_delta = ptp_state->frame_ts - ptp_state->frame_ts_prev;
        ptp_state->frame_ts_delta_sum += ptp_state->frame_ts_delta;
    }
    ptp_state->frame_ts_prev = ptp_state->frame_ts;

    ptp_state->ptp_register_read_this_frame = read_ptp_register;
    if (read_ptp_register) {
        EVT_CameraExecuteCommand(&ecam->camera, "GevTimestampControlLatch");
        EVT_CameraGetUInt32Param(&ecam->camera, "GevTimestampValueHigh", &ptp_state->ptp_time_high);
        EVT_CameraGetUInt32Param(&ecam->camera, "GevTimestampValueLow", &ptp_state->ptp_time_low);
        const unsigned long long latched_ptp_time =
            (((unsigned long long)(ptp_state->ptp_time_high)) << 32) |
            ((unsigned long long)(ptp_state->ptp_time_low));
        if (ptp_state->ptp_register_read_count != 0 &&
            latched_ptp_time >= ptp_state->ptp_time_prev) {
            ptp_state->ptp_time_delta = latched_ptp_time - ptp_state->ptp_time_prev;
            ptp_state->ptp_time_delta_sum += ptp_state->ptp_time_delta;
            ptp_state->ptp_time_delta_samples++;
        } else {
            ptp_state->ptp_time_delta = 0;
        }
        ptp_state->ptp_time = latched_ptp_time;
        ptp_state->ptp_time_prev = ptp_state->ptp_time;
        ptp_state->ptp_register_read_count++;
        ptp_state->last_ptp_register_read_frame = frame_index;
    }

    if (frame_index <= 5) {
        const int64_t latch_minus_frame_ns =
            static_cast<int64_t>(ptp_state->ptp_time) - static_cast<int64_t>(ptp_state->frame_ts);
        std::cout << "[PTP_FIRST_FRAMES]"
                  << " cam=" << (camera_params ? camera_params->camera_serial : std::string("<unknown>"))
                  << " frame=" << frame_index
                  << " frame_ts_ns=" << ptp_state->frame_ts
                  << " latched_ptp_ns=" << ptp_state->ptp_time
                  << " latch_minus_frame_ns=" << latch_minus_frame_ns
                  << " frame_delta_ns=" << ptp_state->frame_ts_delta
                  << " latch_delta_ns=" << ptp_state->ptp_time_delta
                  << " ptp_register_read=" << (read_ptp_register ? 1 : 0)
                  << std::endl;
    }
}

static inline Emergent::CEmergentFrame* resolve_queued_camera_frame(
    CameraEmergent* ecam,
    const Emergent::CEmergentFrame* received_frame)
{
    if (!ecam || !received_frame || !received_frame->imagePtr ||
        !ecam->evt_frame || ecam->evt_frame_count <= 0) {
        return nullptr;
    }

    for (int i = 0; i < ecam->evt_frame_count; ++i) {
        if (ecam->evt_frame[i].imagePtr == received_frame->imagePtr) {
            return &ecam->evt_frame[i];
        }
    }
    return nullptr;
}

void acquire_frames(
    CameraEmergent *ecam,
    CameraParams *camera_params,
    CameraEachSelect* camera_select,
    CameraControl* camera_control,
    PTPParams* ptp_params,
    INDIGOSignalBuilder* /*indigo_signal_builder*/,
    COpenGLDisplay* openGLDisplay,
    RecordingIngress* recording_ingress,
    YoloWorker* yolo_worker,
    ImageWriterWorker* image_writer,
    CameraResources* resources,
    FrameIPCManager* frame_ipc_manager,
    yolo_event_log::SyntheticYoloEventEmitter* synthetic_yolo_event_emitter,
    SpatialSnapshotWorker* spatial_snapshot_worker
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
    uint64_t last_receive_host_ns = 0;
    uint64_t last_camera_timestamp_ns = 0;
    uint64_t acquisition_resource_starvations = 0;
    uint64_t gpu_direct_frames = 0;
    uint64_t gpu_ring_copy_frames = 0;
    uint64_t gpu_copy_frames = 0;
    uint64_t gpu_direct_frames_total = 0;
    uint64_t gpu_ring_copy_frames_total = 0;
    uint64_t gpu_copy_frames_total = 0;
    const int display_preview_max_fps = resolve_display_preview_max_fps(camera_select);
    const unsigned int source_frame_rate =
        (camera_params && camera_params->frame_rate > 0)
            ? camera_params->frame_rate
            : static_cast<unsigned int>(display_preview_max_fps > 0 ? display_preview_max_fps : 1);
    DisplayPreviewCadence display_preview_cadence(display_preview_max_fps, source_frame_rate);
    uint64_t display_preview_eligible_frames = 0;
    uint64_t display_preview_selected_frames = 0;
    uint64_t display_preview_skipped_frames = 0;
    uint64_t gpu_direct_attr_errors = 0;
    uint64_t gpu_direct_non_device = 0;
    uint64_t gpu_direct_wrong_device = 0;
    std::map<int, uint64_t> get_frame_errors_by_code;
    auto last_gpu_direct_log_time = std::chrono::steady_clock::now();
    int free_entries_available = resources ? resources->acquire_work_entries_max : 0;
    int free_events_available = resources ? static_cast<int>(resources->event_pool.size()) : 0;
    int yolo_events_available =
        (resources && resources->yolo_events_queue) ? static_cast<int>(resources->yolo_event_pool.size()) : 0;
    int free_entries_low = free_entries_available;
    int free_events_low = free_events_available;
    int yolo_events_low = yolo_events_available;
    uint64_t worker_enqueue_rejections = 0;
    struct PendingRequeue {
        Emergent::CEmergentCamera* camera;
        Emergent::CEmergentFrame* frame;
        cudaEvent_t* copy_ready_event;
        cudaEvent_t* consumer_done_event;
        std::atomic<bool>* consumer_done_event_recorded;
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
    AcquisitionCadenceProbeRecorder acquisition_cadence_probe_recorder(camera_params);
    std::deque<PtpReceiveHistoryEntry> ptp_receive_history;
    std::deque<RecordingSubmitHistoryEntry> recording_submit_history;
    bool ptp_stale_history_dumped = false;
    if (yolo_decimate > 1) {
        std::cout << "[YOLO] Cam " << camera_params->camera_serial
                  << " decimate=1/" << yolo_decimate << std::endl;
    }
    const bool detect_priority_recording =
        env_flag_enabled("ORANGE_RECORDING_DETECT_PRIORITY", false);
    if (detect_priority_recording) {
        std::cout << "[RECORDING_DETECT_PRIORITY] Cam "
                  << camera_params->camera_serial
                  << " enqueues YOLO before full-frame recording" << std::endl;
    }
    const bool yolo_detach_input =
        env_flag_enabled("ORANGE_YOLO_DETACH_INPUT", true);
    if (yolo_detach_input) {
        std::cout << "[YOLO_DETACH_INPUT] Cam "
                  << camera_params->camera_serial
                  << " will release source after YOLO input is ready" << std::endl;
    }

    auto reset_ptp_summary_stats = [&]() {
        ptp_offset_stats.reset();
        latch_minus_frame_stats.reset();
        frame_delta_stats.reset();
        latch_delta_stats.reset();
    };

    auto build_ptp_camera_summary_json = [&](bool finalized) {
        nlohmann::json summary = nlohmann::json::object();
        const uint64_t acquisition_frames = camera_state.frame_count;
        const uint64_t recording_frames_assigned = last_recording_frame_count;
        const RecordingIngressStats recording_stats =
            recording_ingress ? recording_ingress->GetStats() : RecordingIngressStats{};
        summary["camera_serial"] = camera_params->camera_serial;
        summary["camera_id"] = camera_params->camera_id;
        summary["gpu_id"] = camera_params->gpu_id;
        summary["sync_camera_enabled"] = camera_control->sync_camera;
        summary["finalized"] = finalized;
        summary["updated_at_utc"] = get_current_utc_timestamp();
        summary["frame_count"] = acquisition_frames;
        summary["frame_count_semantics"] = "legacy_alias_for_acquisition_frames_received_total";
        summary["acquisition_frames_received_total"] = acquisition_frames;
        summary["sync_observed_frames_total"] =
            camera_control->sync_camera ? acquisition_frames : 0ULL;
        summary["sync_observed_frame_count_source"] = "successful_EVT_CameraGetFrame";
        summary["recording_frames_assigned_total"] = recording_frames_assigned;
        summary["last_recording_frame_id"] = recording_frames_assigned;
        summary["worker_enqueue_rejections"] = worker_enqueue_rejections;
        summary["worker_entry_release_underflows"] =
            worker_entry_release_underflow_count().load(std::memory_order_relaxed);
        summary["worker_entry_double_releases"] =
            worker_entry_release_double_release_count().load(std::memory_order_relaxed);
        summary["worker_entry_retain_after_release_count"] =
            worker_entry_retain_after_release_count().load(std::memory_order_relaxed);
        summary["worker_entry_release_underflows_global"] =
            worker_entry_release_underflow_count().load(std::memory_order_relaxed);
        summary["worker_entry_double_releases_global"] =
            worker_entry_release_double_release_count().load(std::memory_order_relaxed);
        summary["worker_entry_retain_after_release_global"] =
            worker_entry_retain_after_release_count().load(std::memory_order_relaxed);
        const WorkerEntryRefCountDiagnosticCounts camera_ref_count_counts =
            worker_entry_ref_count_diagnostic_counts_for_camera(camera_params->camera_serial);
        summary["worker_entry_release_underflows_camera"] =
            camera_ref_count_counts.release_underflows;
        summary["worker_entry_double_releases_camera"] =
            camera_ref_count_counts.double_releases;
        summary["worker_entry_retain_after_release_camera"] =
            camera_ref_count_counts.retain_after_release;
        summary["recording_ingress_submitted_frames"] = recording_stats.submitted_frames;
        summary["recording_ingress_enqueue_rejected_frames"] =
            recording_stats.enqueue_rejected_frames;
        summary["encoded_frames_total"] = nullptr;
        summary["encoded_frame_count_source"] = "not_available_in_ptp_sync_summary";
        summary["encoded_frame_count_authoritative_artifacts"] = {
            {"metadata_csv", "Cam" + camera_params->camera_serial + "_meta.csv"},
            {"keyframe_json", "Cam" + camera_params->camera_serial + "_keyframe.json"},
            {"recording_session_json", "recording_session.json"}
        };
        summary["frames_received"] = camera_state.frames_recd;
        summary["dropped_frames"] = camera_state.dropped_frames;
        summary["camera_frame_id_gaps"] = camera_state.dropped_frames;
        summary["get_frame_errors"] = camera_state.get_frame_errors;
        summary["last_get_frame_error_code"] = camera_state.last_get_frame_error_code;
        nlohmann::json get_frame_errors_by_code_json = nlohmann::json::object();
        for (const auto& [code, count] : get_frame_errors_by_code) {
            get_frame_errors_by_code_json[std::to_string(code)] = count;
        }
        summary["get_frame_errors_by_code"] = get_frame_errors_by_code_json;
        summary["last_frame_timestamp_ns"] = ptp_state.frame_ts;
        summary["last_latched_ptp_time_ns"] = ptp_state.ptp_time;
        summary["ptp_register_read_decimate"] = ptp_register_read_decimate();
        summary["ptp_register_reads"] = ptp_state.ptp_register_read_count;
        summary["last_ptp_register_read_frame"] = ptp_state.last_ptp_register_read_frame;
        summary["ptp_offset_ns"] = ptp_offset_stats.to_json();
        summary["latch_minus_frame_ns"] = latch_minus_frame_stats.to_json();
        summary["frame_delta_ns"] = frame_delta_stats.to_json();
        summary["latch_delta_ns"] = latch_delta_stats.to_json();
        const uint64_t delta_samples = (camera_state.frame_count > 1) ? (camera_state.frame_count - 1) : 0;
        summary["delta_samples"] = delta_samples;
        summary["latch_delta_samples"] = ptp_state.ptp_time_delta_samples;
        if (delta_samples > 0) {
            summary["avg_frame_delta_ns_running"] = ptp_state.frame_ts_delta_sum / delta_samples;
        }
        if (ptp_state.ptp_time_delta_samples > 0) {
            summary["avg_latch_delta_ns_running"] =
                ptp_state.ptp_time_delta_sum / ptp_state.ptp_time_delta_samples;
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
        sample.display_preview_max_fps = display_preview_max_fps;
        sample.display_preview_eligible_frames = display_preview_eligible_frames;
        sample.display_preview_selected_frames = display_preview_selected_frames;
        sample.display_preview_skipped_frames = display_preview_skipped_frames;
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
        sample.worker_enqueue_rejections = worker_enqueue_rejections;
        sample.worker_entry_release_underflows =
            worker_entry_release_underflow_count().load(std::memory_order_relaxed);
        sample.worker_entry_double_releases =
            worker_entry_release_double_release_count().load(std::memory_order_relaxed);
        sample.worker_entry_retain_after_release_count =
            worker_entry_retain_after_release_count().load(std::memory_order_relaxed);
        sample.worker_entry_release_underflows_global =
            sample.worker_entry_release_underflows;
        sample.worker_entry_double_releases_global =
            sample.worker_entry_double_releases;
        sample.worker_entry_retain_after_release_global =
            sample.worker_entry_retain_after_release_count;
        const WorkerEntryRefCountDiagnosticCounts camera_ref_count_counts =
            worker_entry_ref_count_diagnostic_counts_for_camera(camera_params->camera_serial);
        sample.worker_entry_release_underflows_camera =
            camera_ref_count_counts.release_underflows;
        sample.worker_entry_double_releases_camera =
            camera_ref_count_counts.double_releases;
        sample.worker_entry_retain_after_release_camera =
            camera_ref_count_counts.retain_after_release;
        sample.preprocess_buffers_available = recording_stats.preprocess_buffers_available;
        sample.preprocess_events_available = recording_stats.preprocess_events_available;
        sample.preprocess_resource_waits = recording_stats.preprocess_resource_waits;
        sample.preprocess_frames_dropped = recording_stats.preprocess_frames_dropped;
        sample.detect_priority_gated_frames = recording_stats.detect_priority_gated_frames;
        sample.detect_priority_waited_frames = recording_stats.detect_priority_waited_frames;
        sample.detect_priority_wait_timeouts = recording_stats.detect_priority_wait_timeouts;
        sample.detect_priority_wait_total_ns = recording_stats.detect_priority_wait_total_ns;
        sample.detect_priority_wait_max_ns = recording_stats.detect_priority_wait_max_ns;
        sample.encode_failures = recording_stats.encode_failures;
        sample.encode_slow_frames = recording_stats.encode_slow_frames;
        sample.external_ipc_frames_acked = recording_stats.external_ipc_frames_acked;
        sample.external_ipc_failures = recording_stats.external_ipc_failures;
        sample.external_ipc_ack_timeouts = recording_stats.external_ipc_ack_timeouts;
        sample.submitted_frames = recording_stats.submitted_frames;
        sample.enqueue_rejected_frames = recording_stats.enqueue_rejected_frames;
        sample.primary_routed_frames = recording_stats.primary_routed_frames;
        sample.helper_requested_frames = recording_stats.helper_requested_frames;
        sample.helper_fallback_frames = recording_stats.helper_fallback_frames;
        sample.helper_dispatched_frames = recording_stats.helper_dispatched_frames;
        sample.last_target_gpu_id = recording_stats.last_target_gpu_id;
        sample.last_route_mode = recording_stats.last_route_mode;
        sample.camera_dropped_frames = camera_state.dropped_frames;
        sample.get_frame_errors = camera_state.get_frame_errors;
        sample.last_get_frame_error_code = camera_state.last_get_frame_error_code;
        sample.get_frame_errors_by_code = get_frame_errors_by_code;
        sample.gpu_direct_frames = gpu_direct_frames_total;
        sample.gpu_ring_copy_frames = gpu_ring_copy_frames_total;
        sample.gpu_copy_frames = gpu_copy_frames_total;
        return sample;
    };

    auto finalize_pipeline_perf_recorder_if_drained = [&]() -> bool {
        if (pipeline_perf_recorder.current_folder().empty()) {
            return true;
        }
        if (recording_ingress && !recording_ingress->IsDrained()) {
            return false;
        }

        const PipelinePerfSample final_pipeline_sample = build_pipeline_perf_sample();
        pipeline_perf_recorder.Record(final_pipeline_sample);
        pipeline_perf_recorder.Close();
        return true;
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
            const auto query_requeue_event =
                [](cudaEvent_t* event,
                   const char* label,
                   const std::atomic<bool>* event_recorded = nullptr) {
                if (!event) {
                    return cudaSuccess;
                }
                if (event_recorded &&
                    !event_recorded->load(std::memory_order_acquire)) {
                    return cudaErrorNotReady;
                }
                cudaError_t status = cudaEventQuery(*event);
                if (status == cudaSuccess || status == cudaErrorNotReady) {
                    return status;
                }
                std::cerr << "[GPU_DIRECT] Requeue " << label << " event query failed: "
                          << cudaGetErrorString(status) << std::endl;
                return status;
            };

            const cudaError_t copy_status =
                query_requeue_event(it->copy_ready_event, "copy-ready");
            const cudaError_t consumer_status =
                query_requeue_event(
                    it->consumer_done_event,
                    "consumer-done",
                    it->consumer_done_event_recorded);
            if (copy_status == cudaSuccess && consumer_status == cudaSuccess) {
                EVT_CameraQueueFrame(it->camera, it->frame);
                it = pending_requeues.erase(it);
                continue;
            }
            if (copy_status == cudaErrorNotReady || consumer_status == cudaErrorNotReady) {
                ++it;
                continue;
            }
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

        Emergent::CEmergentFrame* received_frame = &current_entry->camera_frame_recv;
        const uint64_t get_frame_call_host_ns = steady_clock_now_ns();
        camera_state.camera_return = EVT_CameraGetFrame(&ecam->camera, received_frame, 1000);

        if (camera_state.camera_return == EVT_SUCCESS) {
            Emergent::CEmergentFrame* frame_to_requeue =
                resolve_queued_camera_frame(ecam, received_frame);
            if (!frame_to_requeue) {
                frame_to_requeue = received_frame;
            }
            const uint64_t receive_host_ns = steady_clock_now_ns();
            const uint64_t get_frame_wait_ns =
                receive_host_ns > get_frame_call_host_ns
                    ? receive_host_ns - get_frame_call_host_ns
                    : 0;
            uint64_t ptp_check_done_host_ns = 0;
            if (camera_control->sync_camera) {
                PTP_timestamp_checking(&ptp_state, ecam, received_frame, &camera_state, camera_params);
                ptp_check_done_host_ns = steady_clock_now_ns();
            }

            struct timespec ts_rt1;
            clock_gettime(CLOCK_REALTIME, &ts_rt1);
            uint64_t real_time = (ts_rt1.tv_sec * 1000000000LL) + ts_rt1.tv_nsec;
            camera_state.dropped_frames += count_camera_frame_id_gaps(
                camera_state.id_prev,
                received_frame->frame_id);
            camera_state.id_prev = next_camera_frame_id_prev(received_frame->frame_id);
            camera_state.frames_recd++;
            camera_state.frame_count++;
            current_entry->frame_id = camera_state.frame_count; // Assign absolute frame ID
            const uint64_t receive_delta_ns =
                (last_receive_host_ns > 0 && receive_host_ns >= last_receive_host_ns)
                    ? receive_host_ns - last_receive_host_ns
                    : 0;
            last_receive_host_ns = receive_host_ns;
            const uint64_t camera_timestamp_ns = received_frame->timestamp;
            const uint64_t camera_timestamp_delta_ns =
                (last_camera_timestamp_ns > 0 && camera_timestamp_ns >= last_camera_timestamp_ns)
                    ? camera_timestamp_ns - last_camera_timestamp_ns
                    : 0;
            last_camera_timestamp_ns = camera_timestamp_ns;

        //     std::cout << "[DEBUG] Camera " << camera_params->camera_serial 
        //   << " incremented to frame_count=" << camera_state.frame_count 
        //   << " (entry->frame_id=" << current_entry->frame_id << ")" << std::endl;

            if (current_entry->d_image_pool) {
                current_entry->d_image = current_entry->d_image_pool;
            }
            current_entry->d_analytics_image = nullptr;
            current_entry->analytics_owned_frame_valid = false;
            current_entry->gpu_direct_mode = false;
            current_entry->owns_memory = true;
            current_entry->camera_buffer_ptr = nullptr;
            current_entry->camera_instance = nullptr;
            current_entry->camera_frame_struct = nullptr;
            current_entry->ingress_event_record_host_ns = 0;
            current_entry->yolo_ptp_done_host_ns = ptp_check_done_host_ns;
            current_entry->yolo_resource_ready_host_ns = 0;
            current_entry->yolo_pointer_attrs_done_host_ns = 0;
            current_entry->yolo_dispatch_ready_host_ns = 0;
            current_entry->yolo_before_recording_submit_host_ns = 0;
            current_entry->yolo_after_recording_submit_host_ns = 0;
            current_entry->yolo_enqueue_host_ns = 0;
            current_entry->yolo_enqueued_host_ns = 0;
            current_entry->yolo_dequeue_host_ns = 0;
            current_entry->yolo_queue_depth_at_enqueue = -1;
            current_entry->yolo_queue_depth_after_enqueue = -1;
            current_entry->yolo_queue_depth_after_dequeue = -1;
            current_entry->yolo_dispatched = false;
            current_entry->yolo_input_detach_requested = false;
            current_entry->yolo_input_ready_host_ns = 0;
            current_entry->yolo_input_ready_event_recorded.store(false);
            current_entry->yolo_completion_event_recorded.store(false);

            bool will_display = false;
            if (camera_select->stream_on && openGLDisplay) {
                display_preview_eligible_frames++;
                will_display = display_preview_cadence.ShouldDisplayNextFrame();
                if (will_display) {
                    display_preview_selected_frames++;
                } else {
                    display_preview_skipped_frames++;
                }
            }
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
                    EVT_CameraQueueFrame(&ecam->camera, frame_to_requeue);
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
                    EVT_CameraQueueFrame(&ecam->camera, frame_to_requeue);
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
                current_entry->yolo_resource_ready_host_ns = steady_clock_now_ns();
            }

            // If recording is active, increment and assign the recording-specific frame ID
            if (camera_control->record_video) {
                current_entry->recording_frame_id = ++local_recording_frame_count;
                last_recording_frame_count = current_entry->recording_frame_id;
                camera_control->latest_recording_frame_id.store(
                    current_entry->recording_frame_id,
                    std::memory_order_relaxed);
            } else {
                current_entry->recording_frame_id = 0;
                if (!camera_control->preserve_recording_session_state) {
                    local_recording_frame_count = 0;
                    camera_control->latest_recording_frame_id.store(
                        0,
                        std::memory_order_relaxed);
                }
            }

            const std::string live_recording_folder = current_recording_folder(camera_control);
            if (!live_recording_folder.empty()) {
                if (live_recording_folder != pipeline_perf_recorder.current_folder()) {
                    if (!pipeline_perf_recorder.current_folder().empty()) {
                        (void)finalize_pipeline_perf_recorder_if_drained();
                    }
                    if (pipeline_perf_recorder.current_folder().empty()) {
                        pipeline_perf_recorder.Rotate(live_recording_folder);
                    }
                }
            } else {
                (void)finalize_pipeline_perf_recorder_if_drained();
            }
            acquisition_cadence_probe_recorder.Rotate(live_recording_folder);
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
            const bool will_snapshot =
                spatial_snapshot_worker &&
                spatial_snapshot_worker->HasPendingRequest();
            if (will_snapshot) dispatch_count++;

            cudaPointerAttributes attrs{};
            cudaError_t attr_status = cudaPointerGetAttributes(&attrs, received_frame->imagePtr);
            if (attr_status != cudaSuccess) {
                gpu_direct_attr_errors++;
                cudaGetLastError(); // Clear error so we can continue.
            }
            bool use_direct_pointer = (attr_status == cudaSuccess &&
                                       attrs.type == cudaMemoryTypeDevice &&
                                       attrs.device == camera_params->gpu_id);
            const bool force_ring_copy =
                use_direct_pointer &&
                camera_params &&
                camera_params->acquisition_buffer_mode == "force_ring_copy";
            const bool analytics_owned_frame_enabled =
                env_flag_enabled("ORANGE_ANALYTICS_EARLY_OWNED_FRAME", true) &&
                will_yolo &&
                use_direct_pointer &&
                !force_ring_copy;
            const bool use_analytics_hybrid = analytics_owned_frame_enabled;
            const bool recording_requires_owned_source =
                will_record &&
                recording_ingress &&
                recording_ingress->requires_owned_cuda_source();
            bool use_ring_copy =
                use_direct_pointer &&
                (dispatch_count > 1 || recording_requires_owned_source) &&
                !use_analytics_hybrid;
            if (force_ring_copy) {
                use_ring_copy = true;
            }

            if (attr_status == cudaSuccess) {
                if (attrs.type != cudaMemoryTypeDevice) {
                    gpu_direct_non_device++;
                } else if (attrs.device != camera_params->gpu_id) {
                    gpu_direct_wrong_device++;
                }
            }
            if (will_yolo) {
                current_entry->yolo_pointer_attrs_done_host_ns = steady_clock_now_ns();
            }

            current_entry->event_ptr = current_event;
            current_entry->yolo_completion_event = yolo_event;

            if (use_analytics_hybrid) {
                gpu_direct_frames++;
                gpu_direct_frames_total++;
                current_entry->d_image = static_cast<unsigned char*>(received_frame->imagePtr);
                current_entry->d_analytics_image = current_entry->d_image_pool;
                current_entry->analytics_owned_frame_valid = true;
                current_entry->gpu_direct_mode = false;
                current_entry->owns_memory = true;
                current_entry->camera_buffer_ptr = nullptr;
                current_entry->camera_instance = nullptr;
                current_entry->camera_frame_struct = nullptr;

                // Let YOLO consume the ingress lease immediately while the owned
                // analytics copy is produced for delayed consumers.
                ck(cudaEventRecord(*current_entry->event_ptr, stream));
                current_entry->ingress_event_record_host_ns = steady_clock_now_ns();
                ck(cudaMemcpyAsync(
                    current_entry->d_analytics_image,
                    received_frame->imagePtr,
                    received_frame->bufferSize,
                    cudaMemcpyDeviceToDevice,
                    stream));
                ck(cudaEventRecord(current_entry->analytics_ready_event, stream));
            } else if (use_direct_pointer && !use_ring_copy) {
                gpu_direct_frames++;
                gpu_direct_frames_total++;
                current_entry->d_image = static_cast<unsigned char*>(received_frame->imagePtr);
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
                ck(cudaMemcpyAsync(current_entry->d_image, received_frame->imagePtr, received_frame->bufferSize, cudaMemcpyDeviceToDevice, stream));
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
                    EVT_CameraQueueFrame(&ecam->camera, frame_to_requeue);
                }
            }

            if (!use_analytics_hybrid) {
                ck(cudaEventRecord(*current_entry->event_ptr, stream));
                current_entry->ingress_event_record_host_ns = steady_clock_now_ns();
            }
            current_entry->yolo_input_detach_requested = yolo_detach_input && will_yolo;
            if (use_ring_copy) {
                pending_requeues.push_back(
                    {&ecam->camera, frame_to_requeue, current_entry->event_ptr, nullptr, nullptr});
            } else if (use_analytics_hybrid) {
                cudaEvent_t* yolo_source_done_event = current_entry->yolo_completion_event;
                std::atomic<bool>* yolo_source_done_event_recorded =
                    &current_entry->yolo_completion_event_recorded;
                if (current_entry->yolo_input_detach_requested &&
                    current_entry->yolo_input_ready_event) {
                    yolo_source_done_event = &current_entry->yolo_input_ready_event;
                    yolo_source_done_event_recorded =
                        &current_entry->yolo_input_ready_event_recorded;
                }
                pending_requeues.push_back(
                    {&ecam->camera,
                     frame_to_requeue,
                     &current_entry->analytics_ready_event,
                     yolo_source_done_event,
                     yolo_source_done_event_recorded});
            }

            current_entry->width = received_frame->size_x;
            current_entry->height = received_frame->size_y;
            current_entry->pixelFormat = received_frame->pixel_type;
            current_entry->timestamp = received_frame->timestamp;
            current_entry->timestamp_sys = real_time;
            current_entry->frame_id = camera_state.frame_count;
            current_entry->camera_frame_id = received_frame->frame_id;
            current_entry->recording_folder = live_recording_folder;
            current_entry->source_buffer_bytes = received_frame->bufferSize;
            current_entry->acquisition_receive_host_ns = receive_host_ns;
            current_entry->yolo_detect_done_host_ns = 0;
            current_entry->recording_submit_host_ns = 0;
            current_entry->recording_target_gpu_id = -1;
            current_entry->recording_helper_requested = false;
            current_entry->recording_route_helper = false;
            current_entry->helper_enqueue_host_ns = 0;
            current_entry->helper_enqueue_queue_depth = -1;
            current_entry->helper_enqueue_available_buffers = -1;
            current_entry->helper_enqueue_available_events = -1;
            current_entry->has_detections = will_yolo;
            current_entry->yolo_dispatched = will_yolo;
            current_entry->yolo_input_detach_requested = yolo_detach_input && will_yolo;
            current_entry->detections_ready.store(false);
            current_entry->ipc_frame_id = 0;

            if (camera_control->sync_camera) {
                const int64_t latch_minus_frame_ns =
                    static_cast<int64_t>(ptp_state.ptp_time) -
                    static_cast<int64_t>(ptp_state.frame_ts);
                PtpReceiveHistoryEntry receive_entry;
                receive_entry.local_frame_id = camera_state.frame_count;
                receive_entry.camera_frame_id = received_frame->frame_id;
                receive_entry.frame_timestamp_ns = current_entry->timestamp;
                receive_entry.timestamp_sys_ns = current_entry->timestamp_sys;
                receive_entry.latch_minus_frame_ns = latch_minus_frame_ns;
                receive_entry.frame_delta_ns = ptp_state.frame_ts_delta;
                receive_entry.latch_delta_ns = ptp_state.ptp_time_delta;
                receive_entry.camera_dropped_frames = camera_state.dropped_frames;
                receive_entry.get_frame_errors = camera_state.get_frame_errors;
                receive_entry.last_get_frame_error_code = camera_state.last_get_frame_error_code;
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
            current_entry->frame_ipc_manager =
                camera_select->send_yolo_via_frame_ipc ? ipc_manager : nullptr;

            if (synthetic_yolo_event_emitter) {
                yolo_event_log::SyntheticYoloFrameInput synthetic_frame;
                synthetic_frame.recording_folder = current_entry->recording_folder;
                synthetic_frame.local_frame_id = current_entry->frame_id;
                synthetic_frame.camera_frame_id = current_entry->camera_frame_id;
                synthetic_frame.recording_frame_id = current_entry->recording_frame_id;
                synthetic_frame.ipc_frame_id = current_entry->ipc_frame_id;
                synthetic_frame.record_active = camera_control->record_video;
                synthetic_frame.camera_timestamp = current_entry->timestamp;
                synthetic_frame.timestamp_sys_ns = current_entry->timestamp_sys;
                synthetic_frame.width = current_entry->width;
                synthetic_frame.height = current_entry->height;
                synthetic_yolo_event_emitter->EmitFrame(synthetic_frame);
            }

            if (camera_select->frame_save_state == State_Write_New_Frame && image_writer) {
                ImageWriter_Entry* save_job = new ImageWriter_Entry();
                save_job->event_ptr = current_event;
                if (!image_writer->PutObjectToQueueIn(save_job)) {
                    delete save_job;
                    std::cerr << "[ACQ] Image writer enqueue rejected"
                              << " cam=" << camera_params->camera_serial
                              << " frame=" << current_entry->frame_id
                              << std::endl;
                }
            }
            
            // Create a dispatch counter to track how many workers will process this frame
            // If the camera is set to stream, record video, or run YOLO detection,
            // we increment the dispatch count for each active worker.
            // If no workers are active, we return the frame to the free queue.
            // This allows us to efficiently manage resources and avoid unnecessary processing.
            if (dispatch_count > 0) {
                current_entry->ref_count.store(1);
                WorkerEntryRefGuard acquisition_base_ref_guard(
                    resources->recycle_queue,
                    current_entry,
                    WorkerEntryReleaseContext{
                        camera_params->camera_serial.c_str(),
                        "acquisition_base_ref"},
                    true);
                if (will_yolo) {
                    current_entry->yolo_dispatch_ready_host_ns = steady_clock_now_ns();
                }

                if (use_direct_pointer && !use_ring_copy && !use_analytics_hybrid) {
                    current_entry->camera_buffer_ptr = received_frame->imagePtr;
                    current_entry->camera_instance = &ecam->camera;
                    current_entry->camera_frame_struct = frame_to_requeue;
                }

                auto log_fanout_enqueue_rejected = [&](const char* worker_name) {
                    worker_enqueue_rejections++;
                    std::cerr << "[ACQ] Worker enqueue rejected"
                              << " cam=" << camera_params->camera_serial
                              << " worker=" << worker_name
                              << " frame=" << current_entry->frame_id
                              << " camera_frame=" << current_entry->camera_frame_id
                              << " recording_frame=" << current_entry->recording_frame_id
                              << " rejections=" << worker_enqueue_rejections
                              << std::endl;
                };

                auto mark_yolo_enqueue_failed = [&]() {
                    current_entry->yolo_dispatched = false;
                    current_entry->has_detections = false;
                    current_entry->detections_ready.store(true, std::memory_order_release);
                    if (current_entry->yolo_input_ready_event) {
                        cudaEventRecord(current_entry->yolo_input_ready_event, stream);
                        current_entry->yolo_input_ready_event_recorded.store(
                            true,
                            std::memory_order_release);
                    }
                    if (current_entry->yolo_completion_event) {
                        cudaEventRecord(*current_entry->yolo_completion_event, stream);
                        current_entry->yolo_completion_event_recorded.store(
                            true,
                            std::memory_order_release);
                    }
                };

                auto enqueue_yolo = [&]() -> bool {
                    current_entry->yolo_enqueue_host_ns = steady_clock_now_ns();
                    current_entry->yolo_queue_depth_at_enqueue =
                        yolo_worker->GetCountQueueInSize();
                    bool enqueue_rejected = false;
                    if (!retain_and_enqueue_worker_entry(
                            yolo_worker,
                            resources->recycle_queue,
                            current_entry,
                            WorkerEntryReleaseContext{
                                camera_params->camera_serial.c_str(),
                                "yolo"},
                            &enqueue_rejected)) {
                        mark_yolo_enqueue_failed();
                        if (enqueue_rejected) {
                            log_fanout_enqueue_rejected("yolo");
                        }
                        return false;
                    }
                    return true;
                };
                const bool dispatch_yolo_before_recording =
                    detect_priority_recording && will_record && will_yolo;

                if (will_display) {
                    bool enqueue_rejected = false;
                    if (!retain_and_enqueue_worker_entry(
                            openGLDisplay,
                            resources->recycle_queue,
                            current_entry,
                            WorkerEntryReleaseContext{
                                camera_params->camera_serial.c_str(),
                                "display"},
                            &enqueue_rejected) &&
                        enqueue_rejected) {
                        log_fanout_enqueue_rejected("display");
                    }
                }
                if (dispatch_yolo_before_recording) {
                    enqueue_yolo();
                }
                if (will_snapshot && spatial_snapshot_worker->TryClaimNextFrame()) {
                    bool enqueue_rejected = false;
                    if (!retain_and_enqueue_worker_entry(
                            spatial_snapshot_worker,
                            resources->recycle_queue,
                            current_entry,
                            WorkerEntryReleaseContext{
                                camera_params->camera_serial.c_str(),
                                "spatial_snapshot"},
                            &enqueue_rejected)) {
                        spatial_snapshot_worker->CompleteClaimedRequestWithError(
                            enqueue_rejected
                                ? "Full-resolution stream snapshot enqueue was rejected during shutdown."
                                : "Full-resolution stream snapshot could not retain the acquisition frame.");
                        if (enqueue_rejected) {
                            log_fanout_enqueue_rejected("spatial_snapshot");
                        }
                    }
                }
                if (will_record) {
                    if (will_yolo) {
                        current_entry->yolo_before_recording_submit_host_ns = steady_clock_now_ns();
                    }
                    WorkerEntryLease recording_lease = TryRetainWorkerEntryLease(
                        resources->recycle_queue,
                        current_entry,
                        WorkerEntryReleaseContext{
                            camera_params->camera_serial.c_str(),
                            "recording"});
                    const bool recording_retained = recording_lease.active();
                    bool recording_accepted = false;
                    if (recording_lease) {
                        recording_accepted = recording_ingress->SubmitFrame(current_entry);
                        if (recording_accepted) {
                            recording_lease.TransferToConsumer();
                        }
                    }
                    if (!recording_accepted) {
                        if (recording_retained) {
                            log_fanout_enqueue_rejected("recording");
                            if (camera_control->record_video &&
                                recording_ingress->fail_on_drop()) {
                                std::cerr << "[ACQ][ERROR] recording failed:"
                                          << " frame dropped while recording.fail_on_drop"
                                          << " is enabled; stopping recording cleanly"
                                          << " cam=" << camera_params->camera_serial
                                          << " frame=" << current_entry->frame_id
                                          << " recording_frame="
                                          << current_entry->recording_frame_id
                                          << std::endl;
                                camera_control->record_video = false;
                                camera_control->recording_draining = true;
                                camera_control->stop_record = true;
                            }
                        }
                    }
                    if (will_yolo) {
                        current_entry->yolo_after_recording_submit_host_ns = steady_clock_now_ns();
                    }
                    if (camera_control->sync_camera) {
                        RecordingSubmitHistoryEntry submit_entry;
                        submit_entry.local_frame_id = current_entry->frame_id;
                        submit_entry.recording_frame_id = current_entry->recording_frame_id;
                        submit_entry.camera_frame_id = received_frame->frame_id;
                        submit_entry.dispatch_count = dispatch_count;
                        submit_entry.use_direct_pointer = use_direct_pointer;
                        submit_entry.use_ring_copy = use_ring_copy;
                        submit_entry.ingress_stats = recording_ingress->GetStats();
                        append_recording_submit_history(
                            &recording_submit_history,
                            submit_entry);
                    }
                }
                const uint64_t probe_frame_id =
                    current_entry->recording_frame_id > 0
                        ? current_entry->recording_frame_id
                        : current_entry->frame_id;
                if (!live_recording_folder.empty() &&
                    probe_frame_id >= kAcquisitionCadenceProbeFrameMin &&
                    probe_frame_id <= kAcquisitionCadenceProbeFrameMax) {
                    AcquisitionCadenceProbeSample cadence_sample;
                    cadence_sample.timestamp_utc = get_current_utc_timestamp();
                    cadence_sample.local_frame_id = current_entry->frame_id;
                    cadence_sample.recording_frame_id = current_entry->recording_frame_id;
                    cadence_sample.camera_frame_id = received_frame->frame_id;
                    cadence_sample.camera_timestamp_ns = camera_timestamp_ns;
                    cadence_sample.camera_timestamp_delta_ns = camera_timestamp_delta_ns;
                    cadence_sample.receive_host_ns = receive_host_ns;
                    cadence_sample.receive_delta_ns = receive_delta_ns;
                    cadence_sample.get_frame_wait_ns = get_frame_wait_ns;
                    cadence_sample.ptp_active = camera_control->sync_camera;
                    cadence_sample.ptp_register_read =
                        camera_control->sync_camera &&
                        ptp_state.ptp_register_read_this_frame;
                    cadence_sample.ptp_register_read_decimate = ptp_register_read_decimate();
                    cadence_sample.ptp_register_read_age_frames =
                        (camera_control->sync_camera &&
                         current_entry->frame_id >= ptp_state.last_ptp_register_read_frame)
                            ? current_entry->frame_id - ptp_state.last_ptp_register_read_frame
                            : 0;
                    cadence_sample.latched_ptp_time_ns =
                        camera_control->sync_camera ? ptp_state.ptp_time : 0;
                    cadence_sample.latch_minus_frame_ns =
                        camera_control->sync_camera
                            ? static_cast<int64_t>(ptp_state.ptp_time) -
                                  static_cast<int64_t>(ptp_state.frame_ts)
                            : 0;
                    cadence_sample.latch_delta_ns =
                        camera_control->sync_camera ? ptp_state.ptp_time_delta : 0;
                    cadence_sample.record_active = camera_control->record_video;
                    cadence_sample.dispatch_count = dispatch_count;
                    cadence_sample.will_display = will_display;
                    cadence_sample.display_preview_max_fps = display_preview_max_fps;
                    cadence_sample.display_preview_eligible_frames = display_preview_eligible_frames;
                    cadence_sample.display_preview_selected_frames = display_preview_selected_frames;
                    cadence_sample.display_preview_skipped_frames = display_preview_skipped_frames;
                    cadence_sample.will_record = will_record;
                    cadence_sample.will_yolo = will_yolo;
                    cadence_sample.use_direct_pointer = use_direct_pointer;
                    cadence_sample.use_ring_copy = use_ring_copy;
                    cadence_sample.free_entries_available = free_entries_available;
                    cadence_sample.free_entries_low_watermark = free_entries_low;
                    cadence_sample.free_events_available = free_events_available;
                    cadence_sample.free_events_low_watermark = free_events_low;
                    cadence_sample.yolo_events_available = yolo_events_available;
                    cadence_sample.yolo_events_low_watermark = yolo_events_low;
                    cadence_sample.pending_requeues = pending_requeues.size();
                    cadence_sample.acquisition_resource_starvations =
                        acquisition_resource_starvations;
                    cadence_sample.worker_enqueue_rejections = worker_enqueue_rejections;
                    cadence_sample.camera_dropped_frames = camera_state.dropped_frames;
                    cadence_sample.get_frame_errors = camera_state.get_frame_errors;
                    cadence_sample.last_get_frame_error_code =
                        camera_state.last_get_frame_error_code;
                    cadence_sample.recording_submit_host_ns =
                        current_entry->recording_submit_host_ns;
                    cadence_sample.receive_to_submit_ns =
                        current_entry->recording_submit_host_ns > receive_host_ns
                            ? current_entry->recording_submit_host_ns - receive_host_ns
                            : 0;
                    cadence_sample.recording_target_gpu_id =
                        current_entry->recording_target_gpu_id;
                    cadence_sample.recording_helper_requested =
                        current_entry->recording_helper_requested;
                    cadence_sample.recording_route_helper =
                        current_entry->recording_route_helper;
                    cadence_sample.helper_enqueue_queue_depth =
                        current_entry->helper_enqueue_queue_depth;
                    cadence_sample.helper_enqueue_available_buffers =
                        current_entry->helper_enqueue_available_buffers;
                    cadence_sample.helper_enqueue_available_events =
                        current_entry->helper_enqueue_available_events;
                    cadence_sample.helper_enqueue_delay_ns =
                        current_entry->helper_enqueue_host_ns > receive_host_ns
                            ? current_entry->helper_enqueue_host_ns - receive_host_ns
                            : 0;
                    cadence_sample.ingress_stats =
                        recording_ingress ? recording_ingress->GetStats()
                                          : RecordingIngressStats{};
                    acquisition_cadence_probe_recorder.Record(cadence_sample);
                }
                if (!dispatch_yolo_before_recording && will_yolo) {
                    enqueue_yolo();
                }

            } else {
                // FRAME_IPC: Important - even if no workers are active, we still sent the frame IPC above
                // This ensures frame synchronization works even when just recording without display/YOLO
                if (use_direct_pointer && !use_ring_copy) {
                    EVT_CameraQueueFrame(&ecam->camera, frame_to_requeue);
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
                        (ptp_state.ptp_time_delta_samples > 0)
                            ? (ptp_state.ptp_time_delta_sum / ptp_state.ptp_time_delta_samples)
                            : 0;
                    const uint64_t ptp_register_read_age_frames =
                        camera_state.frame_count >= ptp_state.last_ptp_register_read_frame
                            ? camera_state.frame_count - ptp_state.last_ptp_register_read_frame
                            : 0;
                    int32_t current_ptp_offset = 0;
                    EVT_ERROR ptp_offset_ret = EVT_CameraGetInt32Param(&ecam->camera, "PtpOffset", &current_ptp_offset);
                    if (ptp_offset_ret == EVT_SUCCESS) {
                        ptp_offset_stats.add(current_ptp_offset);
                    }
                    if (ptp_state.ptp_register_read_this_frame) {
                        latch_minus_frame_stats.add(latch_minus_frame_ns);
                    }
                    frame_delta_stats.add(static_cast<int64_t>(ptp_state.frame_ts_delta));
                    if (ptp_state.ptp_register_read_this_frame &&
                        ptp_state.ptp_time_delta_samples > 0) {
                        latch_delta_stats.add(static_cast<int64_t>(ptp_state.ptp_time_delta));
                    }
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
                                  << " ptp_register_read=" << (ptp_state.ptp_register_read_this_frame ? 1 : 0)
                                  << " ptp_register_read_age_frames=" << ptp_register_read_age_frames
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
                                  << " ptp_register_read=" << (ptp_state.ptp_register_read_this_frame ? 1 : 0)
                                  << " ptp_register_read_age_frames=" << ptp_register_read_age_frames
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
                          << " display_selected=" << display_preview_selected_frames
                          << " display_skipped=" << display_preview_skipped_frames
                          << " display_max_fps=" << display_preview_max_fps
                          << " yolo=" << (camera_select->yolo ? "on" : "off")
                          << " record=" << (camera_control->record_video ? "on" : "off")
                          << std::endl;
                const PipelinePerfSample pipeline_sample = build_pipeline_perf_sample();
                if (!live_recording_folder.empty() &&
                    live_recording_folder == pipeline_perf_recorder.current_folder()) {
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
                          << " detect_gate=" << pipeline_sample.detect_priority_gated_frames
                          << " detect_wait=" << pipeline_sample.detect_priority_waited_frames
                          << " detect_wait_timeout=" << pipeline_sample.detect_priority_wait_timeouts
                          << " enc_fail=" << pipeline_sample.encode_failures
                          << " enc_slow=" << pipeline_sample.encode_slow_frames
                          << " frame_id_gaps=" << pipeline_sample.camera_dropped_frames
                          << " get_frame_errors=" << pipeline_sample.get_frame_errors
                          << " last_get_frame_error=" << pipeline_sample.last_get_frame_error_code
                          << " display_selected=" << pipeline_sample.display_preview_selected_frames
                          << " display_skipped=" << pipeline_sample.display_preview_skipped_frames
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
                ((ptp_register_read_decimate() <= 1
                      ? ptp_state.ptp_time
                      : ptp_state.frame_ts) > ptp_params->ptp_stop_time)) {
                const uint64_t observed_stop_ns =
                    ptp_register_read_decimate() <= 1
                        ? ptp_state.ptp_time
                        : ptp_state.frame_ts;
                uint64_t ptp_stop_counter = sync_fetch_and_add(&ptp_params->ptp_stop_counter, 1);
                std::cout << "[PTP_STOP] Cam " << camera_params->camera_serial
                          << " reached stop gate count=" << ptp_stop_counter
                          << " target_ns=" << ptp_params->ptp_stop_time
                          << " observed_ns=" << observed_stop_ns
                          << " observed_source="
                          << (ptp_register_read_decimate() <= 1 ? "latched_ptp" : "frame_timestamp")
                          << std::endl;
                while (ptp_params->ptp_stop_counter != static_cast<uint64_t>(camera_params->num_cameras)) {
                    usleep(10);
                }
                ptp_params->ptp_stop_reached = true;
                camera_control->subscribe = false;
                NVTX_RANGE_POP();
                break;
            }
        } else {
            camera_state.get_frame_errors++;
            camera_state.last_get_frame_error_code = camera_state.camera_return;
            get_frame_errors_by_code[camera_state.camera_return]++;
            std::cerr << "EVT_CameraGetFrame Error, " << camera_state.camera_return
                      << " (" << get_evt_error_string(static_cast<EVT_ERROR>(camera_state.camera_return)) << ")"
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
        if (pending.copy_ready_event) {
            cudaEventSynchronize(*pending.copy_ready_event);
        }
        if (pending.consumer_done_event) {
            cudaEventSynchronize(*pending.consumer_done_event);
        }
        EVT_CameraQueueFrame(pending.camera, pending.frame);
    }
    pending_requeues.clear();

    if (camera_control->sync_camera && !ptp_summary_recording_folder.empty()) {
        update_ptp_sync_summary_camera(
            ptp_summary_recording_folder,
            camera_params->camera_serial,
            build_ptp_camera_summary_json(true));
    }
    // Cleanup
    {
        NVTX_RANGE("Cleanup_and_Shutdown");
        CUDA_CTX_LOG("=== ACQUIRE FRAMES CLEANUP ===");

        // FRAME_IPC: Log final statistics if IPC was active
        if (ipc_manager) {
            // IPC logging disabled: final stats.
        }

        if (recording_ingress) {
            const auto source_release_drain_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!recording_ingress->IsDrained() &&
                   std::chrono::steady_clock::now() < source_release_drain_deadline) {
                usleep(1000);
            }
            if (!recording_ingress->IsDrained()) {
                std::cerr << "[SOURCE_RELEASE] Timed out waiting for recording ingress "
                          << "to drain before AcquisitionStop for camera "
                          << camera_params->camera_serial << std::endl;
            }
        }

        (void)finalize_pipeline_perf_recorder_if_drained();
        acquisition_cadence_probe_recorder.Close();

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
    }
}
