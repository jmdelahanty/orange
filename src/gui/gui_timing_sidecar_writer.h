#pragma once

#include "gui_display_frame_rate.h"
#include "json.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace orange::gui {

struct GuiTimingSidecarOptions {
    std::chrono::milliseconds aggregation_interval{1000};
    std::size_t queue_capacity = 256;
};

// Recording-scoped chronological GUI timing telemetry. The GUI thread only
// updates one bounded one-second accumulator and performs a non-blocking queue
// offer at each boundary. A dedicated writer thread owns all CSV I/O.
//
// This sidecar complements (and does not replace) GuiDisplayFrameRateStats:
// the latter gives compact whole-recording statistics, while this artifact
// preserves when a slowdown occurred without retaining every GUI frame.
class GuiTimingSidecarWriter {
public:
    explicit GuiTimingSidecarWriter(
        GuiTimingSidecarOptions options = GuiTimingSidecarOptions{});
    ~GuiTimingSidecarWriter();

    GuiTimingSidecarWriter(const GuiTimingSidecarWriter&) = delete;
    GuiTimingSidecarWriter& operator=(const GuiTimingSidecarWriter&) = delete;

    bool Start(
        const std::filesystem::path& recording_folder,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now()) noexcept;

    void Observe(
        double delta_time_s,
        const GuiFrameTimingSample& sample,
        bool crop_recording_enabled,
        bool crop_preview_visible,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now()) noexcept;

    // Seals the partial final interval and joins the writer. Idempotent.
    void StopAndDrain(
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now()) noexcept;

    bool HasSessionForFolder(
        const std::filesystem::path& recording_folder) const noexcept;
    bool accepting() const noexcept;

    // Safe after StopAndDrain. During a recording it returns a provisional
    // status without attempting to hash a file still being written.
    nlohmann::json ArtifactJson() const noexcept;

private:
    struct WindowAccumulator;
    struct QueuedRow;

    void SealCurrentWindow(
        std::chrono::steady_clock::time_point end_time) noexcept;
    void Enqueue(QueuedRow row) noexcept;
    void WriterMain() noexcept;
    void ResetSessionState() noexcept;

    GuiTimingSidecarOptions options_;
    std::filesystem::path recording_folder_;
    std::filesystem::path artifact_path_;
    std::chrono::steady_clock::time_point session_started_at_{};
    std::chrono::system_clock::time_point session_started_wall_time_{};
    std::uint64_t current_window_index_ = 0;
    bool session_initialized_ = false;
    bool window_active_ = false;
    WindowAccumulator* current_window_ = nullptr;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueuedRow> queue_;
    std::thread writer_thread_;
    std::atomic<bool> accepting_{false};
    bool stop_requested_ = false;
    bool writer_finished_ = false;
    std::string writer_error_;
    std::uint64_t windows_offered_ = 0;
    std::uint64_t windows_written_ = 0;
    std::uint64_t windows_dropped_ = 0;
    std::uint64_t samples_offered_ = 0;
    std::uint64_t samples_written_ = 0;
    std::uint64_t samples_dropped_ = 0;
    std::size_t queue_high_water_ = 0;
    std::uint64_t first_window_written_ = 0;
    std::uint64_t last_window_written_ = 0;
    bool has_written_window_ = false;
    std::uintmax_t artifact_size_bytes_ = 0;
    std::string artifact_sha256_;
};

}  // namespace orange::gui
