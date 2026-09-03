#include "recording_ingress.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unordered_map>
#include <unistd.h>
#include <utility>

#include "encoder_preprocess_worker.h"
#include "external_recorder_ipc_protocol.h"
#include "threadworker.h"
#include "worker_entry_release.h"

namespace {
constexpr uint8_t kRouteModePrimary = 0;
constexpr uint8_t kRouteModeHelper = 1;

uint64_t recording_ingress_now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void append_unique_gpu_id(std::vector<int>* gpu_ids, int gpu_id)
{
    if (!gpu_ids || gpu_id < 0) {
        return;
    }
    if (std::find(gpu_ids->begin(), gpu_ids->end(), gpu_id) == gpu_ids->end()) {
        gpu_ids->push_back(gpu_id);
    }
}

void release_recording_entry_to_recycle(
    SafeQueue<WORKER_ENTRY*>* recycle_queue,
    WORKER_ENTRY* entry,
    const WorkerEntryReleaseContext& context = WorkerEntryReleaseContext{})
{
    release_worker_entry_to_recycle(recycle_queue, entry, context);
}

bool recording_ingress_env_flag_enabled(const char* name, bool default_value = false)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return default_value;
    }
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized == "1" ||
           normalized == "true" ||
           normalized == "yes" ||
           normalized == "on";
}
} // namespace

std::string normalize_recording_sink_mode(const std::string& value)
{
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (normalized.empty()) {
        return "real";
    }
    if (normalized == "real" ||
        normalized == "preprocess_only" ||
        normalized == "immediate_recycle" ||
        normalized == "threaded_handoff_only" ||
        normalized == "external_ipc") {
        return normalized;
    }
    return {};
}

bool is_real_recording_sink_mode(const std::string& value)
{
    return normalize_recording_sink_mode(value) == "real";
}

class RecordingIngress::ThreadedHandoffWorker : public CThreadWorker<WORKER_ENTRY> {
public:
    explicit ThreadedHandoffWorker(SafeQueue<WORKER_ENTRY*>* recycle_queue)
        : CThreadWorker<WORKER_ENTRY>("RecordingSinkHandoff"),
          recycle_queue_(recycle_queue) {}

    double fps() const { return current_fps_.load(std::memory_order_relaxed); }
    bool IsDrained() const {
        return in_flight_.load(std::memory_order_relaxed) == 0 &&
               GetCountQueueIn() == 0;
    }

protected:
    void OnFlushTick() override {}  // no flush-time housekeeping

    bool WorkerFunction(WORKER_ENTRY* entry) override
    {
        if (!entry) {
            return false;  // defensive; flush ticks arrive via OnFlushTick()
        }
        in_flight_.fetch_add(1, std::memory_order_relaxed);
        release_recording_entry_to_recycle(
            recycle_queue_,
            entry,
            WorkerEntryReleaseContext{nullptr, threadName});

        const auto now = std::chrono::steady_clock::now();
        frame_counter_++;
        const std::chrono::duration<double> elapsed = now - last_fps_update_time_;
        if (elapsed.count() >= 1.0) {
            current_fps_.store(frame_counter_ / elapsed.count(), std::memory_order_relaxed);
            frame_counter_ = 0;
            last_fps_update_time_ = now;
        }
        in_flight_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }

private:
    SafeQueue<WORKER_ENTRY*>* recycle_queue_ = nullptr;
    std::chrono::steady_clock::time_point last_fps_update_time_ = std::chrono::steady_clock::now();
    std::atomic<int> frame_counter_{0};
    std::atomic<double> current_fps_{0.0};
    std::atomic<int> in_flight_{0};
};

class RecordingIngress::ExternalIpcHandoffWorker : public CThreadWorker<WORKER_ENTRY> {
public:
    ExternalIpcHandoffWorker(SafeQueue<WORKER_ENTRY*>* recycle_queue,
                             std::string camera_serial,
                             int source_gpu_id,
                             int route_hint_gpu_id,
                             uint32_t recording_gop_length,
                             uint32_t recording_frame_rate)
        : CThreadWorker<WORKER_ENTRY>(("ExternalIpcRecorder_Cam_" + camera_serial).c_str()),
          recycle_queue_(recycle_queue),
          camera_serial_(std::move(camera_serial)),
          source_gpu_id_(source_gpu_id),
          route_hint_gpu_id_(route_hint_gpu_id),
          recording_gop_length_(std::max<uint32_t>(1u, recording_gop_length)),
          recording_frame_rate_(std::max<uint32_t>(1u, recording_frame_rate)),
          session_id_("external_ipc_" + camera_serial_),
          stream_id_(camera_serial_),
          socket_path_("/tmp/orange_external_recorder_" + camera_serial_ + ".sock"),
          ack_timeout_ms_(resolve_ack_timeout_ms()),
          deferred_release_(recording_ingress_env_flag_enabled(
              "ORANGE_EXTERNAL_RECORDER_DEFERRED_RELEASE", false)),
          detect_priority_gate_(recording_ingress_env_flag_enabled(
              "ORANGE_EXTERNAL_RECORDER_DETECT_PRIORITY", true))
    {
        if (detect_priority_gate_) {
            std::cout << "[ExternalIpcRecorder] camera=" << camera_serial_
                      << " detect-priority gate enabled: frames are handed to the"
                      << " recorder after YOLO completes (timeout_ns="
                      << kExternalDetectPriorityWaitTimeoutNs << ")" << std::endl;
        }
        if (deferred_release_) {
            std::cout << "[ExternalIpcRecorder] camera=" << camera_serial_
                      << " forcing deferred source release protocol" << std::endl;
        }
    }

    ~ExternalIpcHandoffWorker() override
    {
        release_all_pending("worker shutdown");
        close_socket();
    }

    double fps() const { return current_fps_.load(std::memory_order_relaxed); }
    bool IsDrained()
    {
        if (in_flight_.load(std::memory_order_relaxed) == 0 &&
            pending_release_count() > 0) {
            poll_protocol_lines(false);
        }
        return in_flight_.load(std::memory_order_relaxed) == 0 &&
               GetCountQueueIn() == 0 &&
               pending_release_count() == 0;
    }
    uint64_t frames_acked() const { return frames_acked_.load(std::memory_order_relaxed); }
    uint64_t failures() const { return failures_.load(std::memory_order_relaxed); }
    uint64_t ack_timeouts() const { return ack_timeouts_.load(std::memory_order_relaxed); }
    void RequestRecordingDrain(const char* reason)
    {
        {
            std::lock_guard<std::mutex> lock(drain_request_mutex_);
            drain_requested_ = true;
            drain_reason_ = reason && *reason ? reason : "recording_draining";
        }
        (void)EnqueueFlushTick();
    }
    void ResetConnection()
    {
        release_all_pending("connection reset");
        close_socket();
        session_id_ = "external_ipc_" + camera_serial_;
        socket_path_ = "/tmp/orange_external_recorder_" + camera_serial_ + ".sock";
    }

protected:
    void ThreadRunning() override
    {
        std::cout << "Child Thread Start 0 (ExternalIpcRecorder_Cam_"
                  << camera_serial_ << ")" << std::endl;
        while (IsMachineOn() || GetCountQueueIn() > 0 || pending_release_count() > 0) {
            if (!IsMachineOn()) {
                send_client_drain_control("worker_draining");
            }
            if (deferred_release_ || pending_release_count() > 0) {
                poll_protocol_lines(false);
            }
            WORKER_ENTRY* entry = GetObjectFromQueueIn();
            if (entry) {
                WorkerFunction(entry);
                continue;
            }
            if (drain_requested_) {
                OnFlushTick();
                continue;
            }
            usleep(1000);
        }
        std::cout << "Child Thread DONE 0 (ExternalIpcRecorder_Cam_"
                  << camera_serial_ << ")" << std::endl;
        send_client_drain_control("worker_drained");
        send_client_finalize_control("worker_drained");
        close_socket();
    }

    void OnFlushTick() override
    {
        if (deferred_release_) {
            poll_protocol_lines(false);
        }
        if (drain_requested_) {
            if (pending_release_count() > 0 ||
                in_flight_.load(std::memory_order_relaxed) > 0 ||
                GetCountQueueIn() > 0) {
                usleep(1000);
                (void)EnqueueFlushTick();
                return;
            }

            std::string reason;
            {
                std::lock_guard<std::mutex> lock(drain_request_mutex_);
                reason = drain_reason_.empty()
                    ? "recording_drained"
                    : drain_reason_;
                drain_requested_ = false;
            }
            send_client_drain_control(reason.c_str());
            send_client_finalize_control(reason.c_str());
        }
    }

    bool WorkerFunction(WORKER_ENTRY* entry) override
    {
        if (!entry) {
            return false;  // defensive; flush ticks arrive via OnFlushTick()
        }
        in_flight_.fetch_add(1, std::memory_order_relaxed);
        if (deferred_release_) {
            poll_protocol_lines(false);
        }
        wait_for_detect_priority(entry);
        bool release_entry_now = true;
        const bool ok = detach_frame(entry, &release_entry_now);
        if (ok) {
            frames_acked_.fetch_add(1, std::memory_order_relaxed);
        } else {
            failures_.fetch_add(1, std::memory_order_relaxed);
        }
        if (release_entry_now) {
            release_recording_entry_to_recycle(
                recycle_queue_,
                entry,
                WorkerEntryReleaseContext{camera_serial_.c_str(), threadName});
        }
        if (deferred_release_) {
            poll_protocol_lines(false);
        }
        update_fps();
        in_flight_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }

public:
    uint64_t detect_priority_gated_frames() const { return detect_priority_gated_frames_.load(std::memory_order_relaxed); }
    uint64_t detect_priority_waited_frames() const { return detect_priority_waited_frames_.load(std::memory_order_relaxed); }
    uint64_t detect_priority_wait_timeouts() const { return detect_priority_wait_timeouts_.load(std::memory_order_relaxed); }
    uint64_t detect_priority_wait_total_ns() const { return detect_priority_wait_total_ns_.load(std::memory_order_relaxed); }
    uint64_t detect_priority_wait_max_ns() const { return detect_priority_wait_max_ns_.load(std::memory_order_relaxed); }

private:
    // Detect-priority gate for the external recorder path (lever 2b in
    // docs/detect_latency_review_2026_09_03.md). The recorder's detach copy
    // and NVENC input copy otherwise land on the detect die while YOLO is
    // running on the same frame; holding the handoff until YOLO's completion
    // event fires moves those copies into the idle part of the frame period.
    // Mirrors EncoderPreprocessWorker's ORANGE_RECORDING_DETECT_PRIORITY gate
    // but waits on completion rather than input-ready, because the copies
    // contend with inference, not just preprocess. Frames never dispatched to
    // YOLO pass through untouched; a wait longer than the timeout is counted
    // and released so recording never stalls behind a stuck detector.
    void wait_for_detect_priority(WORKER_ENTRY* entry)
    {
        if (!detect_priority_gate_ || !entry || !entry->yolo_dispatched) {
            return;
        }
        detect_priority_gated_frames_.fetch_add(1, std::memory_order_relaxed);
        auto yolo_done = [&]() {
            if (entry->detections_ready.load(std::memory_order_acquire)) {
                return true;
            }
            if (!entry->yolo_completion_event ||
                !entry->yolo_completion_event_recorded.load(std::memory_order_acquire)) {
                return false;
            }
            const cudaError_t status = cudaEventQuery(*entry->yolo_completion_event);
            if (status == cudaSuccess) {
                return true;
            }
            if (status == cudaErrorNotReady) {
                return false;
            }
            cudaGetLastError();
            log_limited(std::string("detect-priority completion event query failed: ") +
                        cudaGetErrorString(status));
            return true;
        };
        if (yolo_done()) {
            return;
        }
        const auto wait_start = std::chrono::steady_clock::now();
        bool timed_out = false;
        while (!yolo_done()) {
            const uint64_t elapsed_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - wait_start).count());
            if (elapsed_ns >= kExternalDetectPriorityWaitTimeoutNs) {
                timed_out = true;
                break;
            }
            std::this_thread::sleep_for(kExternalDetectPriorityWaitPollInterval);
        }
        const uint64_t wait_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - wait_start).count());
        detect_priority_waited_frames_.fetch_add(1, std::memory_order_relaxed);
        detect_priority_wait_total_ns_.fetch_add(wait_ns, std::memory_order_relaxed);
        uint64_t observed_max = detect_priority_wait_max_ns_.load(std::memory_order_relaxed);
        while (wait_ns > observed_max &&
               !detect_priority_wait_max_ns_.compare_exchange_weak(
                   observed_max, wait_ns, std::memory_order_relaxed)) {
        }
        if (timed_out) {
            detect_priority_wait_timeouts_.fetch_add(1, std::memory_order_relaxed);
            log_limited("detect-priority wait timed out for recording_frame " +
                        std::to_string(entry->recording_frame_id) +
                        " wait_ns=" + std::to_string(wait_ns));
        }
    }

    static constexpr uint64_t kExternalDetectPriorityWaitTimeoutNs = 50ULL * 1000ULL * 1000ULL;
    static constexpr auto kExternalDetectPriorityWaitPollInterval = std::chrono::microseconds(50);

    static std::string handle_to_hex(const cudaIpcMemHandle_t& handle)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&handle);
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (size_t i = 0; i < sizeof(cudaIpcMemHandle_t); ++i) {
            out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
        }
        return out.str();
    }

    static int resolve_ack_timeout_ms()
    {
        const char* env = std::getenv("ORANGE_EXTERNAL_RECORDER_ACK_TIMEOUT_MS");
        if (!env || !*env) {
            return 1000;
        }
        char* end = nullptr;
        const long parsed = std::strtol(env, &end, 10);
        if (end == env || *end != '\0' || parsed < 1 || parsed > 60000) {
            std::cerr << "[ExternalIpcRecorder] Ignoring invalid "
                      << "ORANGE_EXTERNAL_RECORDER_ACK_TIMEOUT_MS='" << env
                      << "', using 1000" << std::endl;
            return 1000;
        }
        return static_cast<int>(parsed);
    }

    std::string resolve_session_id() const
    {
        const std::string per_camera_env =
            "ORANGE_EXTERNAL_RECORDER_SESSION_ID_CAM_" + camera_serial_;
        if (const char* value = std::getenv(per_camera_env.c_str()); value && *value) {
            return value;
        }
        if (const char* value = std::getenv("ORANGE_EXTERNAL_RECORDER_SESSION_ID"); value && *value) {
            return value;
        }
        return "external_ipc_" + camera_serial_;
    }

    std::string resolve_socket_path() const
    {
        const std::string per_camera_env =
            "ORANGE_EXTERNAL_RECORDER_SOCKET_CAM_" + camera_serial_;
        if (const char* path = std::getenv(per_camera_env.c_str()); path && *path) {
            return path;
        }
        if (const char* path = std::getenv("ORANGE_EXTERNAL_RECORDER_SOCKET"); path && *path) {
            return path;
        }
        return "/tmp/orange_external_recorder_" + camera_serial_ + ".sock";
    }

    void close_socket()
    {
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
        receive_buffer_.clear();
        client_hello_sent_ = false;
        client_drain_sent_ = false;
        client_finalize_sent_ = false;
    }

    bool ensure_connected()
    {
        if (socket_fd_ >= 0) {
            return true;
        }

        socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (socket_fd_ < 0) {
            log_limited("socket() failed: " + std::string(std::strerror(errno)));
            return false;
        }

        timeval timeout{};
        timeout.tv_sec = ack_timeout_ms_ / 1000;
        timeout.tv_usec = (ack_timeout_ms_ % 1000) * 1000;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (socket_path_.size() >= sizeof(addr.sun_path)) {
            log_limited("Socket path too long: " + socket_path_);
            close_socket();
            return false;
        }
        std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
        if (connect(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            log_limited("connect(" + socket_path_ + ") failed: " +
                        std::string(std::strerror(errno)));
            close_socket();
            return false;
        }
        std::cout << "[ExternalIpcRecorder] Connected camera " << camera_serial_
                  << " to " << socket_path_ << std::endl;
        if (!read_recorder_hello()) {
            close_socket();
            return false;
        }
        return true;
    }

    bool send_all(const std::string& data)
    {
        const char* cursor = data.data();
        size_t remaining = data.size();
        while (remaining > 0) {
            const ssize_t written = send(socket_fd_, cursor, remaining, MSG_NOSIGNAL);
            if (written <= 0) {
                return false;
            }
            cursor += written;
            remaining -= static_cast<size_t>(written);
        }
        return true;
    }

    bool read_protocol_line(std::string* line, bool blocking, bool* timed_out)
    {
        if (timed_out) {
            *timed_out = false;
        }
        if (!line) {
            return false;
        }
        while (receive_buffer_.find('\n') == std::string::npos) {
            char ch = '\0';
            const int flags = blocking ? 0 : MSG_DONTWAIT;
            const ssize_t n = recv(socket_fd_, &ch, 1, flags);
            if (n == 0) {
                return false;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (blocking) {
                        ack_timeouts_.fetch_add(1, std::memory_order_relaxed);
                        if (timed_out) {
                            *timed_out = true;
                        }
                    }
                }
                return false;
            }
            receive_buffer_.push_back(ch);
            if (receive_buffer_.size() > 4096) {
                log_limited("external IPC protocol line exceeded 4096 bytes");
                return false;
            }
        }

        const size_t newline = receive_buffer_.find('\n');
        *line = receive_buffer_.substr(0, newline);
        receive_buffer_.erase(0, newline + 1);
        return true;
    }

    bool handle_release_line(uint64_t recording_frame_id)
    {
        WORKER_ENTRY* released_entry = nullptr;
        {
            std::lock_guard<std::mutex> lock(pending_release_mutex_);
            auto it = pending_release_entries_.find(recording_frame_id);
            if (it == pending_release_entries_.end()) {
                return false;
            }
            released_entry = it->second;
            pending_release_entries_.erase(it);
        }
        release_recording_entry_to_recycle(
            recycle_queue_,
            released_entry,
            WorkerEntryReleaseContext{camera_serial_.c_str(), "external_ipc_release"});
        frames_released_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool handle_protocol_line(const std::string& line,
                              uint64_t expected_ack_frame_id,
                              bool* matched_expected_ack,
                              bool* ack_deferred_release)
    {
        if (matched_expected_ack) {
            *matched_expected_ack = false;
        }
        if (ack_deferred_release) {
            *ack_deferred_release = false;
        }
        if (orange::external_recorder::ipc::starts_with_kind(
                line,
                orange::external_recorder::ipc::kRecorderStatusKind)) {
            orange::external_recorder::ipc::RecorderStatusFields status;
            if (!orange::external_recorder::ipc::parse_recorder_status_line(
                    line,
                    &status)) {
                log_limited("invalid external recorder status protocol line: " +
                            status.error + " line='" + line + "'");
                return false;
            }
            recorder_status_messages_received_.fetch_add(
                1,
                std::memory_order_relaxed);
            return true;
        }
        std::istringstream in(line);
        std::string kind;
        uint64_t frame_id = 0;
        in >> kind >> frame_id;
        if (kind == "RELEASE") {
            if (!handle_release_line(frame_id)) {
                log_limited("unexpected external IPC RELEASE for frame " +
                            std::to_string(frame_id));
            }
            return true;
        }
        if (kind == "ACK") {
            if (frame_id == expected_ack_frame_id) {
                std::string token;
                while (in >> token) {
                    if (token == "deferred_release" ||
                        token == "deferred_source_release") {
                        if (ack_deferred_release) {
                            *ack_deferred_release = true;
                        }
                    }
                }
                if (matched_expected_ack) {
                    *matched_expected_ack = true;
                }
                return true;
            }
            log_limited("unexpected external IPC ACK for frame " +
                        std::to_string(frame_id) +
                        " while waiting for " +
                        std::to_string(expected_ack_frame_id));
            return false;
        }
        log_limited("unexpected external IPC protocol line: " + line);
        return false;
    }

    void poll_protocol_lines(bool blocking)
    {
        if (socket_fd_ < 0) {
            return;
        }
        while (true) {
            std::string line;
            bool timed_out = false;
            if (!read_protocol_line(&line, blocking, &timed_out)) {
                return;
            }
            bool matched_ack = false;
            if (!handle_protocol_line(line, 0, &matched_ack, nullptr)) {
                return;
            }
            if (blocking) {
                return;
            }
        }
    }

    bool read_ack(uint64_t recording_frame_id, bool* ack_deferred_release)
    {
        if (ack_deferred_release) {
            *ack_deferred_release = false;
        }
        while (true) {
            std::string line;
            bool timed_out = false;
            if (!read_protocol_line(&line, true, &timed_out)) {
                return false;
            }
            bool matched_ack = false;
            bool line_deferred_release = false;
            if (!handle_protocol_line(
                    line,
                    recording_frame_id,
                    &matched_ack,
                    &line_deferred_release)) {
                return false;
            }
            if (matched_ack) {
                if (ack_deferred_release) {
                    *ack_deferred_release = line_deferred_release;
                }
                return true;
            }
        }
    }

    bool read_recorder_hello()
    {
        std::string line;
        bool timed_out = false;
        if (!read_protocol_line(&line, true, &timed_out)) {
            log_limited(
                timed_out
                    ? "timed out waiting for external recorder protocol hello"
                    : "failed waiting for external recorder protocol hello");
            return false;
        }
        orange::external_recorder::ipc::HelloFields hello;
        if (!orange::external_recorder::ipc::parse_recorder_hello_line(line, &hello)) {
            log_limited("invalid external recorder protocol hello: " +
                        hello.error + " line='" + line + "'");
            return false;
        }
        if (hello.session_id != orange::external_recorder::ipc::token_value(session_id_) ||
            hello.stream_id != orange::external_recorder::ipc::token_value(stream_id_)) {
            log_limited("external recorder protocol identity mismatch: peer session=" +
                        hello.session_id + " stream=" + hello.stream_id +
                        " expected session=" + session_id_ + " stream=" + stream_id_);
            return false;
        }
        std::string identity_error;
        if (!orange::external_recorder::ipc::validate_recording_config_identity(
                hello,
                static_cast<int>(recording_frame_rate_),
                static_cast<int>(recording_gop_length_),
                &identity_error)) {
            log_limited("external recorder recording-config identity rejected: " +
                        identity_error);
            return false;
        }
        if (!send_all(orange::external_recorder::ipc::build_client_hello_line(
                camera_serial_,
                session_id_,
                stream_id_,
                "orange_full_frame",
                static_cast<int>(recording_frame_rate_),
                static_cast<int>(recording_gop_length_)))) {
            log_limited("send client protocol hello failed: " +
                        std::string(std::strerror(errno)));
            return false;
        }
        client_hello_sent_ = true;
        return true;
    }

    bool send_client_control(const char* command, const char* reason)
    {
        if (socket_fd_ < 0 || !client_hello_sent_) {
            return false;
        }
        if (!send_all(orange::external_recorder::ipc::build_client_control_line(
                camera_serial_,
                session_id_,
                stream_id_,
                "orange_full_frame",
                command ? command : "",
                reason ? reason : "drained"))) {
            failures_.fetch_add(1, std::memory_order_relaxed);
            log_limited("send client control failed: " +
                        std::string(std::strerror(errno)));
            return false;
        }
        return true;
    }

    void send_client_drain_control(const char* reason)
    {
        if (client_drain_sent_) {
            return;
        }
        if (send_client_control(
                orange::external_recorder::ipc::kClientControlDrain,
                reason)) {
            client_drain_sent_ = true;
        }
    }

    void send_client_finalize_control(const char* reason)
    {
        if (client_finalize_sent_) {
            return;
        }
        if (!client_drain_sent_) {
            send_client_drain_control(reason);
        }
        if (!send_client_control(
                orange::external_recorder::ipc::kClientControlFinalize,
                reason)) {
            return;
        }
        client_finalize_sent_ = true;
    }

    bool detach_frame(WORKER_ENTRY* entry, bool* release_entry_now)
    {
        if (release_entry_now) {
            *release_entry_now = true;
        }
        refresh_session_from_environment();

        if (source_gpu_id_ >= 0) {
            cudaSetDevice(source_gpu_id_);
        }

        cudaEvent_t* ready_event = entry->delayed_consumer_event();
        if (ready_event) {
            const cudaError_t wait_status = cudaEventSynchronize(*ready_event);
            if (wait_status != cudaSuccess) {
                log_limited(std::string("cudaEventSynchronize failed: ") +
                            cudaGetErrorString(wait_status));
                return false;
            }
        }

        unsigned char* source_ptr = entry->delayed_consumer_image();
        if (!source_ptr || entry->source_buffer_bytes == 0) {
            log_limited("Frame has no exportable source buffer");
            return false;
        }
        if (!entry->owns_memory && !entry->has_analytics_owned_source()) {
            log_limited("Frame source is not owned cudaMalloc memory; enable owned/ring source");
            return false;
        }

        std::string handle_hex;
        auto handle_it = handle_cache_.find(source_ptr);
        if (handle_it == handle_cache_.end()) {
            cudaIpcMemHandle_t handle{};
            const cudaError_t handle_status = cudaIpcGetMemHandle(
                &handle,
                static_cast<void*>(source_ptr));
            if (handle_status != cudaSuccess) {
                log_limited(std::string("cudaIpcGetMemHandle failed: ") +
                            cudaGetErrorString(handle_status));
                return false;
            }
            handle_hex = handle_to_hex(handle);
            handle_cache_.emplace(source_ptr, handle_hex);
        } else {
            handle_hex = handle_it->second;
        }

        if (!ensure_connected()) {
            return false;
        }

        std::ostringstream msg;
        const uint64_t zero_based_frame =
            entry->recording_frame_id > 0 ? entry->recording_frame_id - 1 : 0;
        const uint64_t gop_index =
            zero_based_frame / static_cast<uint64_t>(recording_gop_length_);
        const uint32_t frame_index_within_gop = static_cast<uint32_t>(
            zero_based_frame % static_cast<uint64_t>(recording_gop_length_));

        msg << "FRAME "
            << camera_serial_ << " "
            << entry->recording_frame_id << " "
            << entry->frame_id << " "
            << source_gpu_id_ << " "
            << entry->width << " "
            << entry->height << " "
            << entry->pixelFormat << " "
            << entry->source_buffer_bytes << " "
            << entry->timestamp << " "
            << entry->timestamp_sys << " "
            << handle_hex << " "
            << session_id_ << " "
            << stream_id_ << " "
            << gop_index << " "
            << frame_index_within_gop << " "
            << route_hint_gpu_id_ << " "
            << 0 << " "
            << "single_shard"
            << "\n";

        if (!send_all(msg.str())) {
            log_limited("send frame descriptor failed: " + std::string(std::strerror(errno)));
            close_socket();
            return false;
        }
        const bool force_deferred_release = deferred_release_;
        if (force_deferred_release) {
            {
                std::lock_guard<std::mutex> lock(pending_release_mutex_);
                pending_release_entries_[entry->recording_frame_id] = entry;
            }
            if (release_entry_now) {
                *release_entry_now = false;
            }
        }
        bool ack_deferred_release = false;
        if (!read_ack(entry->recording_frame_id, &ack_deferred_release)) {
            log_limited("detach ack failed for frame " +
                        std::to_string(entry->recording_frame_id));
            bool still_pending = false;
            if (force_deferred_release) {
                std::lock_guard<std::mutex> lock(pending_release_mutex_);
                auto it = pending_release_entries_.find(entry->recording_frame_id);
                if (it != pending_release_entries_.end()) {
                    pending_release_entries_.erase(it);
                    still_pending = true;
                }
            }
            if (release_entry_now) {
                *release_entry_now = still_pending || !force_deferred_release;
            }
            close_socket();
            return false;
        }
        if (ack_deferred_release && !force_deferred_release) {
            {
                std::lock_guard<std::mutex> lock(pending_release_mutex_);
                pending_release_entries_[entry->recording_frame_id] = entry;
            }
            if (release_entry_now) {
                *release_entry_now = false;
            }
        }
        if (force_deferred_release || ack_deferred_release) {
            poll_protocol_lines(false);
        }
        return true;
    }

    size_t pending_release_count() const
    {
        std::lock_guard<std::mutex> lock(pending_release_mutex_);
        return pending_release_entries_.size();
    }

    void release_all_pending(const char* reason)
    {
        std::unordered_map<uint64_t, WORKER_ENTRY*> pending;
        {
            std::lock_guard<std::mutex> lock(pending_release_mutex_);
            pending.swap(pending_release_entries_);
        }
        if (pending.empty()) {
            return;
        }
        std::cerr << "[ExternalIpcRecorder] camera=" << camera_serial_
                  << " releasing " << pending.size()
                  << " pending external IPC frames during "
                  << (reason ? reason : "cleanup") << std::endl;
        for (auto& [frame_id, entry] : pending) {
            (void)frame_id;
            release_recording_entry_to_recycle(
                recycle_queue_,
                entry,
                WorkerEntryReleaseContext{camera_serial_.c_str(), "external_ipc_cleanup"});
        }
    }

    void update_fps()
    {
        const auto now = std::chrono::steady_clock::now();
        frame_counter_++;
        const std::chrono::duration<double> elapsed = now - last_fps_update_time_;
        if (elapsed.count() >= 1.0) {
            current_fps_.store(frame_counter_ / elapsed.count(), std::memory_order_relaxed);
            frame_counter_ = 0;
            last_fps_update_time_ = now;
        }
    }

    void log_limited(const std::string& message)
    {
        const uint64_t count = failures_logged_.fetch_add(1, std::memory_order_relaxed);
        if (count < 10 || (count % 100) == 0) {
            std::cerr << "[ExternalIpcRecorder] camera=" << camera_serial_
                      << " " << message << std::endl;
        }
    }

    void refresh_session_from_environment()
    {
        const std::string next_session_id = resolve_session_id();
        const std::string next_socket_path = resolve_socket_path();
        if (next_session_id == session_id_ && next_socket_path == socket_path_) {
            return;
        }
        release_all_pending("session refresh");
        close_socket();
        session_id_ = next_session_id;
        socket_path_ = next_socket_path;
    }

    SafeQueue<WORKER_ENTRY*>* recycle_queue_ = nullptr;
    std::string camera_serial_;
    int source_gpu_id_ = -1;
    int route_hint_gpu_id_ = -1;
    uint32_t recording_gop_length_ = 1;
    uint32_t recording_frame_rate_ = 1;
    std::string session_id_;
    std::string stream_id_;
    std::string socket_path_;
    int ack_timeout_ms_ = 1000;
    int socket_fd_ = -1;
    bool deferred_release_ = false;
    bool detect_priority_gate_ = false;
    std::atomic<uint64_t> detect_priority_gated_frames_{0};
    std::atomic<uint64_t> detect_priority_waited_frames_{0};
    std::atomic<uint64_t> detect_priority_wait_timeouts_{0};
    std::atomic<uint64_t> detect_priority_wait_total_ns_{0};
    std::atomic<uint64_t> detect_priority_wait_max_ns_{0};
    bool client_hello_sent_ = false;
    bool client_drain_sent_ = false;
    bool client_finalize_sent_ = false;
    std::atomic<bool> drain_requested_{false};
    std::mutex drain_request_mutex_;
    std::string drain_reason_;
    std::string receive_buffer_;
    mutable std::mutex pending_release_mutex_;
    std::unordered_map<uint64_t, WORKER_ENTRY*> pending_release_entries_;
    std::unordered_map<unsigned char*, std::string> handle_cache_;
    std::chrono::steady_clock::time_point last_fps_update_time_ = std::chrono::steady_clock::now();
    std::atomic<int> frame_counter_{0};
    std::atomic<double> current_fps_{0.0};
    std::atomic<int> in_flight_{0};
    std::atomic<uint64_t> frames_acked_{0};
    std::atomic<uint64_t> frames_released_{0};
    std::atomic<uint64_t> recorder_status_messages_received_{0};
    std::atomic<uint64_t> failures_{0};
    std::atomic<uint64_t> ack_timeouts_{0};
    std::atomic<uint64_t> failures_logged_{0};
};

RecordingIngress::RecordingIngress(EncoderPreprocessWorker* primary_preprocess_worker,
                                   int source_gpu_id,
                                   int primary_encode_gpu_id,
                                   uint32_t recording_gop_length,
                                   uint32_t recording_frame_rate,
                                   const ResolvedRecordingConfig& resolved_recording_config,
                                   SafeQueue<WORKER_ENTRY*>* recycle_queue,
                                   const std::string& recording_sink_mode,
                                   const std::string& camera_serial)
    : primary_preprocess_worker_(primary_preprocess_worker),
      source_gpu_id_(source_gpu_id),
      primary_encode_gpu_id_(primary_encode_gpu_id),
      recording_gop_length_(std::max<uint32_t>(1u, recording_gop_length)),
      recording_frame_rate_(std::max<uint32_t>(1u, recording_frame_rate)),
      resolved_recording_config_(resolved_recording_config),
      recycle_queue_(recycle_queue),
      recording_sink_mode_(normalize_recording_sink_mode(recording_sink_mode)),
      camera_serial_(camera_serial.empty() ? "unknown" : camera_serial)
{
    if (recording_sink_mode_.empty()) {
        throw std::runtime_error("Unsupported recording sink mode: " + recording_sink_mode);
    }

    if (recording_sink_mode_ == "threaded_handoff_only") {
        if (!recycle_queue_) {
            throw std::runtime_error(
                "threaded_handoff_only recording sink mode requires a recycle queue");
        }
        threaded_handoff_worker_ = std::make_unique<ThreadedHandoffWorker>(recycle_queue_);
    } else if (recording_sink_mode_ == "external_ipc") {
        if (!recycle_queue_) {
            throw std::runtime_error(
                "external_ipc recording sink mode requires a recycle queue");
        }
        const std::string serial = camera_serial.empty() ? "unknown" : camera_serial;
        external_ipc_handoff_worker_ =
            std::make_unique<ExternalIpcHandoffWorker>(
                recycle_queue_,
                serial,
                source_gpu_id_,
                primary_encode_gpu_id_,
                recording_gop_length_,
                recording_frame_rate_);
    }

    if (recording_sink_mode_ != "real") {
        route_gpu_ids_.push_back(primary_encode_gpu_id_);
        return;
    }

    if (!resolved_recording_config_.strategy.split_gop_enabled()) {
        route_gpu_ids_.push_back(primary_encode_gpu_id_);
        return;
    }

    const std::string& policy = resolved_recording_config_.strategy.split_gop.source_encoder_policy;
    if (policy == "pure_offload") {
        for (int gpu_id : resolved_recording_config_.strategy.split_gop.encoder_gpu_ids) {
            if (gpu_id != primary_encode_gpu_id_) {
                append_unique_gpu_id(&route_gpu_ids_, gpu_id);
            }
        }
        if (route_gpu_ids_.empty() && !resolved_recording_config_.strategy.split_gop.strict) {
            route_gpu_ids_.push_back(primary_encode_gpu_id_);
        }
        return;
    }

    route_gpu_ids_.push_back(primary_encode_gpu_id_);
    if (policy == "hybrid_split") {
        for (int gpu_id : resolved_recording_config_.strategy.split_gop.encoder_gpu_ids) {
            if (gpu_id != primary_encode_gpu_id_) {
                append_unique_gpu_id(&route_gpu_ids_, gpu_id);
            }
        }
    }
}

RecordingIngress::~RecordingIngress()
{
    shutdown();
}

void RecordingIngress::RegisterHelperPreprocessWorker(int encode_gpu_id,
                                                      EncoderPreprocessWorker* preprocess_worker)
{
    if (encode_gpu_id < 0 || !preprocess_worker) {
        return;
    }
    if (encode_gpu_id == primary_encode_gpu_id_) {
        primary_preprocess_worker_ = preprocess_worker;
        return;
    }
    helper_preprocess_workers_[encode_gpu_id] = preprocess_worker;
    append_unique_gpu_id(&route_gpu_ids_, encode_gpu_id);
}

int RecordingIngress::select_target_gpu_id(uint64_t recording_frame_id, bool* helper_requested) const
{
    if (helper_requested) {
        *helper_requested = false;
    }
    if (route_gpu_ids_.empty()) {
        if (resolved_recording_config_.strategy.split_gop_enabled() &&
            resolved_recording_config_.strategy.split_gop.source_encoder_policy == "pure_offload") {
            if (helper_requested) {
                *helper_requested = true;
            }
            return -1;
        }
        return primary_encode_gpu_id_;
    }
    if (route_gpu_ids_.size() == 1) {
        const int only_gpu = route_gpu_ids_.front();
        if (helper_requested) {
            *helper_requested = only_gpu != primary_encode_gpu_id_;
        }
        return only_gpu;
    }

    const uint64_t zero_based_frame = recording_frame_id > 0 ? recording_frame_id - 1 : 0;
    const uint64_t gop_index = zero_based_frame / static_cast<uint64_t>(recording_gop_length_);
    const int target_gpu_id = route_gpu_ids_[static_cast<size_t>(gop_index % route_gpu_ids_.size())];
    if (helper_requested) {
        *helper_requested = target_gpu_id != primary_encode_gpu_id_;
    }
    return target_gpu_id;
}

EncoderPreprocessWorker* RecordingIngress::resolve_target_worker(int target_gpu_id) const
{
    if (target_gpu_id == primary_encode_gpu_id_) {
        return primary_preprocess_worker_;
    }
    const auto it = helper_preprocess_workers_.find(target_gpu_id);
    if (it == helper_preprocess_workers_.end()) {
        return nullptr;
    }
    return it->second;
}

void RecordingIngress::increment_last_route_mode_primary()
{
    last_route_mode_.store(kRouteModePrimary, std::memory_order_relaxed);
}

void RecordingIngress::increment_last_route_mode_helper()
{
    last_route_mode_.store(kRouteModeHelper, std::memory_order_relaxed);
}

bool RecordingIngress::SubmitFrame(WORKER_ENTRY* entry)
{
    if (entry) {
        entry->recording_submit_host_ns = recording_ingress_now_ns();
        entry->recording_target_gpu_id = -1;
        entry->recording_helper_requested = false;
        entry->recording_route_helper = false;
        entry->helper_enqueue_host_ns = 0;
        entry->helper_enqueue_queue_depth = -1;
        entry->helper_enqueue_available_buffers = -1;
        entry->helper_enqueue_available_events = -1;
    }

    if (recording_sink_mode_ == "immediate_recycle") {
        submitted_frames_.fetch_add(1, std::memory_order_relaxed);
        primary_routed_frames_.fetch_add(1, std::memory_order_relaxed);
        last_target_gpu_id_.store(primary_encode_gpu_id_, std::memory_order_relaxed);
        increment_last_route_mode_primary();
        if (entry) {
            entry->recording_target_gpu_id = primary_encode_gpu_id_;
            entry->recording_route_helper = false;
        }
        release_entry(entry);
        return true;
    }

    if (recording_sink_mode_ == "threaded_handoff_only") {
        if (!threaded_handoff_worker_) {
            throw std::runtime_error("threaded_handoff_only sink mode has no handoff worker");
        }
        submitted_frames_.fetch_add(1, std::memory_order_relaxed);
        primary_routed_frames_.fetch_add(1, std::memory_order_relaxed);
        last_target_gpu_id_.store(primary_encode_gpu_id_, std::memory_order_relaxed);
        increment_last_route_mode_primary();
        if (entry) {
            entry->recording_target_gpu_id = primary_encode_gpu_id_;
            entry->recording_route_helper = false;
        }
        if (!threaded_handoff_worker_->PutObjectToQueueIn(entry)) {
            const uint64_t dropped_total =
                enqueue_rejected_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (should_log_deduplicated_count(dropped_total)) {
                std::cerr << "[RecordingIngress][WARNING] frame dropped:"
                          << " threaded_handoff_only enqueue rejected"
                          << " cam=" << camera_serial_
                          << " frame=" << (entry ? entry->frame_id : 0)
                          << " recording_frame=" << (entry ? entry->recording_frame_id : 0)
                          << " queue_depth=" << threaded_handoff_worker_->GetCountQueueInSize()
                          << " queue_capacity=" << threaded_handoff_worker_->GetMaxQueueSize()
                          << " dropped_total=" << dropped_total
                          << std::endl;
            }
            return false;
        }
        return true;
    }

    if (recording_sink_mode_ == "external_ipc") {
        if (!external_ipc_handoff_worker_) {
            throw std::runtime_error("external_ipc sink mode has no handoff worker");
        }
        submitted_frames_.fetch_add(1, std::memory_order_relaxed);
        primary_routed_frames_.fetch_add(1, std::memory_order_relaxed);
        last_target_gpu_id_.store(primary_encode_gpu_id_, std::memory_order_relaxed);
        increment_last_route_mode_primary();
        if (entry) {
            entry->recording_target_gpu_id = primary_encode_gpu_id_;
            entry->recording_route_helper = false;
        }
        if (!external_ipc_handoff_worker_->PutObjectToQueueIn(entry)) {
            const uint64_t dropped_total =
                enqueue_rejected_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (should_log_deduplicated_count(dropped_total)) {
                std::cerr << "[RecordingIngress][WARNING] frame dropped:"
                          << " external_ipc enqueue rejected"
                          << " cam=" << camera_serial_
                          << " frame=" << (entry ? entry->frame_id : 0)
                          << " recording_frame=" << (entry ? entry->recording_frame_id : 0)
                          << " queue_depth=" << external_ipc_handoff_worker_->GetCountQueueInSize()
                          << " queue_capacity=" << external_ipc_handoff_worker_->GetMaxQueueSize()
                          << " dropped_total=" << dropped_total
                          << std::endl;
            }
            return false;
        }
        return true;
    }

    if (!primary_preprocess_worker_) {
        throw std::runtime_error("RecordingIngress has no primary preprocess worker");
    }

    submitted_frames_.fetch_add(1, std::memory_order_relaxed);

    bool helper_requested = false;
    int target_gpu_id = select_target_gpu_id(entry ? entry->recording_frame_id : 0, &helper_requested);
    if (entry) {
        entry->recording_helper_requested = helper_requested;
    }
    EncoderPreprocessWorker* target_worker = resolve_target_worker(target_gpu_id);
    if (helper_requested) {
        helper_requested_frames_.fetch_add(1, std::memory_order_relaxed);
    }

    if (!target_worker) {
        if (helper_requested && resolved_recording_config_.strategy.split_gop.strict) {
            throw std::runtime_error(
                "Split-GOP helper GPU " + std::to_string(target_gpu_id) +
                " was selected by RecordingIngress but no helper preprocess worker is registered");
        }
        target_gpu_id = primary_encode_gpu_id_;
        target_worker = primary_preprocess_worker_;
        if (helper_requested) {
            helper_fallback_frames_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (target_gpu_id == primary_encode_gpu_id_) {
        primary_routed_frames_.fetch_add(1, std::memory_order_relaxed);
        increment_last_route_mode_primary();
        if (entry) {
            entry->recording_route_helper = false;
        }
    } else {
        helper_dispatched_frames_.fetch_add(1, std::memory_order_relaxed);
        increment_last_route_mode_helper();
        if (entry) {
            entry->recording_route_helper = true;
            entry->helper_enqueue_host_ns = entry->recording_submit_host_ns;
            entry->helper_enqueue_queue_depth = target_worker->GetCountQueueInSize();
            entry->helper_enqueue_available_buffers =
                target_worker->available_buffers_.load(std::memory_order_relaxed);
            entry->helper_enqueue_available_events =
                target_worker->available_events_.load(std::memory_order_relaxed);
        }
    }

    last_target_gpu_id_.store(target_gpu_id, std::memory_order_relaxed);
    if (entry) {
        entry->recording_target_gpu_id = target_gpu_id;
    }
    if (!target_worker->PutObjectToQueueIn(entry)) {
        const uint64_t dropped_total =
            enqueue_rejected_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (should_log_deduplicated_count(dropped_total)) {
            std::cerr << "[RecordingIngress][WARNING] frame dropped:"
                      << " preprocess enqueue rejected"
                      << " cam=" << camera_serial_
                      << " frame=" << (entry ? entry->frame_id : 0)
                      << " recording_frame=" << (entry ? entry->recording_frame_id : 0)
                      << " target_gpu=" << target_gpu_id
                      << " queue_depth=" << target_worker->GetCountQueueInSize()
                      << " queue_capacity=" << target_worker->GetMaxQueueSize()
                      << " dropped_total=" << dropped_total
                      << std::endl;
        }
        return false;
    }
    return true;
}

RecordingIngressStats RecordingIngress::GetStats() const
{
    RecordingIngressStats stats;
    auto accumulate_nonnegative = [](int* total, int value) {
        if (!total || value < 0) {
            return;
        }
        if (*total < 0) {
            *total = 0;
        }
        *total += value;
    };

    auto accumulate_worker = [&](EncoderPreprocessWorker* worker, bool is_primary) {
        if (!worker) {
            return;
        }

        const double preprocess_fps = worker->get_fps();
        const double encode_fps = worker->get_hw_fps();
        if (is_primary) {
            stats.preprocess_fps_primary = preprocess_fps;
            stats.encode_fps_primary = encode_fps;
        } else {
            stats.preprocess_fps_helpers += preprocess_fps;
            stats.encode_fps_helpers += encode_fps;
        }

        accumulate_nonnegative(&stats.preprocess_queue_depth, worker->GetCountQueueInSize());
        accumulate_nonnegative(&stats.encode_queue_depth, worker->get_hw_queue_depth());
        accumulate_nonnegative(
            &stats.preprocess_buffers_available,
            worker->available_buffers_.load(std::memory_order_relaxed));
        accumulate_nonnegative(
            &stats.preprocess_events_available,
            worker->available_events_.load(std::memory_order_relaxed));

        stats.preprocess_resource_waits += worker->get_resource_waits();
        stats.preprocess_frames_dropped += worker->get_frames_dropped();
        stats.detect_priority_gated_frames += worker->get_detect_priority_gated_frames();
        stats.detect_priority_waited_frames += worker->get_detect_priority_waited_frames();
        stats.detect_priority_wait_timeouts += worker->get_detect_priority_wait_timeouts();
        stats.detect_priority_wait_total_ns += worker->get_detect_priority_wait_total_ns();
        stats.detect_priority_wait_max_ns = std::max(
            stats.detect_priority_wait_max_ns,
            worker->get_detect_priority_wait_max_ns());
        stats.encode_failures += worker->get_hw_encode_failures();
        stats.encode_slow_frames += worker->get_hw_slow_frames();
    };

    if (recording_sink_mode_ == "threaded_handoff_only") {
        stats.preprocess_fps_primary = threaded_handoff_worker_ ? threaded_handoff_worker_->fps() : 0.0;
        stats.preprocess_fps = stats.preprocess_fps_primary;
        stats.preprocess_queue_depth =
            threaded_handoff_worker_ ? threaded_handoff_worker_->GetCountQueueInSize() : -1;
    } else if (recording_sink_mode_ == "external_ipc") {
        stats.preprocess_fps_primary =
            external_ipc_handoff_worker_ ? external_ipc_handoff_worker_->fps() : 0.0;
        stats.preprocess_fps = stats.preprocess_fps_primary;
        stats.preprocess_queue_depth =
            external_ipc_handoff_worker_ ? external_ipc_handoff_worker_->GetCountQueueInSize() : -1;
        stats.external_ipc_frames_acked =
            external_ipc_handoff_worker_ ? external_ipc_handoff_worker_->frames_acked() : 0;
        stats.external_ipc_failures =
            external_ipc_handoff_worker_ ? external_ipc_handoff_worker_->failures() : 0;
        stats.external_ipc_ack_timeouts =
            external_ipc_handoff_worker_ ? external_ipc_handoff_worker_->ack_timeouts() : 0;
        if (external_ipc_handoff_worker_) {
            stats.detect_priority_gated_frames =
                external_ipc_handoff_worker_->detect_priority_gated_frames();
            stats.detect_priority_waited_frames =
                external_ipc_handoff_worker_->detect_priority_waited_frames();
            stats.detect_priority_wait_timeouts =
                external_ipc_handoff_worker_->detect_priority_wait_timeouts();
            stats.detect_priority_wait_total_ns =
                external_ipc_handoff_worker_->detect_priority_wait_total_ns();
            stats.detect_priority_wait_max_ns =
                external_ipc_handoff_worker_->detect_priority_wait_max_ns();
        }
    } else if (recording_sink_mode_ == "real") {
        accumulate_worker(primary_preprocess_worker_, true);
        for (const auto& [gpu_id, worker] : helper_preprocess_workers_) {
            (void)gpu_id;
            accumulate_worker(worker, false);
        }

        stats.preprocess_fps = stats.preprocess_fps_primary + stats.preprocess_fps_helpers;
        stats.encode_fps = stats.encode_fps_primary + stats.encode_fps_helpers;
    }
    stats.submitted_frames = submitted_frames_.load(std::memory_order_relaxed);
    stats.primary_routed_frames = primary_routed_frames_.load(std::memory_order_relaxed);
    stats.helper_requested_frames = helper_requested_frames_.load(std::memory_order_relaxed);
    stats.helper_fallback_frames = helper_fallback_frames_.load(std::memory_order_relaxed);
    stats.helper_dispatched_frames = helper_dispatched_frames_.load(std::memory_order_relaxed);
    stats.enqueue_rejected_frames = enqueue_rejected_frames_.load(std::memory_order_relaxed);
    stats.last_target_gpu_id = last_target_gpu_id_.load(std::memory_order_relaxed);
    stats.last_route_mode =
        last_route_mode_.load(std::memory_order_relaxed) == kRouteModeHelper ? "helper" : "primary";
    return stats;
}

bool RecordingIngress::IsDrained() const
{
    if (recording_sink_mode_ == "immediate_recycle") {
        return true;
    }
    if (recording_sink_mode_ == "threaded_handoff_only") {
        return !threaded_handoff_worker_ || threaded_handoff_worker_->IsDrained();
    }
    if (recording_sink_mode_ == "external_ipc") {
        return !external_ipc_handoff_worker_ || external_ipc_handoff_worker_->IsDrained();
    }
    if (primary_preprocess_worker_ && !primary_preprocess_worker_->IsDrained()) {
        return false;
    }
    for (const auto& [gpu_id, worker] : helper_preprocess_workers_) {
        (void)gpu_id;
        if (worker && !worker->IsDrained()) {
            return false;
        }
    }
    return true;
}

void RecordingIngress::start()
{
    if (threaded_handoff_worker_) {
        threaded_handoff_worker_->SetMaxQueueSize(240);
        threaded_handoff_worker_->StartThread();
    }
    if (external_ipc_handoff_worker_) {
        external_ipc_handoff_worker_->SetMaxQueueSize(240);
        external_ipc_handoff_worker_->StartThread();
    }
}

bool RecordingIngress::uses_external_ipc() const
{
    return recording_sink_mode_ == "external_ipc";
}

void RecordingIngress::request_recording_drain()
{
    if (external_ipc_handoff_worker_) {
        external_ipc_handoff_worker_->RequestRecordingDrain("recording_draining");
    }
}

void RecordingIngress::request_stop()
{
    if (threaded_handoff_worker_) {
        threaded_handoff_worker_->StopThread();
    }
    if (external_ipc_handoff_worker_) {
        external_ipc_handoff_worker_->StopThread();
    }
}

void RecordingIngress::shutdown()
{
    request_stop();
    threaded_handoff_worker_.reset();
    external_ipc_handoff_worker_.reset();
}

void RecordingIngress::reset_external_ipc_connection()
{
    if (external_ipc_handoff_worker_) {
        external_ipc_handoff_worker_->ResetConnection();
    }
}

bool RecordingIngress::requires_owned_cuda_source() const
{
    return recording_sink_mode_ == "external_ipc";
}

void RecordingIngress::release_entry(WORKER_ENTRY* entry)
{
    release_recording_entry_to_recycle(
        recycle_queue_,
        entry,
        WorkerEntryReleaseContext{camera_serial_.c_str(), "recording_ingress"});
}
