#include "session/spatial_roi_recorder_camera_contract.h"

#include "session/spatial_roi_recording_config.h"

#include <set>
#include <string>
#include <utility>

namespace orange::session::spatial_roi {
namespace {

bool fail(std::string* error_out, std::string message)
{
    if (error_out) {
        *error_out = std::move(message);
    }
    return false;
}

bool same_raster(const orange::spatial_roi::SpatialRoiFrameRaster& lhs,
                const Raster& rhs) noexcept
{
    return lhs.width == rhs.width && lhs.height == rhs.height;
}

bool same_rect(const orange::spatial_roi::SpatialRoiFrameRect& lhs,
               const Rect& rhs) noexcept
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width &&
           lhs.height == rhs.height;
}

bool note_unique(std::set<std::string>* values,
                 const std::string& value,
                 const char* name,
                 std::string* error_out)
{
    if (!values->insert(value).second) {
        return fail(error_out,
                    std::string("camera-level spatial ROI contract has duplicate ") +
                        name + ": " + value);
    }
    return true;
}

}  // namespace

bool parse_spatial_roi_recorder_camera_contract(
    const nlohmann::json& candidate_contract,
    const nlohmann::json& independently_verified_plan,
    const std::string& expected_recording_root,
    const SpatialRoiRecorderRuntimeGpuMapping& expected_gpu_mapping,
    SpatialRoiRecorderCameraContractView* view_out,
    std::string* error_out)
{
    if (!view_out) {
        return fail(error_out,
                    "camera-level spatial ROI contract destination is null");
    }
    *view_out = SpatialRoiRecorderCameraContractView{};
    if (error_out) {
        error_out->clear();
    }

    try {
        // Parse the plan independently before looking at candidate contract
        // fields.  In particular, candidate stream_count, stream_order, and
        // repeated identity fields cannot choose the camera or its streams.
        SpatialRoiRecordingPlan plan;
        std::string authority_error;
        if (!parse_verified_plan(independently_verified_plan,
                                 &plan,
                                 &authority_error)) {
            return fail(error_out,
                        "camera-level spatial ROI plan is not independently verified: " +
                            authority_error);
        }
        if (plan.cameras.size() != 1) {
            return fail(error_out,
                        "camera-level spatial ROI first slice requires exactly one camera");
        }
        const auto& camera_entry = *plan.cameras.begin();
        const std::string& camera_serial = camera_entry.first;
        const SpatialRoiPlanCameraDescriptor& camera = camera_entry.second;
        if (camera.camera_serial != camera_serial) {
            return fail(error_out,
                        "verified camera map key disagrees with camera_serial");
        }
        if (camera.rois.size() != 4 || plan.admission_usage.roi_count != 4 ||
            plan.admission_usage.encoder_stream_count != 4) {
            return fail(error_out,
                        "camera-level spatial ROI first slice requires exactly four ROI streams");
        }

        // This call authenticates the complete candidate by rebuilding the
        // deterministic per-stream contract from the verified plan, root, and
        // runtime mapping.  The selected stream is only a parser selector; it
        // does not affect authentication of any other stream.
        SpatialRoiRecorderContractView authenticated;
        if (!parse_spatial_roi_recorder_contract(
                candidate_contract,
                independently_verified_plan,
                expected_recording_root,
                expected_gpu_mapping,
                camera.rois.front().logical_stream_id,
                &authenticated,
                &authority_error)) {
            return fail(error_out,
                        "camera-level spatial ROI contract authentication failed: " +
                            authority_error);
        }

        if (authenticated.stream_count != 4 ||
            authenticated.stream_order.size() != 4 ||
            authenticated.streams.size() != 4) {
            return fail(error_out,
                        "authenticated camera-level contract does not contain exactly four streams");
        }
        if (authenticated.recording_id != plan.recording_id ||
            authenticated.session_id != plan.recording_id ||
            authenticated.recording_identity_token !=
                plan.recording_identity_token ||
            authenticated.producer_generation != plan.producer_generation ||
            authenticated.spatial_roi_plan_sha256 != plan.plan_sha256 ||
            authenticated.recording_root != expected_recording_root) {
            return fail(error_out,
                        "authenticated camera-level contract has inconsistent shared identity or root");
        }
        if (authenticated.analytics_gpu_by_camera_serial.size() != 1 ||
            authenticated.analytics_gpu_by_camera_serial.at(camera_serial) !=
                expected_gpu_mapping.analytics_gpu_by_camera_serial.at(
                    camera_serial)) {
            return fail(error_out,
                        "authenticated camera-level contract has an invalid analytics GPU assignment");
        }
        if (authenticated.recorder_gpu_by_logical_stream_id.size() != 4) {
            return fail(error_out,
                        "authenticated camera-level contract must map four recorder GPUs");
        }

        std::set<std::string> logical_stream_ids;
        std::set<std::string> roi_ids;
        std::set<std::string> region_ids;
        std::set<std::string> socket_paths;
        std::set<std::string> artifact_absolute_paths;
        std::set<std::string> artifact_relative_paths;
        for (std::size_t index = 0; index < authenticated.streams.size(); ++index) {
            const SpatialRoiRecorderStreamView& stream =
                authenticated.streams[index];
            const SpatialRoiPlanRoiDescriptor& roi = camera.rois[index];

            // stream_order is authenticated by the deterministic parser.  The
            // explicit check here makes the order guarantee visible at this
            // process-level boundary and protects future parser refactors.
            if (authenticated.stream_order[index] != stream.logical_stream_id ||
                stream.logical_stream_id != roi.logical_stream_id) {
                return fail(error_out,
                            "authenticated stream vector is not in verified plan order");
            }
            if (stream.stream_kind != "spatial_roi" ||
                stream.output_kind != "spatial_roi" ||
                stream.geometry.source_coordinate_space !=
                    "camera_native_full_frame_pixels" ||
                stream.geometry.video_coordinate_space !=
                    "spatial_roi_encoded_pixels") {
                return fail(error_out,
                            "camera-level first slice accepts only fixed-region spatial ROI streams");
            }
            if (stream.camera_id != camera.camera_id ||
                stream.camera_serial != camera.camera_serial ||
                stream.arena_group_id != camera.arena_group_id ||
                !same_raster(stream.geometry.native_raster,
                             camera.native_raster) ||
                !same_rect(stream.geometry.content_rect, roi.source_rect) ||
                !same_raster(stream.geometry.encoded_raster,
                             roi.encoded_raster) ||
                !same_rect(stream.geometry.encoded_content_rect,
                           roi.encoded_content_rect)) {
                return fail(error_out,
                            "camera-level stream camera identity or geometry disagrees with the verified plan");
            }
            if (stream.analytics_gpu_id !=
                    authenticated.analytics_gpu_by_camera_serial.at(
                        camera_serial) ||
                stream.source_gpu_id != stream.analytics_gpu_id ||
                stream.recorder_gpu_id !=
                    authenticated.recorder_gpu_by_logical_stream_id.at(
                        stream.logical_stream_id) ||
                stream.assigned_gpu_id != stream.recorder_gpu_id) {
                return fail(error_out,
                            "camera-level stream GPU assignment disagrees with authenticated mapping");
            }
            if (!note_unique(&logical_stream_ids,
                             stream.logical_stream_id,
                             "logical_stream_id",
                             error_out) ||
                !note_unique(&roi_ids, stream.roi_id, "roi_id", error_out) ||
                !note_unique(&region_ids,
                             stream.region_id,
                             "region_id",
                             error_out) ||
                !note_unique(&socket_paths,
                             stream.socket_path,
                             "socket_path",
                             error_out)) {
                return false;
            }

            // The per-stream parser has already checked that these are the
            // exact deterministic artifact set.  Recheck global disjointness
            // because this is the ownership boundary for one process and four
            // independent output cores.
            for (const auto& [kind, artifact] : stream.artifacts) {
                (void)kind;
                if (!note_unique(&artifact_absolute_paths,
                                 artifact.absolute_path,
                                 "absolute artifact path",
                                 error_out) ||
                    !note_unique(&artifact_relative_paths,
                                 artifact.relative_path,
                                 "relative artifact path",
                                 error_out)) {
                    return false;
                }
            }
        }
        if (logical_stream_ids.size() != 4 || roi_ids.size() != 4 ||
            region_ids.size() != 4 || socket_paths.size() != 4 ||
            artifact_absolute_paths.empty() ||
            artifact_absolute_paths.size() != artifact_relative_paths.size()) {
            return fail(error_out,
                        "camera-level spatial ROI identities or artifacts are not unique and disjoint");
        }

        SpatialRoiRecorderCameraContractView view;
        view.schema_id = kSpatialRoiRecorderCameraContractSchemaId;
        view.schema_version = kSpatialRoiRecorderCameraContractSchemaVersion;
        view.product_kind = kSpatialRoiRecorderCameraProductKind;
        view.recording_id = authenticated.recording_id;
        view.session_id = authenticated.session_id;
        view.recording_identity_token = authenticated.recording_identity_token;
        view.producer_generation = authenticated.producer_generation;
        view.spatial_roi_plan_sha256 = authenticated.spatial_roi_plan_sha256;
        view.recording_root = authenticated.recording_root;
        view.artifact_root = authenticated.artifact_root;
        view.camera_id = camera.camera_id;
        view.camera_serial = camera.camera_serial;
        view.native_raster = {camera.native_raster.width,
                              camera.native_raster.height};
        view.analytics_gpu_id =
            authenticated.analytics_gpu_by_camera_serial.at(camera_serial);
        view.stream_count = authenticated.stream_count;
        view.stream_order = authenticated.stream_order;
        view.ipc_v2 = authenticated.ipc_v2;
        view.aggregate_bounds = authenticated.aggregate_bounds;
        view.storage_preflight_policy = authenticated.storage_preflight_policy;
        view.analytics_gpu_by_camera_serial =
            authenticated.analytics_gpu_by_camera_serial;
        view.recorder_gpu_by_logical_stream_id =
            authenticated.recorder_gpu_by_logical_stream_id;
        view.streams = authenticated.streams;
        *view_out = std::move(view);
        return true;
    } catch (const std::exception& ex) {
        return fail(error_out,
                    std::string("camera-level spatial ROI contract parse failed: ") +
                        ex.what());
    } catch (...) {
        return fail(error_out,
                    "camera-level spatial ROI contract parse failed: unknown exception");
    }
}

}  // namespace orange::session::spatial_roi
