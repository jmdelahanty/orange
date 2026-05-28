#ifndef ORANGE_GUI_DISPLAY_FRAME_RATE_H
#define ORANGE_GUI_DISPLAY_FRAME_RATE_H

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace orange::gui {

struct GuiFrameRateBucket {
    std::vector<double> samples;

    void Reset()
    {
        samples.clear();
    }

    void Add(const double fps)
    {
        if (std::isfinite(fps) && fps > 0.0 && fps < 10000.0) {
            samples.push_back(fps);
        }
    }
};

struct GuiDurationBucket {
    std::vector<double> samples_ms;

    void Reset()
    {
        samples_ms.clear();
    }

    void Add(const double duration_ms)
    {
        if (std::isfinite(duration_ms) && duration_ms >= 0.0 && duration_ms < 60000.0) {
            samples_ms.push_back(duration_ms);
        }
    }
};

struct GuiFrameTimingSample {
    double frame_total_ms = 0.0;
    double pre_frame_maintenance_ms = 0.0;
    double imgui_new_frame_ms = 0.0;
    double orange_window_draw_ms = 0.0;
    double recording_panel_draw_ms = 0.0;
    double camera_properties_draw_ms = 0.0;
    double main_texture_upload_ms = 0.0;
    double crop_texture_upload_ms = 0.0;
    double camera_window_draw_ms = 0.0;
    double crop_window_draw_ms = 0.0;
    double speed_graph_draw_ms = 0.0;
    double render_present_ms = 0.0;
    uint64_t main_texture_upload_count = 0;
    uint64_t crop_texture_upload_count = 0;
};

struct GuiDisplayFrameRateStats {
    bool recording_active = false;
    bool saw_crop_preview_enabled = false;
    bool saw_crop_preview_hidden = false;
    bool saw_crop_preview_visible = false;
    GuiFrameRateBucket overall;
    GuiFrameRateBucket crop_preview_hidden;
    GuiFrameRateBucket crop_preview_visible;
    GuiDurationBucket frame_total_ms;
    GuiDurationBucket pre_frame_maintenance_ms;
    GuiDurationBucket imgui_new_frame_ms;
    GuiDurationBucket orange_window_draw_ms;
    GuiDurationBucket recording_panel_draw_ms;
    GuiDurationBucket camera_properties_draw_ms;
    GuiDurationBucket main_texture_upload_ms;
    GuiDurationBucket crop_texture_upload_ms;
    GuiDurationBucket camera_window_draw_ms;
    GuiDurationBucket crop_window_draw_ms;
    GuiDurationBucket speed_graph_draw_ms;
    GuiDurationBucket render_present_ms;
    uint64_t main_texture_upload_count = 0;
    uint64_t crop_texture_upload_count = 0;

    void Reset()
    {
        recording_active = true;
        saw_crop_preview_enabled = false;
        saw_crop_preview_hidden = false;
        saw_crop_preview_visible = false;
        overall.Reset();
        crop_preview_hidden.Reset();
        crop_preview_visible.Reset();
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
        main_texture_upload_count = 0;
        crop_texture_upload_count = 0;
    }

    void Finish()
    {
        recording_active = false;
    }
};

inline double gui_fps_percentile(std::vector<double> values, const double percentile)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double clamped_percentile = std::clamp(percentile, 0.0, 100.0);
    if (clamped_percentile <= 0.0) {
        return values.front();
    }
    const std::size_t index = static_cast<std::size_t>(
        std::ceil((clamped_percentile / 100.0) * static_cast<double>(values.size())) - 1.0);
    return values[std::min(index, values.size() - 1)];
}

inline nlohmann::json gui_duration_bucket_json(const GuiDurationBucket& bucket)
{
    nlohmann::json out = {
        {"sample_count", bucket.samples_ms.size()},
        {"min_ms", 0.0},
        {"p05_ms", 0.0},
        {"p50_ms", 0.0},
        {"p95_ms", 0.0},
        {"max_ms", 0.0},
        {"mean_ms", 0.0}
    };
    if (bucket.samples_ms.empty()) {
        return out;
    }

    double sum = 0.0;
    double min_value = bucket.samples_ms.front();
    double max_value = bucket.samples_ms.front();
    for (const double value : bucket.samples_ms) {
        sum += value;
        min_value = std::min(min_value, value);
        max_value = std::max(max_value, value);
    }
    out["min_ms"] = min_value;
    out["p05_ms"] = gui_fps_percentile(bucket.samples_ms, 5.0);
    out["p50_ms"] = gui_fps_percentile(bucket.samples_ms, 50.0);
    out["p95_ms"] = gui_fps_percentile(bucket.samples_ms, 95.0);
    out["max_ms"] = max_value;
    out["mean_ms"] = sum / static_cast<double>(bucket.samples_ms.size());
    return out;
}

inline nlohmann::json gui_frame_rate_bucket_json(const GuiFrameRateBucket& bucket)
{
    nlohmann::json out = {
        {"sample_count", bucket.samples.size()},
        {"min_fps", 0.0},
        {"p05_fps", 0.0},
        {"p50_fps", 0.0},
        {"p95_fps", 0.0},
        {"max_fps", 0.0},
        {"mean_fps", 0.0}
    };
    if (bucket.samples.empty()) {
        return out;
    }

    double sum = 0.0;
    double min_value = bucket.samples.front();
    double max_value = bucket.samples.front();
    for (const double value : bucket.samples) {
        sum += value;
        min_value = std::min(min_value, value);
        max_value = std::max(max_value, value);
    }
    out["min_fps"] = min_value;
    out["p05_fps"] = gui_fps_percentile(bucket.samples, 5.0);
    out["p50_fps"] = gui_fps_percentile(bucket.samples, 50.0);
    out["p95_fps"] = gui_fps_percentile(bucket.samples, 95.0);
    out["max_fps"] = max_value;
    out["mean_fps"] = sum / static_cast<double>(bucket.samples.size());
    return out;
}

inline void gui_sample_display_frame_rate(GuiDisplayFrameRateStats* stats,
                                          const float delta_time_s,
                                          const bool recording_active,
                                          const bool crop_recording_enabled,
                                          const bool crop_preview_windows_visible)
{
    if (!stats || !stats->recording_active || !recording_active || delta_time_s <= 0.0f) {
        return;
    }
    const double fps = 1.0 / static_cast<double>(delta_time_s);
    stats->overall.Add(fps);
    if (!crop_recording_enabled) {
        return;
    }
    stats->saw_crop_preview_enabled = true;
    if (crop_preview_windows_visible) {
        stats->saw_crop_preview_visible = true;
        stats->crop_preview_visible.Add(fps);
    } else {
        stats->saw_crop_preview_hidden = true;
        stats->crop_preview_hidden.Add(fps);
    }
}

inline void gui_sample_frame_timings(GuiDisplayFrameRateStats* stats,
                                     const bool recording_active,
                                     const GuiFrameTimingSample& sample)
{
    if (!stats || !stats->recording_active || !recording_active) {
        return;
    }
    stats->frame_total_ms.Add(sample.frame_total_ms);
    stats->pre_frame_maintenance_ms.Add(sample.pre_frame_maintenance_ms);
    stats->imgui_new_frame_ms.Add(sample.imgui_new_frame_ms);
    stats->orange_window_draw_ms.Add(sample.orange_window_draw_ms);
    stats->recording_panel_draw_ms.Add(sample.recording_panel_draw_ms);
    stats->camera_properties_draw_ms.Add(sample.camera_properties_draw_ms);
    stats->main_texture_upload_ms.Add(sample.main_texture_upload_ms);
    stats->crop_texture_upload_ms.Add(sample.crop_texture_upload_ms);
    stats->camera_window_draw_ms.Add(sample.camera_window_draw_ms);
    stats->crop_window_draw_ms.Add(sample.crop_window_draw_ms);
    stats->speed_graph_draw_ms.Add(sample.speed_graph_draw_ms);
    stats->render_present_ms.Add(sample.render_present_ms);
    stats->main_texture_upload_count += sample.main_texture_upload_count;
    stats->crop_texture_upload_count += sample.crop_texture_upload_count;
}

inline nlohmann::json gui_display_frame_rate_json(const GuiDisplayFrameRateStats& stats,
                                                  const int stream_downsample = 0,
                                                  const int display_preview_max_fps = -1,
                                                  const bool yolo_speed_graphs_enabled = false)
{
    return {
        {"schema_version", 1},
        {"source", "imgui_io_delta_time"},
        {"stream_downsample", stream_downsample},
        {"display_preview_max_fps", display_preview_max_fps},
        {"yolo_speed_graphs_enabled", yolo_speed_graphs_enabled},
        {"saw_crop_preview_enabled", stats.saw_crop_preview_enabled},
        {"saw_crop_preview_hidden", stats.saw_crop_preview_hidden},
        {"saw_crop_preview_visible", stats.saw_crop_preview_visible},
        {"overall", gui_frame_rate_bucket_json(stats.overall)},
        {"crop_preview_hidden", gui_frame_rate_bucket_json(stats.crop_preview_hidden)},
        {"crop_preview_visible", gui_frame_rate_bucket_json(stats.crop_preview_visible)},
        {"timings", {
            {"frame_total_ms", gui_duration_bucket_json(stats.frame_total_ms)},
            {"pre_frame_maintenance_ms", gui_duration_bucket_json(stats.pre_frame_maintenance_ms)},
            {"imgui_new_frame_ms", gui_duration_bucket_json(stats.imgui_new_frame_ms)},
            {"orange_window_draw_ms", gui_duration_bucket_json(stats.orange_window_draw_ms)},
            {"recording_panel_draw_ms", gui_duration_bucket_json(stats.recording_panel_draw_ms)},
            {"camera_properties_draw_ms", gui_duration_bucket_json(stats.camera_properties_draw_ms)},
            {"main_texture_upload_ms", gui_duration_bucket_json(stats.main_texture_upload_ms)},
            {"crop_texture_upload_ms", gui_duration_bucket_json(stats.crop_texture_upload_ms)},
            {"camera_window_draw_ms", gui_duration_bucket_json(stats.camera_window_draw_ms)},
            {"crop_window_draw_ms", gui_duration_bucket_json(stats.crop_window_draw_ms)},
            {"speed_graph_draw_ms", gui_duration_bucket_json(stats.speed_graph_draw_ms)},
            {"render_present_ms", gui_duration_bucket_json(stats.render_present_ms)},
            {"main_texture_upload_count", stats.main_texture_upload_count},
            {"crop_texture_upload_count", stats.crop_texture_upload_count}
        }}
    };
}

}  // namespace orange::gui

#endif  // ORANGE_GUI_DISPLAY_FRAME_RATE_H
