#pragma once

// The recording's YOLO spatial mask arm record, recording_geometry_contract.json
// analytics_runtime.yolo_spatial_mask, built as the struct generated from the published
// orange.analytics.spatial_mask_recording_arm v1 (docs/schemas). The GUI
// (arm_gui_yolo_spatial_masks, src/orange.cpp) applies the policies to running workers within a
// timeout; the headless client requests them before its workers start. Each armed camera's
// entry is its orange.analytics.spatial_mask_runtime v1 policy with status and generation; a
// camera whose policy did not resolve gets a resolution-failure record.

#include "json.hpp"
#include "orange_outputs/orange_analytics_spatial_mask_recording_arm_v1.h"
#include "yolo_spatial_mask.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

namespace orange::analytics_mask {

namespace arm_schema = orange::outputs::gen::spatial_mask_recording_arm_v1;
using ArmRecord = arm_schema::OrangeAnalyticsSpatialMaskRecordingArmV1;
using ArmCameraFailure = arm_schema::OrangeAnalyticsSpatialMaskRecordingArmV1ResolutionFailure;

namespace detail {
inline ArmRecord arm_record(const Mode mode, const std::string& configuration_source,
                            const float input_context_outset_px)
{
    ArmRecord record;
    record.mode = mode_to_string(mode);
    record.configuration_source = configuration_source;
    record.input_context_outset_px = input_context_outset_px;
    record.outside_tensor_value = 0.0;
    record.has_immutable_for_recording = true;
    record.cameras.emplace();
    return record;
}
}  // namespace detail

// The GUI arms running workers, each within apply_timeout_ms. The caller sets the status.
inline ArmRecord gui_arm_record(const Mode mode, const std::string& configuration_source,
                                const float input_context_outset_px, const int apply_timeout_ms)
{
    ArmRecord record = detail::arm_record(mode, configuration_source, input_context_outset_px);
    record.apply_timeout_ms = static_cast<std::uint64_t>(apply_timeout_ms);
    return record;
}

// The headless client requests the policies before its workers start.
inline ArmRecord headless_arm_record(const Mode mode, const std::string& configuration_source,
                                     const float input_context_outset_px)
{
    ArmRecord record = detail::arm_record(mode, configuration_source, input_context_outset_px);
    record.has_application_phase = true;
    return record;
}

// A GUI start whose mask configuration did not resolve: only the status and the error.
inline ArmRecord failed_arm_record(const std::string& error)
{
    ArmRecord record;
    record.status = "failed";
    record.error = error;
    return record;
}

inline void set_arm_failed(ArmRecord* record, const std::string& error)
{
    record->status = "failed";
    record->error = error;
}

// A camera's policy with its status (resolved, armed, requested_before_worker_start) and, once
// requested or applied, its generation.
inline void set_camera_policy(ArmRecord* record, const std::string& camera_serial, const Policy& policy,
                              const std::string& status,
                              const std::optional<std::uint64_t> policy_generation = std::nullopt)
{
    PolicyRecord entry = policy_record(policy);
    entry.status = status;
    entry.policy_generation = policy_generation;
    (*record->cameras)[camera_serial] = std::move(entry);
}

// Update the status / generation of a camera whose policy is already in the record.
inline void update_camera_policy(ArmRecord* record, const std::string& camera_serial,
                                 const std::string& status, const std::uint64_t policy_generation)
{
    auto& entry = std::get<PolicyRecord>(record->cameras->at(camera_serial));
    entry.status = status;
    entry.policy_generation = policy_generation;
}

inline void set_camera_failure(ArmRecord* record, const std::string& camera_serial,
                               const std::string& error_code, const std::string& error)
{
    ArmCameraFailure failure;
    failure.error_code = error_code;
    failure.error = error;
    (*record->cameras)[camera_serial] = std::move(failure);
}

// A camera's policy entry as JSON, for the contract's cameras.<serial>.yolo_spatial_mask_runtime.
inline nlohmann::json camera_policy_json(const ArmRecord& record, const std::string& camera_serial)
{
    return runtime_schema::to_json(std::get<PolicyRecord>(record.cameras->at(camera_serial)));
}

// Store the record at analytics_runtime.yolo_spatial_mask. It is checked against the published
// schema first; a record that does not match (a writer that left its status unset, say) is
// reported, and recording_snapshot's reader refuses it later.
inline void store_arm_record(nlohmann::json* contract, const ArmRecord& record)
{
    const nlohmann::json json = arm_schema::to_json(record);
    ArmRecord check;
    std::string error;
    if (!arm_schema::parse(json, &check, &error, "analytics_runtime/yolo_spatial_mask")) {
        std::cerr << "[spatial_mask] arm record does not match "
                     "orange.analytics.spatial_mask_recording_arm v1: " << error << std::endl;
    }
    (*contract)["analytics_runtime"]["yolo_spatial_mask"] = json;
}

}  // namespace orange::analytics_mask
