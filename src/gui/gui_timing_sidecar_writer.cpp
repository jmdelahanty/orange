#include "gui/gui_timing_sidecar_writer.h"

#include "bounded_sample_statistics.h"
#include "fsuid_guard.h"
#include "gui/spatial_layout/sha256.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace orange::gui {
namespace {

constexpr const char* kSchemaId = "orange.gui_timing_windows";
constexpr int kSchemaVersion = 1;
constexpr std::size_t kWindowMaxRetainedSamples = 2048;

struct MetricSummary {
    std::uint64_t sample_count = 0;
    bool percentiles_exact = true;
    double minimum = 0.0;
    double p05 = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
};

MetricSummary summarize(const orange::BoundedSampleStatistics& samples)
{
    MetricSummary result;
    result.sample_count = samples.sample_count();
    result.percentiles_exact = samples.percentiles_exact();
    if (samples.empty()) {
        return result;
    }
    const std::vector<double> sorted = samples.sorted_retained_samples();
    result.minimum = samples.min();
    result.p05 = percentile_from_sorted_samples(sorted, 0.05);
    result.p50 = percentile_from_sorted_samples(sorted, 0.50);
    result.p95 = percentile_from_sorted_samples(sorted, 0.95);
    result.maximum = samples.max();
    result.mean = samples.mean();
    return result;
}

void append_metric_header(std::ostringstream* out, const char* name)
{
    *out << ',' << name << "_sample_count"
         << ',' << name << "_percentiles_exact"
         << ',' << name << "_min"
         << ',' << name << "_p05"
         << ',' << name << "_p50"
         << ',' << name << "_p95"
         << ',' << name << "_max"
         << ',' << name << "_mean";
}

void append_metric(std::ostringstream* out, const MetricSummary& value)
{
    *out << ',' << value.sample_count
         << ',' << (value.percentiles_exact ? 1 : 0)
         << ',' << value.minimum
         << ',' << value.p05
         << ',' << value.p50
         << ',' << value.p95
         << ',' << value.maximum
         << ',' << value.mean;
}

std::string csv_header()
{
    std::ostringstream out;
    out << "schema_id,schema_version,window_index,window_start_offset_ms,"
           "window_end_offset_ms,first_sample_offset_ms,last_sample_offset_ms,"
           "gui_frame_count,crop_recording_enabled_frame_count,"
           "crop_preview_visible_frame_count,main_texture_upload_count,"
           "crop_texture_upload_count";
    append_metric_header(&out, "fps");
    append_metric_header(&out, "frame_total_ms");
    append_metric_header(&out, "pre_frame_maintenance_ms");
    append_metric_header(&out, "imgui_new_frame_ms");
    append_metric_header(&out, "orange_window_draw_ms");
    append_metric_header(&out, "recording_panel_draw_ms");
    append_metric_header(&out, "camera_properties_draw_ms");
    append_metric_header(&out, "main_texture_upload_ms");
    append_metric_header(&out, "crop_texture_upload_ms");
    append_metric_header(&out, "camera_window_draw_ms");
    append_metric_header(&out, "crop_window_draw_ms");
    append_metric_header(&out, "speed_graph_draw_ms");
    append_metric_header(&out, "render_present_ms");
    out << '\n';
    return out.str();
}

double duration_ms(
    const std::chrono::steady_clock::time_point begin,
    const std::chrono::steady_clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

std::uint64_t posix_time_ns(
    const std::chrono::system_clock::time_point time)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::nanoseconds>(time.time_since_epoch()).count());
}

}  // namespace

struct GuiTimingSidecarWriter::WindowAccumulator {
    explicit WindowAccumulator(const std::uint64_t index)
        : window_index(index),
          fps(kWindowMaxRetainedSamples),
          frame_total_ms(kWindowMaxRetainedSamples),
          pre_frame_maintenance_ms(kWindowMaxRetainedSamples),
          imgui_new_frame_ms(kWindowMaxRetainedSamples),
          orange_window_draw_ms(kWindowMaxRetainedSamples),
          recording_panel_draw_ms(kWindowMaxRetainedSamples),
          camera_properties_draw_ms(kWindowMaxRetainedSamples),
          main_texture_upload_ms(kWindowMaxRetainedSamples),
          crop_texture_upload_ms(kWindowMaxRetainedSamples),
          camera_window_draw_ms(kWindowMaxRetainedSamples),
          crop_window_draw_ms(kWindowMaxRetainedSamples),
          speed_graph_draw_ms(kWindowMaxRetainedSamples),
          render_present_ms(kWindowMaxRetainedSamples)
    {
    }

    void Reset(const std::uint64_t index)
    {
        window_index = index;
        gui_frame_count = 0;
        crop_recording_enabled_frame_count = 0;
        crop_preview_visible_frame_count = 0;
        main_texture_upload_count = 0;
        crop_texture_upload_count = 0;
        first_sample_offset_ms = 0.0;
        last_sample_offset_ms = 0.0;
        has_sample = false;
        fps.Reset();
        frame_total_ms.Reset();
        pre_frame_maintenance_ms.Reset();
        imgui_new_frame_ms.Reset();
        orange_window_draw_ms.Reset();
        recording_panel_draw_ms.Reset();
        camera_properties_draw_ms.Reset();
        main_texture_upload_ms.Reset();
        crop_texture_upload_ms.Reset();
        camera_window_draw_ms.Reset();
        crop_window_draw_ms.Reset();
        speed_graph_draw_ms.Reset();
        render_present_ms.Reset();
    }

    std::uint64_t window_index = 0;
    std::uint64_t gui_frame_count = 0;
    std::uint64_t crop_recording_enabled_frame_count = 0;
    std::uint64_t crop_preview_visible_frame_count = 0;
    std::uint64_t main_texture_upload_count = 0;
    std::uint64_t crop_texture_upload_count = 0;
    double first_sample_offset_ms = 0.0;
    double last_sample_offset_ms = 0.0;
    bool has_sample = false;
    orange::BoundedSampleStatistics fps;
    orange::BoundedSampleStatistics frame_total_ms;
    orange::BoundedSampleStatistics pre_frame_maintenance_ms;
    orange::BoundedSampleStatistics imgui_new_frame_ms;
    orange::BoundedSampleStatistics orange_window_draw_ms;
    orange::BoundedSampleStatistics recording_panel_draw_ms;
    orange::BoundedSampleStatistics camera_properties_draw_ms;
    orange::BoundedSampleStatistics main_texture_upload_ms;
    orange::BoundedSampleStatistics crop_texture_upload_ms;
    orange::BoundedSampleStatistics camera_window_draw_ms;
    orange::BoundedSampleStatistics crop_window_draw_ms;
    orange::BoundedSampleStatistics speed_graph_draw_ms;
    orange::BoundedSampleStatistics render_present_ms;
};

struct GuiTimingSidecarWriter::QueuedRow {
    std::uint64_t window_index = 0;
    std::uint64_t sample_count = 0;
    std::string csv;
};

GuiTimingSidecarWriter::GuiTimingSidecarWriter(GuiTimingSidecarOptions options)
    : options_(std::move(options))
{
    if (options_.aggregation_interval <= std::chrono::milliseconds(0)) {
        options_.aggregation_interval = std::chrono::milliseconds(1000);
    }
    if (options_.queue_capacity == 0) {
        options_.queue_capacity = 1;
    }
}

GuiTimingSidecarWriter::~GuiTimingSidecarWriter()
{
    StopAndDrain();
    delete current_window_;
}

void GuiTimingSidecarWriter::ResetSessionState() noexcept
{
    delete current_window_;
    current_window_ = nullptr;
    window_active_ = false;
    current_window_index_ = 0;
    session_initialized_ = false;
    recording_folder_.clear();
    artifact_path_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    stop_requested_ = false;
    writer_finished_ = false;
    writer_error_.clear();
    windows_offered_ = 0;
    windows_written_ = 0;
    windows_dropped_ = 0;
    samples_offered_ = 0;
    samples_written_ = 0;
    samples_dropped_ = 0;
    queue_high_water_ = 0;
    first_window_written_ = 0;
    last_window_written_ = 0;
    has_written_window_ = false;
    artifact_size_bytes_ = 0;
    artifact_sha256_.clear();
}

bool GuiTimingSidecarWriter::Start(
    const std::filesystem::path& recording_folder,
    const std::chrono::steady_clock::time_point now) noexcept
{
    try {
        if (session_initialized_ && recording_folder_ == recording_folder) {
            return accepting_.load(std::memory_order_acquire);
        }
        StopAndDrain(now);
        ResetSessionState();
        session_initialized_ = true;
        recording_folder_ = recording_folder.lexically_normal();
        artifact_path_ = recording_folder_ / "gui_timing_windows.csv";
        session_started_at_ = now;
        const auto start_call_steady = std::chrono::steady_clock::now();
        const auto start_call_wall = std::chrono::system_clock::now();
        session_started_wall_time_ = now <= start_call_steady
            ? start_call_wall - std::chrono::duration_cast<
                  std::chrono::system_clock::duration>(start_call_steady - now)
            : start_call_wall;
        if (recording_folder_.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            writer_error_ = "recording folder is empty";
            writer_finished_ = true;
            return false;
        }
        accepting_.store(true, std::memory_order_release);
        writer_thread_ = std::thread(&GuiTimingSidecarWriter::WriterMain, this);
        return true;
    } catch (const std::exception& error) {
        accepting_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        writer_error_ = error.what();
        writer_finished_ = true;
        return false;
    } catch (...) {
        accepting_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        writer_error_ = "unknown sidecar start failure";
        writer_finished_ = true;
        return false;
    }
}

void GuiTimingSidecarWriter::Observe(
    const double delta_time_s,
    const GuiFrameTimingSample& sample,
    const bool crop_recording_enabled,
    const bool crop_preview_visible,
    const std::chrono::steady_clock::time_point now) noexcept
{
    try {
        if (!accepting_.load(std::memory_order_acquire) ||
            !session_initialized_ || now < session_started_at_) {
            return;
        }
        const auto elapsed = now - session_started_at_;
        const auto interval_ns = std::chrono::duration_cast<
            std::chrono::nanoseconds>(options_.aggregation_interval).count();
        const auto elapsed_ns = std::chrono::duration_cast<
            std::chrono::nanoseconds>(elapsed).count();
        const std::uint64_t window_index = static_cast<std::uint64_t>(
            elapsed_ns / std::max<std::int64_t>(interval_ns, 1));
        if (!window_active_ || !current_window_) {
            current_window_index_ = window_index;
            if (!current_window_) {
                current_window_ = new WindowAccumulator(window_index);
            } else {
                current_window_->Reset(window_index);
            }
            window_active_ = true;
        } else if (window_index != current_window_index_) {
            const auto boundary = session_started_at_ +
                options_.aggregation_interval *
                    static_cast<std::int64_t>(current_window_index_ + 1);
            SealCurrentWindow(boundary);
            current_window_index_ = window_index;
            current_window_->Reset(window_index);
            window_active_ = true;
        }

        WindowAccumulator& window = *current_window_;
        const double sample_offset_ms = duration_ms(session_started_at_, now);
        if (!window.has_sample) {
            window.first_sample_offset_ms = sample_offset_ms;
            window.has_sample = true;
        }
        window.last_sample_offset_ms = sample_offset_ms;
        ++window.gui_frame_count;
        if (crop_recording_enabled) {
            ++window.crop_recording_enabled_frame_count;
        }
        if (crop_recording_enabled && crop_preview_visible) {
            ++window.crop_preview_visible_frame_count;
        }
        if (std::isfinite(delta_time_s) && delta_time_s > 0.0 &&
            delta_time_s < 60.0) {
            window.fps.Add(1.0 / delta_time_s);
        }
        const auto add_duration = [](orange::BoundedSampleStatistics* stats,
                                     const double value) {
            if (stats && std::isfinite(value) && value >= 0.0 && value < 60000.0) {
                stats->Add(value);
            }
        };
        add_duration(&window.frame_total_ms, sample.frame_total_ms);
        add_duration(&window.pre_frame_maintenance_ms,
                     sample.pre_frame_maintenance_ms);
        add_duration(&window.imgui_new_frame_ms, sample.imgui_new_frame_ms);
        add_duration(&window.orange_window_draw_ms,
                     sample.orange_window_draw_ms);
        add_duration(&window.recording_panel_draw_ms,
                     sample.recording_panel_draw_ms);
        add_duration(&window.camera_properties_draw_ms,
                     sample.camera_properties_draw_ms);
        add_duration(&window.main_texture_upload_ms,
                     sample.main_texture_upload_ms);
        add_duration(&window.crop_texture_upload_ms,
                     sample.crop_texture_upload_ms);
        add_duration(&window.camera_window_draw_ms,
                     sample.camera_window_draw_ms);
        add_duration(&window.crop_window_draw_ms,
                     sample.crop_window_draw_ms);
        add_duration(&window.speed_graph_draw_ms,
                     sample.speed_graph_draw_ms);
        add_duration(&window.render_present_ms, sample.render_present_ms);
        window.main_texture_upload_count += sample.main_texture_upload_count;
        window.crop_texture_upload_count += sample.crop_texture_upload_count;
    } catch (...) {
        // Diagnostics are fail-open: acquisition and recording must continue.
    }
}

void GuiTimingSidecarWriter::SealCurrentWindow(
    const std::chrono::steady_clock::time_point end_time) noexcept
{
    if (!window_active_ || !current_window_) {
        return;
    }
    WindowAccumulator* window = current_window_;
    window_active_ = false;
    if (!window->has_sample) {
        return;
    }

    try {
        const double interval_ms =
            static_cast<double>(options_.aggregation_interval.count());
        const double start_offset_ms =
            static_cast<double>(window->window_index) * interval_ms;
        const double nominal_end_ms = start_offset_ms + interval_ms;
        const double observed_end_ms = duration_ms(session_started_at_, end_time);
        const double end_offset_ms = std::min(nominal_end_ms,
                                              std::max(start_offset_ms,
                                                       observed_end_ms));

        std::ostringstream out;
        out << std::setprecision(std::numeric_limits<double>::max_digits10)
            << kSchemaId << ',' << kSchemaVersion << ','
            << window->window_index << ',' << start_offset_ms << ','
            << end_offset_ms << ',' << window->first_sample_offset_ms << ','
            << window->last_sample_offset_ms << ',' << window->gui_frame_count
            << ',' << window->crop_recording_enabled_frame_count
            << ',' << window->crop_preview_visible_frame_count
            << ',' << window->main_texture_upload_count
            << ',' << window->crop_texture_upload_count;
        append_metric(&out, summarize(window->fps));
        append_metric(&out, summarize(window->frame_total_ms));
        append_metric(&out, summarize(window->pre_frame_maintenance_ms));
        append_metric(&out, summarize(window->imgui_new_frame_ms));
        append_metric(&out, summarize(window->orange_window_draw_ms));
        append_metric(&out, summarize(window->recording_panel_draw_ms));
        append_metric(&out, summarize(window->camera_properties_draw_ms));
        append_metric(&out, summarize(window->main_texture_upload_ms));
        append_metric(&out, summarize(window->crop_texture_upload_ms));
        append_metric(&out, summarize(window->camera_window_draw_ms));
        append_metric(&out, summarize(window->crop_window_draw_ms));
        append_metric(&out, summarize(window->speed_graph_draw_ms));
        append_metric(&out, summarize(window->render_present_ms));
        out << '\n';
        Enqueue(QueuedRow{window->window_index, window->gui_frame_count, out.str()});
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++windows_offered_;
        ++windows_dropped_;
        samples_offered_ += window->gui_frame_count;
        samples_dropped_ += window->gui_frame_count;
        if (writer_error_.empty()) {
            writer_error_ = "failed to serialize GUI timing window";
        }
    }
}

void GuiTimingSidecarWriter::Enqueue(QueuedRow row) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++windows_offered_;
    samples_offered_ += row.sample_count;
    if (!accepting_.load(std::memory_order_acquire) ||
        queue_.size() >= options_.queue_capacity) {
        ++windows_dropped_;
        samples_dropped_ += row.sample_count;
        return;
    }
    queue_.push_back(std::move(row));
    queue_high_water_ = std::max(queue_high_water_, queue_.size());
    condition_.notify_one();
}

void GuiTimingSidecarWriter::WriterMain() noexcept
{
    orange::ScopedFsuid user_files;
    (void)user_files;
    std::ofstream output;
    try {
        std::error_code error;
        std::filesystem::create_directories(recording_folder_, error);
        if (error) {
            throw std::runtime_error(
                "create_directories failed: " + error.message());
        }
        output.open(artifact_path_, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            throw std::runtime_error(
                "could not open " + artifact_path_.string());
        }
        spatial_layout::checksum::StreamingSha256 hasher;
        const std::string header = csv_header();
        output << header;
        output.flush();
        if (!output.good()) {
            throw std::runtime_error("could not write GUI timing CSV header");
        }
        hasher.update(header);
        std::uintmax_t size_bytes = header.size();

        while (true) {
            QueuedRow row;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [&]() {
                    return stop_requested_ || !queue_.empty();
                });
                if (queue_.empty()) {
                    if (stop_requested_) {
                        break;
                    }
                    continue;
                }
                row = std::move(queue_.front());
                queue_.pop_front();
            }
            output << row.csv;
            output.flush();
            if (!output.good()) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++windows_dropped_;
                samples_dropped_ += row.sample_count;
                throw std::runtime_error("could not append GUI timing CSV row");
            }
            hasher.update(row.csv);
            size_bytes += row.csv.size();
            std::lock_guard<std::mutex> lock(mutex_);
            ++windows_written_;
            samples_written_ += row.sample_count;
            if (!has_written_window_) {
                first_window_written_ = row.window_index;
                has_written_window_ = true;
            }
            last_window_written_ = row.window_index;
        }
        output.close();
        if (output.fail()) {
            throw std::runtime_error("could not close GUI timing CSV");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        artifact_size_bytes_ = size_bytes;
        artifact_sha256_ = "sha256:" + hasher.final_hex();
    } catch (const std::exception& error) {
        accepting_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        writer_error_ = error.what();
        for (const QueuedRow& row : queue_) {
            samples_dropped_ += row.sample_count;
        }
        windows_dropped_ += queue_.size();
        queue_.clear();
    } catch (...) {
        accepting_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        writer_error_ = "unknown GUI timing sidecar write failure";
        for (const QueuedRow& row : queue_) {
            samples_dropped_ += row.sample_count;
        }
        windows_dropped_ += queue_.size();
        queue_.clear();
    }
    accepting_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex_);
    writer_finished_ = true;
}

void GuiTimingSidecarWriter::StopAndDrain(
    const std::chrono::steady_clock::time_point now) noexcept
{
    try {
        if (!session_initialized_) {
            return;
        }
        if (window_active_) {
            SealCurrentWindow(now);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        condition_.notify_all();
        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }
        accepting_.store(false, std::memory_order_release);
    } catch (...) {
        accepting_.store(false, std::memory_order_release);
        if (writer_thread_.joinable()) {
            try {
                writer_thread_.join();
            } catch (...) {
            }
        }
    }
}

bool GuiTimingSidecarWriter::HasSessionForFolder(
    const std::filesystem::path& recording_folder) const noexcept
{
    try {
        return session_initialized_ &&
            recording_folder_ == recording_folder.lexically_normal();
    } catch (...) {
        return false;
    }
}

bool GuiTimingSidecarWriter::accepting() const noexcept
{
    return accepting_.load(std::memory_order_acquire);
}

nlohmann::json GuiTimingSidecarWriter::ArtifactJson() const noexcept
{
    try {
        std::uint64_t windows_offered = 0;
        std::uint64_t windows_written = 0;
        std::uint64_t windows_dropped = 0;
        std::uint64_t samples_offered = 0;
        std::uint64_t samples_written = 0;
        std::uint64_t samples_dropped = 0;
        std::size_t queue_high_water = 0;
        std::size_t queue_size = 0;
        std::uint64_t first_window = 0;
        std::uint64_t last_window = 0;
        bool has_written_window = false;
        bool writer_finished = false;
        std::string writer_error;
        std::uintmax_t size_bytes = 0;
        std::string sha256;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            windows_offered = windows_offered_;
            windows_written = windows_written_;
            windows_dropped = windows_dropped_;
            samples_offered = samples_offered_;
            samples_written = samples_written_;
            samples_dropped = samples_dropped_;
            queue_high_water = queue_high_water_;
            queue_size = queue_.size();
            first_window = first_window_written_;
            last_window = last_window_written_;
            has_written_window = has_written_window_;
            writer_finished = writer_finished_;
            writer_error = writer_error_;
            size_bytes = artifact_size_bytes_;
            sha256 = artifact_sha256_;
        }

        const bool is_accepting = accepting();
        std::string status = "unavailable";
        if (session_initialized_) {
            status = is_accepting ? "recording" :
                ((!writer_error.empty() ||
                  (writer_finished && sha256.empty()))
                    ? "failed" : (writer_finished ? "complete" : "stopping"));
        }
        nlohmann::json out = {
            {"schema_id", kSchemaId},
            {"schema_version", kSchemaVersion},
            {"status", status},
            {"aggregation_interval_ms", options_.aggregation_interval.count()},
            {"row_granularity", "one_nonempty_fixed_time_window"},
            {"writer_policy", "bounded_nonblocking_background_csv_v1"},
            {"percentile_policy", "exact_up_to_2048_samples_per_window"},
            {"started_at_posix_ns", session_initialized_
                ? posix_time_ns(session_started_wall_time_) : 0},
            {"path", session_initialized_ ? artifact_path_.string() : ""},
            {"relative_path", session_initialized_
                ? artifact_path_.filename().string() : ""},
            {"size_bytes", size_bytes},
            {"sha256", sha256},
            {"windows_offered", windows_offered},
            {"windows_written", windows_written},
            {"windows_dropped", windows_dropped},
            {"samples_offered", samples_offered},
            {"samples_written", samples_written},
            {"samples_dropped", samples_dropped},
            {"window_parity_complete", windows_offered ==
                windows_written + windows_dropped},
            {"sample_parity_complete", samples_offered ==
                samples_written + samples_dropped},
            {"queue_capacity", options_.queue_capacity},
            {"queue_high_water", queue_high_water},
            {"queue_size_at_snapshot", queue_size},
            {"first_window_index", has_written_window
                ? nlohmann::json(first_window) : nlohmann::json(nullptr)},
            {"last_window_index", has_written_window
                ? nlohmann::json(last_window) : nlohmann::json(nullptr)},
            {"writer_error", writer_error},
            {"checksum_error", ""},
        };
        return out;
    } catch (...) {
        return {
            {"schema_id", kSchemaId},
            {"schema_version", kSchemaVersion},
            {"status", "failed"},
            {"writer_error", "could not construct sidecar evidence"},
        };
    }
}

}  // namespace orange::gui
