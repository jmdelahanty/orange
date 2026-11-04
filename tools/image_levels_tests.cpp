// Image levels (src/image_levels.cpp): statistics, the camera config block and
// the per-camera check.
#include "image_levels.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace orange::image_levels;

namespace {

int failures = 0;

void require(bool ok, const std::string& what)
{
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

bool throws(const json& camera_config)
{
    try {
        ParseCameraExpectation(camera_config);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

void test_statistics()
{
    // 1000 pixels: value v appears for 1000 * fraction of pixels.
    std::vector<uint8_t> pixels;
    pixels.insert(pixels.end(), 10, 0);     // 1.0% zero
    pixels.insert(pixels.end(), 40, 100);   // to 5.0%
    pixels.insert(pixels.end(), 900, 200);  // to 95.0%
    pixels.insert(pixels.end(), 49, 254);   // to 99.9%
    pixels.insert(pixels.end(), 1, 255);    // 0.1% saturated
    const Statistics s = ComputeStatistics(pixels.data(), pixels.size());
    require(s.pixel_count == 1000, "pixel count");
    require(s.zero_fraction == 0.01 && s.saturated_fraction == 0.001, "zero and saturated fractions");
    require(s.p01 == 0 && s.p05 == 100 && s.p50 == 200 && s.p95 == 200, "low percentiles reach the level exactly");
    require(s.p99 == 254 && s.p999 == 254, "p99 and p999 below the single saturated pixel");
    const double mean = (40 * 100 + 900 * 200 + 49 * 254 + 255) / 1000.0;
    require(std::abs(s.mean - mean) < 1e-9, "mean");

    const std::vector<uint8_t> white(64, 255);
    const Statistics w = ComputeStatistics(white.data(), white.size());
    require(w.saturated_fraction == 1.0 && w.p01 == 255 && w.mean == 255.0, "all saturated");

    const Statistics empty = ComputeStatistics(nullptr, 0);
    require(empty.pixel_count == 0 && empty.mean == 0.0, "no pixels");

    const json j = StatisticsToJson(s);
    for (const char* key : {"pixel_count", "saturation_value", "mean", "zero_fraction", "saturated_fraction", "p01",
                            "p05", "p50", "p95", "p99", "p999"}) {
        require(j.contains(key), std::string("statistics carries ") + key);
    }
    require(j.size() == 11 && j.at("saturation_value") == 255, "statistics shape");
}

json block(json fields)
{
    json b = {{"schema_id", "orange.camera.image_levels"}, {"schema_version", 1}};
    b.update(fields);
    return {{"image_levels", b}};
}

json block2(json fields, const char* region = "dish_interior")
{
    json b = {{"schema_id", "orange.camera.image_levels"}, {"schema_version", 2}, {"region", region}};
    b.update(fields);
    return {{"image_levels", b}};
}

// Pixels inside the circle by the schema's rule, every pixel tested.
uint64_t brute_force_inside(int width, int height, double cx, double cy, double r)
{
    uint64_t n = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double dx = x - cx, dy = y - cy;
            n += dx * dx + dy * dy <= r * r ? 1 : 0;
        }
    }
    return n;
}

void test_circle()
{
    // Values encode position, so the histogram also proves which pixels counted.
    const int w = 37, h = 29;
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) pixels[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>((x * 7 + y * 3) % 251);
    uint64_t seed = 12345;
    const auto next = [&seed]() {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(seed >> 11) / 9007199254740992.0;
    };
    for (int trial = 0; trial < 400; ++trial) {
        const double cx = -8.0 + next() * (w + 16.0);
        const double cy = -8.0 + next() * (h + 16.0);
        const double r = trial % 10 == 0 ? std::floor(next() * 12.0) + 0.0 : next() * 30.0;  // integer radii too
        const Statistics s = ComputeStatisticsInCircle(pixels.data(), w, h, cx, cy, r);
        require(s.pixel_count == brute_force_inside(w, h, cx, cy, r),
                "circle pixel count matches the schema rule (trial " + std::to_string(trial) + ")");
        uint64_t sum = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy <= r * r) sum += pixels[static_cast<size_t>(y) * w + x];
            }
        if (s.pixel_count > 0) {
            require(std::abs(s.mean - static_cast<double>(sum) / s.pixel_count) < 1e-9, "circle mean");
        }
    }
    // A centred circle on a frame with a saturated border: the interior is clean.
    std::vector<uint8_t> frame(100 * 100, 255);
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 100; ++x)
            if ((x - 50) * (x - 50) + (y - 50) * (y - 50) <= 40 * 40) frame[y * 100 + x] = 120;
    const Statistics inside = ComputeStatisticsInCircle(frame.data(), 100, 100, 50.0, 50.0, 40.0);
    require(inside.saturated_fraction == 0.0 && inside.mean == 120.0, "saturated border outside the dish is ignored");
    require(ComputeStatistics(frame.data(), frame.size()).saturated_fraction > 0.4, "whole frame sees the border");
    require(ComputeStatisticsInCircle(frame.data(), 100, 100, 50.0, 50.0, 0.0).pixel_count == 0, "zero radius");
}

void test_camera_block()
{
    require(!ParseCameraExpectation(json::object()).configured, "absent block is not configured");

    const json full = block({{"expected_mean", 182.0},
                             {"mean_tolerance_fraction", 0.15},
                             {"max_saturated_fraction", 0.001},
                             {"basis", {{"source", "test"}, {"settings", {{"iris", 24}, {"exposure", 100}}}}}});
    const CameraExpectation e = ParseCameraExpectation(full);
    require(e.configured && e.expected_mean == 182.0 && e.mean_tolerance_fraction == 0.15 &&
                e.max_saturated_fraction == 0.001,
            "full block parses");
    require(e.region == LevelsRegion::kWholeFrame, "a v1 block checks the whole frame");
    json as_v2 = full.at("image_levels");
    as_v2["schema_version"] = 2;
    as_v2["region"] = "whole_frame";
    require(BuildCameraExpectation(e) == as_v2, "a v1 block is written back as v2, whole_frame");
    const json full2 = block2({{"expected_mean", 150.0}, {"mean_tolerance_fraction", 0.1},
                               {"max_saturated_fraction", 0.002}, {"basis", {{"source", "test"}}}});
    const CameraExpectation e2 = ParseCameraExpectation(full2);
    require(e2.region == LevelsRegion::kDishInterior, "v2 dish_interior parses");
    require(BuildCameraExpectation(e2) == full2.at("image_levels"), "v2 block round-trips");
    require(throws(block({{"max_saturated_fraction", 0.1}, {"region", "whole_frame"}})), "region is not a v1 field");
    require(throws({{"image_levels", {{"schema_id", "orange.camera.image_levels"}, {"schema_version", 2},
                                      {"max_saturated_fraction", 0.1}}}}),
            "v2 needs region");
    require(throws(block2({{"max_saturated_fraction", 0.1}}, "rim")), "unknown region");
    require(throws({{"image_levels", {{"schema_id", "orange.camera.image_levels"}, {"schema_version", 3},
                                      {"region", "whole_frame"}, {"max_saturated_fraction", 0.1}}}}),
            "unknown version");
    require(BuildCameraExpectation(CameraExpectation{}).empty(), "unconfigured builds nothing");

    const CameraExpectation sat_only = ParseCameraExpectation(block({{"max_saturated_fraction", 0.01}}));
    require(sat_only.configured && !sat_only.expected_mean, "saturation-only block");

    require(throws({{"image_levels", 3}}), "not an object");
    require(throws({{"image_levels", {{"schema_id", "x"}, {"schema_version", 1}, {"max_saturated_fraction", 0.1}}}}),
            "wrong schema id");
    require(throws(block({{"max_saturated_fraction", 0.1}, {"extra", 1}})), "unknown field");
    require(throws(block({{"expected_mean", 180}})), "mean without tolerance");
    require(throws(block({{"mean_tolerance_fraction", 0.1}, {"max_saturated_fraction", 0.1}})),
            "tolerance without mean");
    require(throws(block(json::object())), "neither check");
    require(throws(block({{"expected_mean", 255}, {"mean_tolerance_fraction", 0.1}})), "mean out of range");
    require(throws(block({{"max_saturated_fraction", 1.5}})), "fraction out of range");
    require(throws(block({{"max_saturated_fraction", 0.1}, {"basis", "text"}})), "basis not an object");
}

Measurement measured(double mean_target, double saturated)
{
    Measurement m;
    m.ok = true;
    m.source = "test";
    m.statistics.pixel_count = 100;
    m.statistics.mean = mean_target;
    m.statistics.saturated_fraction = saturated;
    return m;
}

void test_evaluate()
{
    const CameraExpectation e = ParseCameraExpectation(block({{"expected_mean", 200.0},
                                                              {"mean_tolerance_fraction", 0.1},
                                                              {"max_saturated_fraction", 0.001},
                                                              {"basis", {{"settings", {{"iris", 19}}}}}}));
    LevelSettings live;
    live.iris = 19;

    json r = EvaluateCamera(e, live, measured(210, 0.0005), std::nullopt, false, "");
    require(r.at("status") == "ok" && r.at("warnings").empty(), "in band, under the limit: ok");
    require(r.at("expectation_basis_settings_differ").empty(), "same iris as the basis");

    r = EvaluateCamera(e, live, measured(170, 0.0), std::nullopt, false, "");
    require(r.at("status") == "warning" && r.at("warnings") == json::array({"mean_out_of_band"}), "mean out of band");

    r = EvaluateCamera(e, live, measured(200, 0.034), std::nullopt, false, "");
    require(r.at("warnings") == json::array({"saturated_fraction_above_limit"}), "saturated above limit");

    live.iris = 24;
    r = EvaluateCamera(e, live, measured(200, 0.0), measured(120, 0.0), true, "");
    require(r.at("expectation_basis_settings_differ") == json::array({"iris"}), "iris differs from the basis");
    require(r.at("rehomed_iris") == true && r.at("first_measurement").at("statistics").at("mean") == 120.0,
            "first measurement kept after a re-home");

    r = EvaluateCamera(CameraExpectation{}, live, measured(200, 0.5), std::nullopt, false, "");
    require(r.at("status") == "no_expectation" && r.at("expectation").is_null(), "no expectation still measured");
    require(r.at("measurement").at("statistics").at("saturated_fraction") == 0.5, "statistics recorded anyway");

    r = EvaluateCamera(e, live, std::nullopt, std::nullopt, false, "recording_started_first");
    require(r.at("status") == "not_measured" && r.at("not_measured_reason") == "recording_started_first", "not measured");

    Measurement failed;
    failed.error = "boom";
    r = EvaluateCamera(e, live, failed, std::nullopt, false, "");
    require(r.at("status") == "measurement_failed" && r.at("measurement").at("error") == "boom", "failed measurement");

    // Dish interior: judged on the interior statistics, never the whole frame.
    const CameraExpectation dish = ParseCameraExpectation(block2({{"expected_mean", 150.0},
                                                                  {"mean_tolerance_fraction", 0.1},
                                                                  {"max_saturated_fraction", 0.001}}));
    Measurement with_dish = measured(200.0, 0.03);  // bright, clipped whole frame
    with_dish.dish_region.available = true;
    with_dish.dish_interior = Statistics{};
    with_dish.dish_interior->pixel_count = 50;
    with_dish.dish_interior->mean = 151.0;
    with_dish.dish_interior->saturated_fraction = 0.0004;
    json d = EvaluateCamera(dish, live, with_dish, std::nullopt, false, "");
    require(d.at("status") == "ok" && d.at("checked_region") == "dish_interior", "interior in band and under the limit");
    with_dish.dish_interior->saturated_fraction = 0.002;
    d = EvaluateCamera(dish, live, with_dish, std::nullopt, false, "");
    require(d.at("warnings") == json::array({"saturated_fraction_above_limit"}), "interior above the limit");
    d = EvaluateCamera(dish, live, measured(150.0, 0.0), std::nullopt, false, "");
    require(d.at("status") == "warning" && d.at("warnings") == json::array({"dish_interior_unavailable"}),
            "no dish region: warned, not judged on the whole frame");
    require(EvaluateCamera(e, live, with_dish, std::nullopt, false, "").at("checked_region") == "whole_frame",
            "v1 expectation checks the whole frame");

    const json rec = BuildRecordingBlock(true, true, {{"2010095", r}});
    require(rec.at("schema_id") == "orange.recording.image_levels_check" && rec.at("schema_version") == 2 &&
                rec.at("policy") == "warn_only",
            "recording block identity");
}


// Sample documents of every status, for scripts/validate_output_schemas.py.
void write_samples(const std::string& dir)
{
    const CameraExpectation e = ParseCameraExpectation(block2({{"expected_mean", 200.0},
                                                               {"mean_tolerance_fraction", 0.1},
                                                               {"max_saturated_fraction", 0.001},
                                                               {"basis", {{"source", "gui_measurement"}}}}));
    const CameraExpectation whole = ParseCameraExpectation(block({{"max_saturated_fraction", 0.001}}));
    std::vector<uint8_t> pixels(4512 * 16, 200);
    pixels[0] = 255;
    pixels[1] = 0;
    Measurement ok;
    ok.ok = true;
    ok.measured_at_utc = "2026-11-02T20:00:00Z";
    ok.source = "full_resolution_native_stream_snapshot";
    ok.native_source_storage = "analytics_owned_device";
    ok.camera_frame_id = 7;
    ok.settings.exposure = 150;
    ok.settings.iris = 19;
    ok.statistics = ComputeStatistics(pixels.data(), pixels.size());
    Measurement no_dish = ok;
    no_dish.dish_region.unavailable_reason = "no_active_dish_rim_observation";
    ok.dish_region.available = true;
    ok.dish_region.center_x = 2256.0;
    ok.dish_region.center_y = 8.0;
    ok.dish_region.radius = 6.5;
    ok.dish_region.source_json = json{{"boundary", "accepted_inner_rim_boundary"},
                                      {"active_pointer_path", "/calibrations/active/physical_dish_registration/Cam1.json"},
                                      {"active_pointer_sha256", "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                                      {"artifact_id", "dishrim_test"},
                                      {"artifact_schema_id", "orange.calibration.dish_top_rim_observation"},
                                      {"observation_path", "/calibrations/sessions/x/observation.json"},
                                      {"observation_sha256", "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
                                      {"manifest_sha256", "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"},
                                      {"observation_created_utc", "2026-11-04T01:00:16Z"}}.dump();
    ok.dish_interior = ComputeStatisticsInCircle(pixels.data(), 4512, 16, 2256.0, 8.0, 6.5);
    Measurement low = ok;
    low.statistics.mean = 120.0;
    Measurement failed;
    failed.error = "no frame within 10 s";
    failed.measured_at_utc = ok.measured_at_utc;
    LevelSettings live = ok.settings;

    ResultStore store;
    store.SetCheckSettings(true, true);
    store.Set("1", EvaluateCamera(e, live, ok, std::nullopt, false, ""));
    store.Set("2", EvaluateCamera(e, live, ok, low, true, ""));
    store.Set("3", EvaluateCamera(CameraExpectation{}, live, ok, std::nullopt, false, ""));
    store.Set("4", EvaluateCamera(e, live, failed, std::nullopt, false, ""));
    store.Set("5", EvaluateCamera(e, live, std::nullopt, std::nullopt, false, "recording_started_before_measurement"));
    store.Set("8", EvaluateCamera(e, live, no_dish, std::nullopt, false, ""));
    store.Set("9", EvaluateCamera(whole, live, no_dish, std::nullopt, false, ""));
    LevelSettings changed = live;
    changed.iris = 24;
    const json rec = store.BuildRecordingBlockAt(
        {{"1", live}, {"2", live}, {"3", live}, {"4", live}, {"5", live}, {"6", live}, {"7", changed}, {"8", live},
         {"9", live}});
    require(rec.at("cameras").at("6").at("not_measured_reason") == "no_check_result", "camera without a result");
    require(rec.at("cameras").at("7").at("not_measured_reason") == "no_check_result", "unknown serial");
    store.Set("7", EvaluateCamera(e, live, ok, std::nullopt, false, ""));
    const json rec2 = store.BuildRecordingBlockAt({{"7", changed}});
    require(rec2.at("cameras").at("7").at("settings_changed_since_measurement") == json::array({"iris"}),
            "iris changed after the measurement");
    std::ofstream(dir + "/image_levels_check_sample.json") << rec.dump(2);
    std::ofstream(dir + "/image_levels_check_sample_changed.json") << rec2.dump(2);
    std::ofstream(dir + "/camera_image_levels_sample.json") << BuildCameraExpectation(e).dump(2);
}

void test_save_refusal()
{
    Measurement m;
    require(!SaveRefusal(m, 0.001).empty(), "failed measurement is refused");
    m.ok = true;
    m.statistics.saturated_fraction = 0.0177;
    const std::string refusal = SaveRefusal(m, 0.001);
    require(!refusal.empty(), "limit below the measurement is refused");
    require(refusal.find("0.0177") != std::string::npos, "refusal names the measured fraction");
    require(SaveRefusal(m, 0.0177).empty(), "limit equal to the measurement is accepted");
    require(SaveRefusal(m, 0.03).empty(), "limit above the measurement is accepted");
}

}  // namespace

int main(int argc, char** argv)
{
    test_statistics();
    test_circle();
    test_save_refusal();
    test_camera_block();
    test_evaluate();
    if (argc == 2) write_samples(argv[1]);
    if (failures != 0) {
        std::cerr << "image_levels_tests: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "image_levels_tests: PASS\n";
    return 0;
}
