#pragma once

#include "json.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace orange::gui {

struct GuiStartupTimingCamera {
    std::string serial;
    int camera_index = -1;
    int gpu_id = -1;
    nlohmann::json context = nlohmann::json::object();
};

// Small, copyable control-plane view of a timing report. It intentionally
// excludes stage arrays so readiness checks do not copy the full artifact.
struct GuiStartupTimingStatus {
    bool available = false;
    std::string transaction_id;
    std::string operation;
    std::string status;
    std::string failure_reason;
    std::string stop_reason;
    std::string artifact_path;
    std::size_t expected_first_frame_camera_count = 0;
    std::size_t observed_first_frame_camera_count = 0;
};

// Records startup-only timing data. All methods are fail-open: diagnostics
// must never change whether a camera or stream starts successfully.
class GuiStartupTimingRecorder {
public:
    static uint64_t NowNs() noexcept;

    void Begin(
        const std::string& operation,
        const std::filesystem::path& artifact_directory,
        const std::vector<GuiStartupTimingCamera>& cameras,
        const nlohmann::json& context = nlohmann::json::object(),
        uint64_t request_started_ns = 0) noexcept;

    void RecordGlobalInterval(
        const std::string& stage,
        uint64_t started_ns,
        uint64_t finished_ns,
        bool completed_normally = true) noexcept;
    void RecordCameraInterval(
        const std::string& camera_serial,
        const std::string& stage,
        uint64_t started_ns,
        uint64_t finished_ns,
        bool completed_normally = true) noexcept;
    void MarkGlobalInstant(
        const std::string& event,
        uint64_t at_ns = 0,
        const nlohmann::json& details = nlohmann::json::object()) noexcept;
    void MarkCameraInstant(
        const std::string& camera_serial,
        const std::string& event,
        uint64_t at_ns = 0,
        const nlohmann::json& details = nlohmann::json::object()) noexcept;

    void MarkHandlerComplete(uint64_t at_ns = 0) noexcept;
    void MarkOperationComplete(uint64_t at_ns = 0) noexcept;
    void MarkFailed(const std::string& reason, uint64_t at_ns = 0) noexcept;
    void MarkStopped(const std::string& reason, uint64_t at_ns = 0) noexcept;
    void MarkFirstFrame(
        const std::string& camera_serial,
        uint64_t local_frame_id,
        uint64_t camera_frame_id,
        uint64_t camera_timestamp_ns,
        uint64_t receive_host_ns) noexcept;

    // Called from the GUI thread. Writes at most once per changed snapshot and
    // uses a temporary file + rename so readers never observe partial JSON.
    void FlushPending() noexcept;

    nlohmann::json Snapshot() const noexcept;
    GuiStartupTimingStatus Status() const noexcept;
    std::filesystem::path artifact_path() const noexcept;

private:
    void RecordIntervalLocked(
        nlohmann::json* destination,
        const std::string& stage,
        uint64_t started_ns,
        uint64_t finished_ns,
        bool completed_normally);
    void MarkInstantLocked(
        nlohmann::json* destination,
        const std::string& event,
        uint64_t at_ns,
        const nlohmann::json& details);
    void RefreshDerivedLocked(uint64_t at_ns);
    double OffsetMsLocked(uint64_t at_ns) const;

    mutable std::mutex mutex_;
    nlohmann::json report_ = nlohmann::json::object();
    std::filesystem::path artifact_path_;
    uint64_t request_started_ns_ = 0;
    uint64_t version_ = 0;
    uint64_t flushed_version_ = 0;
    uint64_t last_write_error_version_ = 0;
    bool active_ = false;
    bool final_summary_logged_ = false;
};

class GuiStartupTimingScope {
public:
    GuiStartupTimingScope(
        GuiStartupTimingRecorder* recorder,
        std::string stage,
        std::string camera_serial = {}) noexcept;
    ~GuiStartupTimingScope() noexcept;

    GuiStartupTimingScope(const GuiStartupTimingScope&) = delete;
    GuiStartupTimingScope& operator=(const GuiStartupTimingScope&) = delete;

    void Finish() noexcept;

private:
    GuiStartupTimingRecorder* recorder_ = nullptr;
    std::string stage_;
    std::string camera_serial_;
    uint64_t started_ns_ = 0;
    int uncaught_exceptions_at_start_ = 0;
    bool finished_ = false;
};

}  // namespace orange::gui
