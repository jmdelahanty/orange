#pragma once

#include "json.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace orange::spatial_roi::recording {

// A recorder affinity value is an explicit Linux CPU list such as
// "3-5,35-37". Parsing is deliberately strict because the value crosses the
// parent/child exec boundary and becomes durable lifecycle metadata.
struct SpatialRoiRecorderCpuList final {
    std::vector<int> cpus;
    std::string canonical;
};

bool parse_spatial_roi_recorder_cpu_list(
    std::string_view value,
    SpatialRoiRecorderCpuList* parsed_out,
    std::string* error_out = nullptr);

struct SpatialRoiRecorderSchedulingSnapshot final {
    bool configured = false;
    std::string configuration_source;
    std::string requested_cpu_list;
    std::string canonical_requested_cpu_list;
    bool affinity_syscall_succeeded = false;
    bool affinity_applied = false;
    bool effective_mask_verified = false;
    std::vector<int> effective_cpus;
    std::string effective_cpu_list;
    std::vector<int> kernel_isolated_cpus;
    std::string kernel_isolated_cpu_list;
    bool kernel_isolation_observed = false;
    std::string kernel_isolation_observation_error;
    std::string scheduler_policy;
    int scheduler_priority = 0;
    std::string error;
};

// Call this once on the recorder control thread immediately after exec and
// before constructing any recorder workers. Linux threads created afterward
// inherit this allowed mask. An empty requested list is observation-only and
// preserves the existing inherited-affinity behavior.
bool initialize_spatial_roi_recorder_scheduling(
    const std::string& requested_cpu_list,
    const std::string& configuration_source,
    SpatialRoiRecorderSchedulingSnapshot* snapshot_out,
    std::string* error_out = nullptr);

nlohmann::json spatial_roi_recorder_scheduling_to_json(
    const SpatialRoiRecorderSchedulingSnapshot& snapshot);

}  // namespace orange::spatial_roi::recording
