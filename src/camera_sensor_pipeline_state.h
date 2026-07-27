#pragma once

#include <EmergentCameraAPIs.h>

#include <string>
#include <vector>

#include "json.hpp"

namespace orange::camera_sensor_pipeline {

struct FeatureSpec {
    const char* name;
    const char* category;
    const char* interpretation;
};

struct CameraIdentity {
    std::string serial;
    std::string model;
    std::string firmware;
};

// Stable inventory of camera nodes relevant to the acquired numeric signal.
// A missing optional node is preserved as `unsupported`; it is not silently
// omitted and is not treated as a probe failure.
const std::vector<FeatureSpec>& feature_specs();

std::string evt_error_name(EVT_ERROR error);
std::string data_type_name(EvtParamDataType data_type);

// All functions in this module are getter-only. They never set a GenICam
// value, execute a command node, or move a selector such as LUTIndex.
nlohmann::json read_feature(
    Emergent::CEmergentCamera* camera,
    const FeatureSpec& feature);

bool read_genicam_xml(
    Emergent::CEmergentCamera* camera,
    std::string* xml_out,
    std::string* error_out);

// Adds requested-vs-applied comparisons to an already collected state. This
// is separate from hardware I/O so the contract can be unit tested without a
// camera and reused by the standalone capability probe.
void add_requested_readbacks(
    nlohmann::json* state,
    const nlohmann::json& requested_feature_values,
    const nlohmann::json& requested_feature_sources);

nlohmann::json capture_state(
    Emergent::CEmergentCamera* camera,
    const CameraIdentity& identity,
    const std::string& capture_stage,
    const nlohmann::json& requested_feature_values = nlohmann::json::object(),
    const nlohmann::json& requested_feature_sources = nlohmann::json::object());

}  // namespace orange::camera_sensor_pipeline
