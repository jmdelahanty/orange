#include "camera_sensor_pipeline_state.h"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

nlohmann::json readable(const nlohmann::json& value)
{
    return {
        {"status", "readable"},
        {"supported", true},
        {"readable", true},
        {"value", value},
    };
}

void test_required_inventory_nodes_and_semantics()
{
    std::set<std::string> names;
    std::string roi_offset_interpretation;
    std::string black_offset_interpretation;
    for (const auto& feature : orange::camera_sensor_pipeline::feature_specs()) {
        names.insert(feature.name);
        if (std::string(feature.name) == "OffsetX") {
            roi_offset_interpretation = feature.interpretation;
        }
        if (std::string(feature.name) == "Offset") {
            black_offset_interpretation = feature.interpretation;
        }
    }
    for (const std::string required : {
             "Gain", "HCG", "PGAGain", "Offset", "OffsetSigned",
             "LUTEnable", "Gamma", "ADC", "DualADC", "PixelFormat",
             "Exposure", "OffsetX", "OffsetY", "Bin", "X_RES", "Y_RES",
             "ReverseX", "ReverseY"}) {
        require(names.count(required) == 1, "inventory contains " + required);
    }
    require(
        roi_offset_interpretation.find("ROI") != std::string::npos,
        "OffsetX is explicitly an ROI coordinate");
    require(
        black_offset_interpretation.find("black-level") != std::string::npos,
        "Offset is explicitly a black-level control");
}

void test_confirmed_requested_readbacks()
{
    nlohmann::json state = {
        {"features", {
            {"Gain", readable(256)},
            {"Exposure", readable(50)},
            {"PixelFormat", readable("Mono8")},
        }},
    };
    const nlohmann::json requested = {
        {"Gain", 256},
        {"Exposure", 50},
        {"PixelFormat", "Mono8"},
    };
    const nlohmann::json sources = {
        {"Gain", "camera_config"},
        {"Exposure", "camera_config"},
        {"PixelFormat", "camera_config"},
    };
    orange::camera_sensor_pipeline::add_requested_readbacks(
        &state, requested, sources);
    require(
        state.value("applied_state_status", "") == "confirmed",
        "matching applied state is confirmed");
    require(state.at("all_requested_readbacks_match") == true, "all values match");
    require(
        state.at("requested_readbacks").at("Gain").value("status", "") == "match",
        "gain comparison matches");
    require(
        state.at("requested_readbacks").at("Gain").value("request_source", "") ==
            "camera_config",
        "request provenance is preserved");
}

void test_mismatch_and_unsupported_are_explicit()
{
    nlohmann::json state = {
        {"features", {
            {"Gain", readable(512)},
            {"HCG", {
                {"status", "unsupported"},
                {"supported", false},
                {"readable", false},
            }},
        }},
    };
    orange::camera_sensor_pipeline::add_requested_readbacks(
        &state,
        {{"Gain", 256}, {"HCG", false}},
        {{"Gain", "camera_config"}, {"HCG", "characterization_request"}});
    require(
        state.value("applied_state_status", "") == "incomplete",
        "unsupported requested node makes applied state incomplete");
    require(
        state.at("requested_readbacks").at("Gain").value("status", "") == "mismatch",
        "numeric mismatch remains explicit");
    require(
        state.at("requested_readbacks").at("HCG").value("status", "") == "unsupported",
        "unsupported node remains explicit");
}

void test_capability_only_capture_is_not_called_confirmed()
{
    nlohmann::json state = {{"features", nlohmann::json::object()}};
    orange::camera_sensor_pipeline::add_requested_readbacks(
        &state, nlohmann::json::object(), nlohmann::json::object());
    require(
        state.value("applied_state_status", "") == "not_requested",
        "capability-only inventory is not mislabeled as applied-state confirmation");
    require(
        state.at("all_requested_readbacks_match").is_null(),
        "empty requested comparison is null rather than true");
}

nlohmann::json readable_ranged(long long value, long long max_value)
{
    nlohmann::json feature = readable(value);
    feature["min"] = 16;
    feature["max"] = max_value;
    feature["increment"] = 16;
    return feature;
}

nlohmann::json unsupported()
{
    return {{"status", "unsupported"}, {"supported", false}, {"readable", false}};
}

void test_raster_geometry_full_sensor_unbinned()
{
    // pancake0 shape: 4512 x 4512 at offset 0, Bin 1x1, firmware constants readable.
    const nlohmann::json features = {
        {"Width", readable_ranged(4512, 4512)},
        {"Height", readable_ranged(4512, 4512)},
        {"OffsetX", readable_ranged(0, 0)},
        {"OffsetY", readable_ranged(0, 0)},
        {"Bin", readable("1x1")},
        {"X_RES", readable(4512)},
        {"Y_RES", readable(4512)},
        {"ReverseX", unsupported()},
        {"ReverseY", unsupported()},
    };
    const nlohmann::json geometry =
        orange::camera_sensor_pipeline::summarize_raster_geometry(features);
    require(geometry.at("bin") == "1x1", "bin entry name is carried");
    require(geometry.at("bin_factor") == 1, "1x1 parses to factor 1");
    require(geometry.at("sensor").at("width_px") == 4512, "sensor width from X_RES");
    require(
        geometry.at("sensor").at("source") == "firmware_constant",
        "firmware constant is the preferred sensor-extent source");
    require(geometry.at("aoi_is_full_sensor") == true, "full-sensor AOI is recognised");
    require(
        geometry.at("stream_raster").at("coordinate_space") == "camera_native_pixels",
        "stream raster names the recorded coordinate space");
    require(geometry.at("stream_raster").at("width_px") == 4512, "stream raster is the AOI");
    require(
        geometry.at("flip_nodes").at("reverse_x") == "unsupported",
        "absent flip node is recorded as unsupported, not omitted");
    require(geometry.at("raster_transform") == "none", "Orange declares no software reorientation");
    require(
        geometry.at("subsampling_nodes").at("decimation_horizontal") == "not_in_inventory",
        "a node missing from the features object is distinguishable from unsupported");
}

void test_raster_geometry_binned_sub_aoi_derived_extent()
{
    // Bin 2x2, AOI 1024 x 1024 at (1000, 1000); firmware constants not readable,
    // so the sensor extent comes from (Width.max + OffsetX) * bin: (1256 + 1000) * 2.
    const nlohmann::json features = {
        {"Width", readable_ranged(1024, 1256)},
        {"Height", readable_ranged(1024, 1256)},
        {"OffsetX", readable_ranged(1000, 1232)},
        {"OffsetY", readable_ranged(1000, 1232)},
        {"Bin", readable("2x2")},
        {"X_RES", unsupported()},
        {"Y_RES", unsupported()},
    };
    const nlohmann::json geometry =
        orange::camera_sensor_pipeline::summarize_raster_geometry(features);
    require(geometry.at("bin_factor") == 2, "2x2 parses to factor 2");
    require(geometry.at("sensor").at("width_px") == 4512, "derived sensor width");
    require(geometry.at("sensor").at("height_px") == 4512, "derived sensor height");
    require(
        geometry.at("sensor").at("source") == "derived_from_width_max_offset_and_bin",
        "derivation is named");
    require(geometry.at("aoi_is_full_sensor") == false, "sub-AOI is not full sensor");
    require(geometry.at("aoi").at("offset_x_px") == 1000, "AOI origin is carried");
}

void test_raster_geometry_without_readback_is_null_not_guessed()
{
    const nlohmann::json geometry =
        orange::camera_sensor_pipeline::summarize_raster_geometry(
            nlohmann::json::object());
    require(geometry.at("bin").is_null(), "no Bin readback gives null");
    require(geometry.at("sensor").at("width_px").is_null(), "no extent is invented");
    require(geometry.at("sensor").at("source") == "unavailable", "source says unavailable");
    require(geometry.at("aoi_is_full_sensor").is_null(), "full-sensor verdict is null");
    require(geometry.at("bin_status") == "not_in_inventory", "bin status is explicit");
}

void test_bin_assumption_mismatch_is_visible()
{
    nlohmann::json state = {{"features", {{"Bin", readable("2x2")}}}};
    orange::camera_sensor_pipeline::add_requested_readbacks(
        &state, {{"Bin", "1x1"}}, {{"Bin", "orange_unbinned_raster_assumption"}});
    require(
        state.value("applied_state_status", "") == "mismatch",
        "a binned camera contradicts Orange's unbinned raster assumption");
}

}  // namespace

int main()
{
    test_required_inventory_nodes_and_semantics();
    test_confirmed_requested_readbacks();
    test_mismatch_and_unsupported_are_explicit();
    test_capability_only_capture_is_not_called_confirmed();
    test_raster_geometry_full_sensor_unbinned();
    test_raster_geometry_binned_sub_aoi_derived_extent();
    test_raster_geometry_without_readback_is_null_not_guessed();
    test_bin_assumption_mismatch_is_visible();
    std::cout << "camera_sensor_pipeline_state_tests: PASS\n";
    return 0;
}
