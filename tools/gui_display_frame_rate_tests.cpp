#include "gui_display_frame_rate.h"
#include "gui/camera_temperature_sampling.h"

#include <chrono>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

void require_near(double actual, double expected, double tolerance, const std::string& message)
{
    if (std::fabs(actual - expected) > tolerance) {
        std::cerr << "FAIL: " << message
                  << " actual=" << actual
                  << " expected=" << expected
                  << std::endl;
        std::exit(1);
    }
}

void test_percentile_bucket_json()
{
    orange::gui::GuiFrameRateBucket bucket;
    bucket.Add(60.0);
    bucket.Add(30.0);
    bucket.Add(120.0);
    bucket.Add(-1.0);
    bucket.Add(0.0);
    bucket.Add(20000.0);

    const nlohmann::json payload = orange::gui::gui_frame_rate_bucket_json(bucket);
    require(payload.value("sample_count", 0) == 3, "invalid FPS samples should be ignored");
    require_near(payload.value("min_fps", 0.0), 30.0, 0.001, "min FPS");
    require_near(payload.value("p05_fps", 0.0), 30.0, 0.001, "p05 FPS");
    require_near(payload.value("p50_fps", 0.0), 60.0, 0.001, "p50 FPS");
    require_near(payload.value("p95_fps", 0.0), 120.0, 0.001, "p95 FPS");
    require_near(payload.value("max_fps", 0.0), 120.0, 0.001, "max FPS");
    require_near(payload.value("mean_fps", 0.0), 70.0, 0.001, "mean FPS");
}

void test_sampling_only_during_active_recording()
{
    orange::gui::GuiDisplayFrameRateStats stats;
    orange::gui::gui_sample_display_frame_rate(&stats, 0.010f, true, true, true);
    require(stats.overall.samples.empty(), "sampling should be inactive before Reset()");

    stats.Reset();
    orange::gui::gui_sample_display_frame_rate(&stats, 0.010f, false, true, true);
    require(stats.overall.samples.empty(), "sampling should ignore drain/finalization frames");

    orange::gui::gui_sample_display_frame_rate(&stats, 0.010f, true, true, true);
    orange::gui::gui_sample_display_frame_rate(&stats, 0.020f, true, true, false);
    orange::gui::gui_sample_display_frame_rate(&stats, 0.016f, true, false, true);
    orange::gui::GuiFrameTimingSample timing;
    timing.frame_total_ms = 12.0;
    timing.pre_frame_maintenance_ms = 0.2;
    timing.imgui_new_frame_ms = 0.3;
    timing.orange_window_draw_ms = 4.0;
    timing.recording_panel_draw_ms = 1.25;
    timing.camera_properties_draw_ms = 0.6;
    timing.main_texture_upload_ms = 1.5;
    timing.crop_texture_upload_ms = 0.25;
    timing.camera_window_draw_ms = 2.0;
    timing.crop_window_draw_ms = 0.5;
    timing.speed_graph_draw_ms = 0.75;
    timing.render_present_ms = 3.0;
    timing.main_texture_upload_count = 4;
    timing.crop_texture_upload_count = 2;
    orange::gui::gui_sample_frame_timings(&stats, true, timing);

    const nlohmann::json payload = orange::gui::gui_display_frame_rate_json(stats, 4, 30, 0, 60, true);
    require(payload["overall"].value("sample_count", 0) == 3, "overall sample count");
    require(payload.value("stream_downsample", 0) == 4, "stream downsample should be recorded");
    require(payload.value("display_preview_max_fps", 0) == 30, "display preview cap should be recorded");
    require(payload.value("swap_interval", -1) == 0, "swap interval should be recorded");
    require(payload.value("frame_max_fps", -1) == 60, "frame cap should be recorded");
    require(payload.value("yolo_speed_graphs_enabled", false), "speed graph visibility should be recorded");
    require(payload["crop_preview_visible"].value("sample_count", 0) == 1, "visible sample count");
    require(payload["crop_preview_hidden"].value("sample_count", 0) == 1, "hidden sample count");
    const nlohmann::json timings = payload["timings"];
    require(timings["frame_total_ms"].value("sample_count", 0) == 1, "frame timing sample count");
    require_near(
        timings["main_texture_upload_ms"].value("p50_ms", 0.0),
        1.5,
        0.001,
        "main texture upload p50");
    require_near(
        timings["orange_window_draw_ms"].value("p50_ms", 0.0),
        4.0,
        0.001,
        "orange window draw p50");
    require_near(
        timings["recording_panel_draw_ms"].value("p50_ms", 0.0),
        1.25,
        0.001,
        "recording panel draw p50");
    require(timings.value("main_texture_upload_count", 0) == 4, "main texture upload count");
    require(timings.value("crop_texture_upload_count", 0) == 2, "crop texture upload count");
    require(payload.value("saw_crop_preview_enabled", false), "crop preview enabled flag");
    require(payload.value("saw_crop_preview_visible", false), "crop preview visible flag");
    require(payload.value("saw_crop_preview_hidden", false), "crop preview hidden flag");
    require_near(
        payload["crop_preview_visible"].value("p50_fps", 0.0),
        100.0,
        0.01,
        "visible p50 FPS");
    require_near(
        payload["crop_preview_hidden"].value("p50_fps", 0.0),
        50.0,
        0.01,
        "hidden p50 FPS");

    stats.Finish();
    orange::gui::gui_sample_display_frame_rate(&stats, 0.010f, true, true, true);
    require(stats.overall.samples.sample_count() == 3,
            "sampling should stop after Finish()");
}

void test_recording_duration_storage_is_bounded()
{
    orange::gui::GuiDurationBucket first;
    orange::gui::GuiDurationBucket second;
    constexpr std::uint64_t kObservedSamples = 100000;
    for (std::uint64_t index = 0; index < kObservedSamples; ++index) {
        const double value = static_cast<double>(index) * 0.5;
        first.Add(value);
        second.Add(value);
    }

    require(first.samples_ms.sample_count() == kObservedSamples,
            "bounded telemetry must preserve the exact observed count");
    require(first.samples_ms.retained_sample_count() ==
                orange::BoundedSampleStatistics::kDefaultMaxRetainedSamples,
            "bounded telemetry must cap retained percentile samples");
    require(!first.samples_ms.percentiles_exact(),
            "a reservoir summary must declare approximate percentiles");
    require_near(first.samples_ms.min(), 0.0, 0.0, "bounded exact minimum");
    require_near(
        first.samples_ms.max(),
        static_cast<double>(kObservedSamples - 1) * 0.5,
        0.0,
        "bounded exact maximum");
    require_near(first.samples_ms.mean(), 24999.75, 0.000001, "bounded exact mean");
    require(first.samples_ms.sorted_retained_samples() ==
                second.samples_ms.sorted_retained_samples(),
            "reservoir sampling must be deterministic");

    const nlohmann::json payload = orange::gui::gui_duration_bucket_json(first);
    require(payload.value("sample_count", 0ULL) == kObservedSamples,
            "JSON must report the full observed count");
    require(payload.value("retained_sample_count", 0ULL) ==
                orange::BoundedSampleStatistics::kDefaultMaxRetainedSamples,
            "JSON must report the bounded retained count");
    require(!payload.value("percentiles_exact", true),
            "JSON must disclose reservoir percentile semantics");
}

void test_camera_temperature_reads_are_decimated()
{
    GuiCameraTemperatureSamplingState state;
    const auto start = std::chrono::steady_clock::time_point(
        std::chrono::milliseconds(5000));
    require(gui_camera_temperature_sample_due(
                &state, true, start, std::chrono::milliseconds(1000)),
            "opening the temperature plot must sample immediately");
    require(!gui_camera_temperature_sample_due(
                &state,
                true,
                start + std::chrono::milliseconds(10),
                std::chrono::milliseconds(1000)),
            "temperature SDK reads must not run every GUI frame");
    require(gui_camera_temperature_sample_due(
                &state,
                true,
                start + std::chrono::milliseconds(1000),
                std::chrono::milliseconds(1000)),
            "temperature sampling must refresh at one hertz");
    require(state.sample_count == 2 && state.skipped_count == 1,
            "temperature sampler counters must describe decimation");
    require(!gui_camera_temperature_sample_due(
                &state,
                false,
                start + std::chrono::milliseconds(1010),
                std::chrono::milliseconds(1000)),
            "hidden temperature plot must not sample");
    require(gui_camera_temperature_sample_due(
                &state,
                true,
                start + std::chrono::milliseconds(1020),
                std::chrono::milliseconds(1000)),
            "reopening the plot must sample immediately");
}

void test_imgui_glfw_size_cache_json()
{
    OrangeImguiGlfwSizeCacheStats stats;
    stats.cache_context_registered = true;
    stats.window_size_cache_hits = 11;
    stats.window_size_fallbacks = 1;
    stats.framebuffer_size_cache_hits = 13;
    stats.framebuffer_size_fallbacks = 2;
    stats.null_window_requests = 3;

    const nlohmann::json payload = orange::gui::gui_imgui_glfw_size_cache_stats_json(stats);
    require(payload.value("schema_version", 0) == 1, "size-cache schema version");
    require(
        payload.value("source", std::string{}) == "orange_imgui_glfw_size_cache",
        "size-cache source");
    require(payload.value("cache_context_registered", false), "size-cache context registered");
    require(payload.value("window_size_cache_hits", 0) == 11, "window-size cache hits");
    require(payload.value("window_size_fallbacks", 0) == 1, "window-size fallback count");
    require(payload.value("framebuffer_size_cache_hits", 0) == 13, "framebuffer-size cache hits");
    require(payload.value("framebuffer_size_fallbacks", 0) == 2, "framebuffer-size fallback count");
    require(payload.value("null_window_requests", 0) == 3, "null-window request count");
    require(payload.value("total_size_requests", 0) == 30, "size-cache total count");

    orange::gui::GuiDisplayFrameRateStats frame_rate_stats;
    const nlohmann::json frame_rate_payload =
        orange::gui::gui_display_frame_rate_json(frame_rate_stats, 4, 30, 1, 60, false, stats);
    require(
        frame_rate_payload["imgui_glfw_size_cache"].value("total_size_requests", 0) == 30,
        "display frame-rate JSON should include size-cache stats");
}

}  // namespace

int main()
{
    test_percentile_bucket_json();
    test_sampling_only_during_active_recording();
    test_recording_duration_storage_is_bounded();
    test_camera_temperature_reads_are_decimated();
    test_imgui_glfw_size_cache_json();
    std::cout << "gui_display_frame_rate_tests passed" << std::endl;
    return 0;
}
