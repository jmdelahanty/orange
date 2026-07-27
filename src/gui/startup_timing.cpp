#include "gui/startup_timing.h"

#include "fsuid_guard.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace orange::gui {
namespace {

std::atomic<uint64_t> g_startup_timing_sequence{0};

std::string utc_timestamp(const char* format)
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now_time);
#else
    gmtime_r(&now_time, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, format);
    return out.str();
}

uint64_t thread_id_hash() noexcept
{
    try {
        return static_cast<uint64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
    } catch (...) {
        return 0;
    }
}

bool terminal_status(const std::string& status)
{
    return status == "complete" || status == "failed" || status == "stopped";
}

bool write_json_atomic(
    const std::filesystem::path& destination,
    const nlohmann::json& payload,
    std::string* error_out)
{
    try {
        orange::ScopedFsuid user_files;
        std::error_code ec;
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) {
            if (error_out) {
                *error_out = "create_directories failed: " + ec.message();
            }
            return false;
        }

        const std::filesystem::path temporary =
            destination.string() + ".tmp." + std::to_string(getpid()) + "." +
            std::to_string(thread_id_hash());
        {
            std::ofstream output(temporary, std::ios::out | std::ios::trunc);
            if (!output.is_open()) {
                if (error_out) {
                    *error_out = "could not open temporary file " + temporary.string();
                }
                return false;
            }
            output << payload.dump(2) << '\n';
            output.flush();
            if (!output.good()) {
                if (error_out) {
                    *error_out = "could not write temporary file " + temporary.string();
                }
                output.close();
                std::filesystem::remove(temporary, ec);
                return false;
            }
        }

        std::filesystem::rename(temporary, destination, ec);
        if (ec) {
            // POSIX rename replaces an existing file, but retain a portable
            // fallback for filesystems with stricter replacement behavior.
            std::error_code remove_error;
            std::filesystem::remove(destination, remove_error);
            ec.clear();
            std::filesystem::rename(temporary, destination, ec);
        }
        if (ec) {
            if (error_out) {
                *error_out = "rename failed: " + ec.message();
            }
            std::filesystem::remove(temporary, ec);
            return false;
        }
        return true;
    } catch (const std::exception& error) {
        if (error_out) {
            *error_out = error.what();
        }
        return false;
    } catch (...) {
        if (error_out) {
            *error_out = "unknown write failure";
        }
        return false;
    }
}

nlohmann::json slowest_stage(const nlohmann::json& stages)
{
    nlohmann::json result = nullptr;
    double maximum_ms = -1.0;
    if (!stages.is_object()) {
        return result;
    }
    for (auto it = stages.begin(); it != stages.end(); ++it) {
        if (!it.value().is_object()) {
            continue;
        }
        const double duration_ms = it.value().value("duration_ms", -1.0);
        if (duration_ms > maximum_ms) {
            maximum_ms = duration_ms;
            result = {
                {"stage", it.key()},
                {"duration_ms", duration_ms},
            };
        }
    }
    return result;
}

}  // namespace

uint64_t GuiStartupTimingRecorder::NowNs() noexcept
{
    try {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    } catch (...) {
        return 0;
    }
}

void GuiStartupTimingRecorder::Begin(
    const std::string& operation,
    const std::filesystem::path& artifact_directory,
    const std::vector<GuiStartupTimingCamera>& cameras,
    const nlohmann::json& context,
    uint64_t request_started_ns) noexcept
{
    try {
        const uint64_t now_ns = request_started_ns > 0 ? request_started_ns : NowNs();
        const uint64_t sequence =
            g_startup_timing_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::string identifier =
            operation + "_" + utc_timestamp("%Y%m%dT%H%M%SZ") + "_" +
            std::to_string(getpid()) + "_" + std::to_string(sequence);
        const std::filesystem::path path = artifact_directory / (identifier + ".json");

        std::lock_guard<std::mutex> lock(mutex_);
        request_started_ns_ = now_ns;
        artifact_path_ = path;
        version_ = 1;
        flushed_version_ = 0;
        last_write_error_version_ = 0;
        active_ = true;
        final_summary_logged_ = false;
        report_ = {
            {"schema_id", "orange.gui.camera_startup_timing"},
            {"schema_version", 1},
            {"transaction_id", identifier},
            {"operation", operation},
            {"status", "in_progress"},
            {"started_at_utc", utc_timestamp("%Y-%m-%dT%H:%M:%SZ")},
            {"clock", {
                {"source", "std::chrono::steady_clock"},
                {"semantics", "offsets_from_request_start"},
            }},
            {"process", {
                {"pid", static_cast<int64_t>(getpid())},
                {"request_thread_id_hash", thread_id_hash()},
            }},
            {"artifact_path", path.string()},
            {"context", context.is_null() ? nlohmann::json::object() : context},
            {"global_stages", nlohmann::json::object()},
            {"global_instants", nlohmann::json::object()},
            {"cameras", nlohmann::json::object()},
            {"camera_order", nlohmann::json::array()},
            {"expected_first_frame_camera_count", cameras.size()},
            {"observed_first_frame_camera_count", 0},
        };
        for (const GuiStartupTimingCamera& camera : cameras) {
            if (camera.serial.empty()) {
                continue;
            }
            report_["camera_order"].push_back(camera.serial);
            report_["cameras"][camera.serial] = {
                {"camera_index", camera.camera_index},
                {"gpu_id", camera.gpu_id},
                {"context", camera.context.is_null()
                                ? nlohmann::json::object()
                                : camera.context},
                {"stages", nlohmann::json::object()},
                {"instants", nlohmann::json::object()},
                {"first_frame_received", false},
            };
        }
        report_["expected_first_frame_camera_count"] =
            report_["cameras"].size();
        RefreshDerivedLocked(now_ns);
    } catch (const std::exception& error) {
        std::cerr << "[GUI][startup_timing] Begin failed open: "
                  << error.what() << std::endl;
    } catch (...) {
        std::cerr << "[GUI][startup_timing] Begin failed with unknown error"
                  << std::endl;
    }
}

double GuiStartupTimingRecorder::OffsetMsLocked(const uint64_t at_ns) const
{
    if (request_started_ns_ == 0 || at_ns <= request_started_ns_) {
        return 0.0;
    }
    return static_cast<double>(at_ns - request_started_ns_) / 1.0e6;
}

void GuiStartupTimingRecorder::RecordIntervalLocked(
    nlohmann::json* destination,
    const std::string& stage,
    uint64_t started_ns,
    uint64_t finished_ns,
    const bool completed_normally)
{
    if (!destination || stage.empty()) {
        return;
    }
    if (finished_ns < started_ns) {
        finished_ns = started_ns;
    }
    (*destination)[stage] = {
        {"start_offset_ms", OffsetMsLocked(started_ns)},
        {"end_offset_ms", OffsetMsLocked(finished_ns)},
        {"duration_ms", static_cast<double>(finished_ns - started_ns) / 1.0e6},
        {"outcome", completed_normally ? "completed" : "exception"},
        {"thread_id_hash", thread_id_hash()},
    };
    ++version_;
}

void GuiStartupTimingRecorder::RecordGlobalInterval(
    const std::string& stage,
    const uint64_t started_ns,
    const uint64_t finished_ns,
    const bool completed_normally) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) {
            return;
        }
        RecordIntervalLocked(
            &report_["global_stages"], stage, started_ns, finished_ns,
            completed_normally);
        RefreshDerivedLocked(finished_ns);
    } catch (...) {
    }
}

void GuiStartupTimingRecorder::RecordCameraInterval(
    const std::string& camera_serial,
    const std::string& stage,
    const uint64_t started_ns,
    const uint64_t finished_ns,
    const bool completed_normally) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || !report_["cameras"].contains(camera_serial)) {
            return;
        }
        RecordIntervalLocked(
            &report_["cameras"][camera_serial]["stages"], stage,
            started_ns, finished_ns, completed_normally);
        RefreshDerivedLocked(finished_ns);
    } catch (...) {
    }
}

void GuiStartupTimingRecorder::MarkInstantLocked(
    nlohmann::json* destination,
    const std::string& event,
    const uint64_t at_ns,
    const nlohmann::json& details)
{
    if (!destination || event.empty()) {
        return;
    }
    nlohmann::json value = {
        {"offset_ms", OffsetMsLocked(at_ns)},
        {"thread_id_hash", thread_id_hash()},
    };
    if (details.is_object() && !details.empty()) {
        value["details"] = details;
    }
    (*destination)[event] = std::move(value);
    ++version_;
}

void GuiStartupTimingRecorder::MarkGlobalInstant(
    const std::string& event,
    uint64_t at_ns,
    const nlohmann::json& details) noexcept
{
    try {
        if (at_ns == 0) {
            at_ns = NowNs();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) {
            return;
        }
        MarkInstantLocked(&report_["global_instants"], event, at_ns, details);
        RefreshDerivedLocked(at_ns);
    } catch (...) {
    }
}

void GuiStartupTimingRecorder::MarkCameraInstant(
    const std::string& camera_serial,
    const std::string& event,
    uint64_t at_ns,
    const nlohmann::json& details) noexcept
{
    try {
        if (at_ns == 0) {
            at_ns = NowNs();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || !report_["cameras"].contains(camera_serial)) {
            return;
        }
        MarkInstantLocked(
            &report_["cameras"][camera_serial]["instants"], event,
            at_ns, details);
        RefreshDerivedLocked(at_ns);
    } catch (...) {
    }
}

void GuiStartupTimingRecorder::MarkHandlerComplete(uint64_t at_ns) noexcept
{
    try {
        if (at_ns == 0) {
            at_ns = NowNs();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (report_.empty()) {
            return;
        }
        report_["gui_handler_complete"] = true;
        report_["gui_handler_duration_ms"] = OffsetMsLocked(at_ns);
        if (!terminal_status(report_.value("status", std::string())) &&
            report_.value("operation", std::string()) == "stream_start") {
            const std::size_t observed = report_.value(
                "observed_first_frame_camera_count", std::size_t{0});
            const std::size_t expected = report_.value(
                "expected_first_frame_camera_count", std::size_t{0});
            if (expected == 0) {
                report_["status"] = "complete";
                report_["completed_offset_ms"] = OffsetMsLocked(at_ns);
                report_["time_to_all_first_frames_ms"] = OffsetMsLocked(at_ns);
                active_ = false;
            } else if (observed < expected) {
                report_["status"] = "awaiting_first_frames";
            }
        }
        ++version_;
        RefreshDerivedLocked(at_ns);
    } catch (...) {
    }
}

void GuiStartupTimingRecorder::MarkOperationComplete(uint64_t at_ns) noexcept
{
    try {
        if (at_ns == 0) {
            at_ns = NowNs();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) {
            return;
        }
        report_["status"] = "complete";
        report_["completed_offset_ms"] = OffsetMsLocked(at_ns);
        active_ = false;
        ++version_;
        RefreshDerivedLocked(at_ns);
    } catch (...) {
    }
}

void GuiStartupTimingRecorder::MarkFailed(
    const std::string& reason,
    uint64_t at_ns) noexcept
{
    try {
        if (at_ns == 0) {
            at_ns = NowNs();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (report_.empty()) {
            return;
        }
        report_["status"] = "failed";
        report_["failure_reason"] = reason;
        report_["completed_offset_ms"] = OffsetMsLocked(at_ns);
        active_ = false;
        ++version_;
        RefreshDerivedLocked(at_ns);
    } catch (...) {
    }
}

void GuiStartupTimingRecorder::MarkStopped(
    const std::string& reason,
    uint64_t at_ns) noexcept
{
    try {
        if (at_ns == 0) {
            at_ns = NowNs();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (report_.empty() || terminal_status(report_.value("status", std::string()))) {
            return;
        }
        report_["status"] = "stopped";
        report_["stop_reason"] = reason;
        report_["completed_offset_ms"] = OffsetMsLocked(at_ns);
        active_ = false;
        ++version_;
        RefreshDerivedLocked(at_ns);
    } catch (...) {
    }
}

void GuiStartupTimingRecorder::MarkFirstFrame(
    const std::string& camera_serial,
    const uint64_t local_frame_id,
    const uint64_t camera_frame_id,
    const uint64_t camera_timestamp_ns,
    const uint64_t receive_host_ns) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || report_.value("operation", std::string()) != "stream_start" ||
            !report_["cameras"].contains(camera_serial)) {
            return;
        }
        nlohmann::json& camera = report_["cameras"][camera_serial];
        if (camera.value("first_frame_received", false)) {
            return;
        }
        camera["first_frame_received"] = true;
        camera["first_frame"] = {
            {"offset_ms", OffsetMsLocked(receive_host_ns)},
            {"local_frame_id", local_frame_id},
            {"camera_frame_id", camera_frame_id},
            {"camera_timestamp_ns", camera_timestamp_ns},
            {"receive_host_ns", receive_host_ns},
            {"thread_id_hash", thread_id_hash()},
        };
        const std::size_t observed =
            report_.value("observed_first_frame_camera_count", std::size_t{0}) + 1;
        report_["observed_first_frame_camera_count"] = observed;
        ++version_;

        const std::size_t expected =
            report_.value("expected_first_frame_camera_count", std::size_t{0});
        if (expected > 0 && observed >= expected) {
            report_["status"] = "complete";
            report_["completed_offset_ms"] = OffsetMsLocked(receive_host_ns);
            report_["time_to_all_first_frames_ms"] = OffsetMsLocked(receive_host_ns);
            active_ = false;
        }
        RefreshDerivedLocked(receive_host_ns);
    } catch (...) {
    }
}

void GuiStartupTimingRecorder::RefreshDerivedLocked(const uint64_t at_ns)
{
    report_["last_update_offset_ms"] = OffsetMsLocked(at_ns);
    report_["slowest_global_stage"] = slowest_stage(report_["global_stages"]);

    double first_frame_min_ms = std::numeric_limits<double>::max();
    double first_frame_max_ms = -1.0;
    nlohmann::json slowest_camera = nullptr;
    double slowest_camera_ms = -1.0;
    for (auto it = report_["cameras"].begin(); it != report_["cameras"].end(); ++it) {
        nlohmann::json& camera = it.value();
        camera["slowest_stage"] = slowest_stage(camera["stages"]);
        if (camera["slowest_stage"].is_object()) {
            const double duration = camera["slowest_stage"].value("duration_ms", -1.0);
            if (duration > slowest_camera_ms) {
                slowest_camera_ms = duration;
                slowest_camera = camera["slowest_stage"];
                slowest_camera["camera_serial"] = it.key();
            }
        }
        if (camera.value("first_frame_received", false) &&
            camera.contains("first_frame")) {
            const double offset = camera["first_frame"].value("offset_ms", -1.0);
            if (offset >= 0.0) {
                first_frame_min_ms = std::min(first_frame_min_ms, offset);
                first_frame_max_ms = std::max(first_frame_max_ms, offset);
            }
        }
    }
    report_["slowest_camera_stage"] = slowest_camera;
    if (first_frame_max_ms >= 0.0 && first_frame_min_ms != std::numeric_limits<double>::max()) {
        report_["first_frame_spread_ms"] = first_frame_max_ms - first_frame_min_ms;
    }
}

nlohmann::json GuiStartupTimingRecorder::Snapshot() const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return report_;
    } catch (...) {
        return nlohmann::json::object();
    }
}

GuiStartupTimingStatus GuiStartupTimingRecorder::Status() const noexcept
{
    GuiStartupTimingStatus status;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (report_.empty()) {
            return status;
        }
        status.available = true;
        status.transaction_id =
            report_.value("transaction_id", std::string());
        status.operation = report_.value("operation", std::string());
        status.status = report_.value("status", std::string());
        status.failure_reason =
            report_.value("failure_reason", std::string());
        status.stop_reason = report_.value("stop_reason", std::string());
        status.artifact_path = artifact_path_.string();
        status.expected_first_frame_camera_count = report_.value(
            "expected_first_frame_camera_count", std::size_t{0});
        status.observed_first_frame_camera_count = report_.value(
            "observed_first_frame_camera_count", std::size_t{0});
    } catch (...) {
        return GuiStartupTimingStatus{};
    }
    return status;
}

std::filesystem::path GuiStartupTimingRecorder::artifact_path() const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return artifact_path_;
    } catch (...) {
        return {};
    }
}

void GuiStartupTimingRecorder::FlushPending() noexcept
{
    nlohmann::json snapshot;
    std::filesystem::path path;
    uint64_t snapshot_version = 0;
    bool should_log_final = false;
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (report_.empty() || artifact_path_.empty() || version_ == flushed_version_) {
                return;
            }
            snapshot = report_;
            path = artifact_path_;
            snapshot_version = version_;
            should_log_final =
                terminal_status(snapshot.value("status", std::string())) &&
                !final_summary_logged_;
        }

        std::string error;
        if (!write_json_atomic(path, snapshot, &error)) {
            bool log_error = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (last_write_error_version_ != snapshot_version) {
                    last_write_error_version_ = snapshot_version;
                    log_error = true;
                }
            }
            if (log_error) {
                std::cerr << "[GUI][startup_timing] failed to write "
                          << path << ": " << error << std::endl;
            }
            return;
        }

        bool emit_summary = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            flushed_version_ = std::max(flushed_version_, snapshot_version);
            if (should_log_final && !final_summary_logged_ &&
                artifact_path_ == path) {
                final_summary_logged_ = true;
                emit_summary = true;
            }
        }
        if (emit_summary) {
            const nlohmann::json summary = {
                {"transaction_id", snapshot.value("transaction_id", std::string())},
                {"operation", snapshot.value("operation", std::string())},
                {"status", snapshot.value("status", std::string())},
                {"gui_handler_duration_ms", snapshot.value("gui_handler_duration_ms", 0.0)},
                {"time_to_all_first_frames_ms", snapshot.value("time_to_all_first_frames_ms", 0.0)},
                {"first_frame_spread_ms", snapshot.value("first_frame_spread_ms", 0.0)},
                {"slowest_global_stage", snapshot.value("slowest_global_stage", nlohmann::json(nullptr))},
                {"slowest_camera_stage", snapshot.value("slowest_camera_stage", nlohmann::json(nullptr))},
                {"artifact_path", path.string()},
            };
            std::cout << "[GUI][startup_timing][summary] " << summary.dump()
                      << std::endl;
        }
    } catch (...) {
    }
}

GuiStartupTimingScope::GuiStartupTimingScope(
    GuiStartupTimingRecorder* recorder,
    std::string stage,
    std::string camera_serial) noexcept
    : recorder_(recorder),
      stage_(std::move(stage)),
      camera_serial_(std::move(camera_serial)),
      started_ns_(GuiStartupTimingRecorder::NowNs()),
      uncaught_exceptions_at_start_(std::uncaught_exceptions())
{
}

GuiStartupTimingScope::~GuiStartupTimingScope() noexcept
{
    Finish();
}

void GuiStartupTimingScope::Finish() noexcept
{
    if (finished_) {
        return;
    }
    finished_ = true;
    if (!recorder_) {
        return;
    }
    const uint64_t finished_ns = GuiStartupTimingRecorder::NowNs();
    const bool completed_normally =
        std::uncaught_exceptions() <= uncaught_exceptions_at_start_;
    if (camera_serial_.empty()) {
        recorder_->RecordGlobalInterval(
            stage_, started_ns_, finished_ns, completed_normally);
    } else {
        recorder_->RecordCameraInterval(
            camera_serial_, stage_, started_ns_, finished_ns,
            completed_normally);
    }
}

}  // namespace orange::gui
