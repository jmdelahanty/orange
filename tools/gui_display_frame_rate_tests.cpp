#include "gui_display_frame_rate.h"

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
    timing.main_texture_upload_ms = 1.5;
    timing.crop_texture_upload_ms = 0.25;
    timing.camera_window_draw_ms = 2.0;
    timing.crop_window_draw_ms = 0.5;
    timing.speed_graph_draw_ms = 0.75;
    timing.render_present_ms = 3.0;
    timing.main_texture_upload_count = 4;
    timing.crop_texture_upload_count = 2;
    orange::gui::gui_sample_frame_timings(&stats, true, timing);

    const nlohmann::json payload = orange::gui::gui_display_frame_rate_json(stats, 4, 30, true);
    require(payload["overall"].value("sample_count", 0) == 3, "overall sample count");
    require(payload.value("stream_downsample", 0) == 4, "stream downsample should be recorded");
    require(payload.value("display_preview_max_fps", 0) == 30, "display preview cap should be recorded");
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
    require(stats.overall.samples.size() == 3, "sampling should stop after Finish()");
}

}  // namespace

int main()
{
    test_percentile_bucket_json();
    test_sampling_only_during_active_recording();
    std::cout << "gui_display_frame_rate_tests passed" << std::endl;
    return 0;
}
