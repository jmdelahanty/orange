#ifndef ORANGE_GUI_DISPLAY_FRAME_RATE_H
#define ORANGE_GUI_DISPLAY_FRAME_RATE_H

#include "bounded_sample_statistics.h"
#include "imgui_glfw_size_cache.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace orange::gui {

struct GuiFrameRateBucket {
    orange::BoundedSampleStatistics samples;

    void Reset()
    {
        samples.Reset();
    }

    void Add(const double fps)
    {
        if (std::isfinite(fps) && fps > 0.0 && fps < 10000.0) {
            samples.Add(fps);
        }
    }
};

struct GuiDurationBucket {
    orange::BoundedSampleStatistics samples_ms;

    void Reset()
    {
        samples_ms.Reset();
    }

    void Add(const double duration_ms)
    {
        if (std::isfinite(duration_ms) && duration_ms >= 0.0 && duration_ms < 60000.0) {
            samples_ms.Add(duration_ms);
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

inline double gui_percentile_from_sorted(
    const std::vector<double>& sorted_values,
    const double percentile)
{
    if (sorted_values.empty()) {
        return 0.0;
    }
    const double clamped_percentile = std::clamp(percentile, 0.0, 100.0);
    if (clamped_percentile <= 0.0) {
        return sorted_values.front();
    }
    const std::size_t index = static_cast<std::size_t>(
        std::ceil((clamped_percentile / 100.0) *
                  static_cast<double>(sorted_values.size())) - 1.0);
    return sorted_values[std::min(index, sorted_values.size() - 1)];
}

inline nlohmann::json gui_duration_bucket_json(const GuiDurationBucket& bucket)
{
    nlohmann::json out = {
        {"sample_count", bucket.samples_ms.sample_count()},
        {"retained_sample_count", bucket.samples_ms.retained_sample_count()},
        {"max_retained_samples", bucket.samples_ms.max_retained_samples()},
        {"percentile_sampling_policy", "deterministic_reservoir_v1"},
        {"percentiles_exact", bucket.samples_ms.percentiles_exact()},
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

    const std::vector<double> sorted_samples =
        bucket.samples_ms.sorted_retained_samples();
    out["min_ms"] = bucket.samples_ms.min();
    out["p05_ms"] = gui_percentile_from_sorted(sorted_samples, 5.0);
    out["p50_ms"] = gui_percentile_from_sorted(sorted_samples, 50.0);
    out["p95_ms"] = gui_percentile_from_sorted(sorted_samples, 95.0);
    out["max_ms"] = bucket.samples_ms.max();
    out["mean_ms"] = bucket.samples_ms.mean();
    return out;
}

inline nlohmann::json gui_frame_rate_bucket_json(const GuiFrameRateBucket& bucket)
{
    nlohmann::json out = {
        {"sample_count", bucket.samples.sample_count()},
        {"retained_sample_count", bucket.samples.retained_sample_count()},
        {"max_retained_samples", bucket.samples.max_retained_samples()},
        {"percentile_sampling_policy", "deterministic_reservoir_v1"},
        {"percentiles_exact", bucket.samples.percentiles_exact()},
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

    const std::vector<double> sorted_samples = bucket.samples.sorted_retained_samples();
    out["min_fps"] = bucket.samples.min();
    out["p05_fps"] = gui_percentile_from_sorted(sorted_samples, 5.0);
    out["p50_fps"] = gui_percentile_from_sorted(sorted_samples, 50.0);
    out["p95_fps"] = gui_percentile_from_sorted(sorted_samples, 95.0);
    out["max_fps"] = bucket.samples.max();
    out["mean_fps"] = bucket.samples.mean();
    return out;
}

inline nlohmann::json gui_imgui_glfw_size_cache_stats_json(
    const OrangeImguiGlfwSizeCacheStats& stats)
{
    const uint64_t total_size_requests =
        stats.window_size_cache_hits +
        stats.window_size_fallbacks +
        stats.framebuffer_size_cache_hits +
        stats.framebuffer_size_fallbacks +
        stats.null_window_requests;
    return {
        {"schema_version", 1},
        {"source", "orange_imgui_glfw_size_cache"},
        {"cache_context_registered", stats.cache_context_registered},
        {"window_size_cache_hits", stats.window_size_cache_hits},
        {"window_size_fallbacks", stats.window_size_fallbacks},
        {"framebuffer_size_cache_hits", stats.framebuffer_size_cache_hits},
        {"framebuffer_size_fallbacks", stats.framebuffer_size_fallbacks},
        {"null_window_requests", stats.null_window_requests},
        {"total_size_requests", total_size_requests}
    };
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
                                                  const int swap_interval = -1,
                                                  const int frame_max_fps = -1,
                                                  const bool yolo_speed_graphs_enabled = false,
                                                  const OrangeImguiGlfwSizeCacheStats& imgui_glfw_size_cache_stats =
                                                      OrangeImguiGlfwSizeCacheStats{})
{
    return {
        {"schema_version", 1},
        {"source", "imgui_io_delta_time"},
        {"stream_downsample", stream_downsample},
        {"display_preview_max_fps", display_preview_max_fps},
        {"swap_interval", swap_interval},
        {"frame_max_fps", frame_max_fps},
        {"yolo_speed_graphs_enabled", yolo_speed_graphs_enabled},
        {"saw_crop_preview_enabled", stats.saw_crop_preview_enabled},
        {"saw_crop_preview_hidden", stats.saw_crop_preview_hidden},
        {"saw_crop_preview_visible", stats.saw_crop_preview_visible},
        {"overall", gui_frame_rate_bucket_json(stats.overall)},
        {"crop_preview_hidden", gui_frame_rate_bucket_json(stats.crop_preview_hidden)},
        {"crop_preview_visible", gui_frame_rate_bucket_json(stats.crop_preview_visible)},
        {"imgui_glfw_size_cache", gui_imgui_glfw_size_cache_stats_json(
            imgui_glfw_size_cache_stats)},
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
