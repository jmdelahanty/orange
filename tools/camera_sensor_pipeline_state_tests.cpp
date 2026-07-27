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
             "Exposure", "OffsetX", "OffsetY"}) {
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

}  // namespace

int main()
{
    test_required_inventory_nodes_and_semantics();
    test_confirmed_requested_readbacks();
    test_mismatch_and_unsupported_are_explicit();
    test_capability_only_capture_is_not_called_confirmed();
    std::cout << "camera_sensor_pipeline_state_tests: PASS\n";
    return 0;
}
