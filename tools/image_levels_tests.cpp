// Image levels (src/image_levels.cpp): statistics, the camera config block and
// the per-camera check.
#include "image_levels.h"

#include <cmath>
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
    require(BuildCameraExpectation(e) == full.at("image_levels"), "full block round-trips");
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

    const json rec = BuildRecordingBlock(true, true, {{"2010095", r}});
    require(rec.at("schema_id") == "orange.recording.image_levels_check" && rec.at("schema_version") == 1 &&
                rec.at("policy") == "warn_only",
            "recording block identity");
}


// Sample documents of every status, for scripts/validate_output_schemas.py.
void write_samples(const std::string& dir)
{
    const CameraExpectation e = ParseCameraExpectation(block({{"expected_mean", 200.0},
                                                              {"mean_tolerance_fraction", 0.1},
                                                              {"max_saturated_fraction", 0.001},
                                                              {"basis", {{"source", "gui_measurement"}}}}));
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
    LevelSettings changed = live;
    changed.iris = 24;
    const json rec = store.BuildRecordingBlockAt(
        {{"1", live}, {"2", live}, {"3", live}, {"4", live}, {"5", live}, {"6", live}, {"7", changed}});
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
