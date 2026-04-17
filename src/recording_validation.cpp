#include "recording_validation.h"

#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace {

CameraRecordingValidationSummary build_camera_recording_validation_summary(
    const RecordingValidationCameraInput& camera,
    const RecordingValidationGpuPathLookup& gpu_path_lookup)
{
    CameraRecordingValidationSummary summary;
    summary.camera_index = camera.camera_index;
    summary.camera_serial = camera.camera_serial;
    summary.record_enabled = camera.record_enabled;
    summary.split_gop_enabled = camera.strategy.split_gop_enabled();
    summary.source_gpu_id = camera.source_gpu_id;
    summary.preferred_topology_class = camera.constraints.preferred_topology_class;
    summary.require_peer_access = camera.constraints.require_peer_access;
    summary.transfer_mode = camera.strategy.split_gop.transfer_mode;
    summary.source_encoder_policy = camera.strategy.split_gop.source_encoder_policy;

    if (!summary.record_enabled || !summary.split_gop_enabled) {
        return summary;
    }

    summary.claimed_gpu_ids = build_recording_claimed_gpu_ids(
        summary.source_gpu_id, camera.strategy.split_gop.encoder_gpu_ids);
    summary.helper_gpu_ids = build_recording_helper_gpu_ids(
        summary.source_gpu_id, camera.strategy.split_gop.encoder_gpu_ids);

    if (summary.helper_gpu_ids.empty()) {
        summary.local_errors.push_back(
            "No non-source helper GPU is configured for split-GOP recording.");
    } else if (summary.helper_gpu_ids.size() != 1) {
        std::ostringstream oss;
        oss << "GUI validation currently supports exactly one helper GPU; found "
            << summary.helper_gpu_ids.size() << ".";
        summary.local_errors.push_back(oss.str());
    }

    for (int helper_gpu_id : summary.helper_gpu_ids) {
        HelperGpuValidationSummary helper_summary;
        helper_summary.helper_gpu_id = helper_gpu_id;

        if (gpu_path_lookup) {
            const RecordingValidationGpuPathInfo path_info =
                gpu_path_lookup(summary.source_gpu_id, helper_gpu_id);
            helper_summary.topology_class = path_info.topology_class;
            helper_summary.topology_error = path_info.topology_error;
            helper_summary.can_access_peer = path_info.can_access_peer;
            helper_summary.can_access_peer_known = path_info.can_access_peer_known;
        }

        if (!summary.preferred_topology_class.empty() &&
            helper_summary.topology_class != summary.preferred_topology_class) {
            helper_summary.topology_matches_preference = false;
            std::ostringstream oss;
            oss << "Helper GPU " << helper_gpu_id
                << " is " << (helper_summary.topology_class.empty()
                                  ? std::string("(unknown topology)")
                                  : helper_summary.topology_class)
                << " relative to source GPU " << summary.source_gpu_id
                << "; expected " << summary.preferred_topology_class << ".";
            summary.local_errors.push_back(oss.str());
        }

        if (summary.require_peer_access &&
            (!helper_summary.can_access_peer_known || !helper_summary.can_access_peer)) {
            helper_summary.peer_requirement_satisfied = false;
            std::ostringstream oss;
            oss << "Helper GPU " << helper_gpu_id
                << " does not satisfy the peer-access requirement for source GPU "
                << summary.source_gpu_id << ".";
            summary.local_errors.push_back(oss.str());
        }

        summary.helpers.push_back(std::move(helper_summary));
    }

    return summary;
}

void populate_session_conflicts(std::vector<CameraRecordingValidationSummary>* summaries)
{
    if (!summaries) {
        return;
    }

    std::map<int, std::vector<std::size_t>> gpu_claims;
    for (std::size_t i = 0; i < summaries->size(); ++i) {
        const CameraRecordingValidationSummary& summary = (*summaries)[i];
        if (!summary.record_enabled || !summary.split_gop_enabled) {
            continue;
        }
        for (int gpu_id : summary.claimed_gpu_ids) {
            gpu_claims[gpu_id].push_back(i);
        }
    }

    for (const auto& [gpu_id, claiming_indices] : gpu_claims) {
        if (claiming_indices.size() < 2) {
            continue;
        }

        std::ostringstream cameras_oss;
        for (std::size_t i = 0; i < claiming_indices.size(); ++i) {
            if (i > 0) {
                cameras_oss << ", ";
            }
            cameras_oss << (*summaries)[claiming_indices[i]].camera_serial;
        }

        for (std::size_t summary_index : claiming_indices) {
            std::ostringstream conflict;
            conflict << "GPU " << gpu_id
                     << " is claimed by multiple record-enabled split-GOP cameras: "
                     << cameras_oss.str() << ".";
            (*summaries)[summary_index].session_errors.push_back(conflict.str());
        }
    }
}

}  // namespace

std::vector<int> build_recording_claimed_gpu_ids(const int source_gpu_id,
                                                 const std::vector<int>& encode_gpu_ids)
{
    std::vector<int> result;
    std::set<int> seen_gpu_ids;
    if (source_gpu_id >= 0 && seen_gpu_ids.insert(source_gpu_id).second) {
        result.push_back(source_gpu_id);
    }
    for (int gpu_id : encode_gpu_ids) {
        if (gpu_id < 0) {
            continue;
        }
        if (seen_gpu_ids.insert(gpu_id).second) {
            result.push_back(gpu_id);
        }
    }
    return result;
}

std::vector<int> build_recording_helper_gpu_ids(const int source_gpu_id,
                                                const std::vector<int>& encode_gpu_ids)
{
    std::vector<int> result;
    std::set<int> seen_gpu_ids;
    for (int gpu_id : encode_gpu_ids) {
        if (gpu_id < 0 || gpu_id == source_gpu_id) {
            continue;
        }
        if (seen_gpu_ids.insert(gpu_id).second) {
            result.push_back(gpu_id);
        }
    }
    return result;
}

std::vector<CameraRecordingValidationSummary> validate_recording_configuration(
    const std::vector<RecordingValidationCameraInput>& cameras,
    const RecordingValidationGpuPathLookup& gpu_path_lookup)
{
    std::vector<CameraRecordingValidationSummary> summaries;
    summaries.reserve(cameras.size());
    for (const auto& camera : cameras) {
        summaries.push_back(build_camera_recording_validation_summary(camera, gpu_path_lookup));
    }
    populate_session_conflicts(&summaries);
    return summaries;
}
