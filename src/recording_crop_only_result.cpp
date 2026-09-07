#include "recording_crop_only_result.h"
#include "recording_crop_only_manifest.h"
#include <cmath>
#include <stdexcept>
namespace orange::recording {
void EvaluateCropOnlyCameraResult(const std::filesystem::path& root, const std::string& serial,
        const CropOnlyResultPolicy& policy, nlohmann::json* row) {
    if (!row) throw std::runtime_error("null crop-only result row");
    auto& r = *row;
    r["media_product_mode"] = kCropOnlyProduct;
    r["recording_result_profile"] = "master_bound_moving_crop_collection_v1";
    r["full_frame_encoder_settings_applicable"] = false;
    r["video_present"] = false; r["video_path"] = "";
    r["video_content_checked"] = false; r["video_content_valid"] = false;
    r["video_content_status"] = "not_selected";
    r["crop_media_status"] = "failed";
    r["crop_frame_count"] = 0;
    r["crop_clip_count"] = 0;
    for (const auto* key : {"crop_clip_index", "crop_recording_output", "master_frame_record",
            "registered_context", "crop_media_validation_profile"}) r.erase(key);
    try {
        const auto parent = ReadVerifiedCropOnlyRecordingManifest(root);
        const auto& binding = parent.at("crop_only_inventory").at("cameras").at(serial);
        r["crop_frame_count"] = binding.at("assigned_frame_count");
        r["crop_clip_count"] = binding.at("clips").size();
        r["crop_clip_index"] = parent.at("crop_clip_index");
        r["crop_recording_output"] = parent.at("recording_outputs").at(serial).at("crop");
        r["master_frame_record"] = binding.at("master");
        r["registered_context"] = binding.at("registered_context");
        r["crop_media_validation_profile"] = "returned_identity_v2_mux_and_full_hevc_decode_v1";
        r["crop_media_status"] = "complete";
        r["status"] = "completed";
        r["pass_fail"] = "pass";
        r["reason"] = "complete master-bound moving crop collection";
        if ((policy.zero_camera_drops && r.value("camera_frame_id_gaps", int64_t(0)) > 0) ||
            (policy.zero_acquisition_starvation && r.value("acq_starve_final", uint64_t(0)) > 0) ||
            (policy.zero_preprocess_drops && r.value("pre_drops_final", uint64_t(0)) > 0) ||
            r.value("get_frame_errors_final", uint64_t(0)) > 0) {
            r["pass_fail"] = "fail"; r["reason"] = "crop-only acquisition counter policy failed";
        } else {
            const double fps = r.value("acq_fps_mean", 0.0);
            if (!std::isfinite(fps) || fps <= 0 ||
                (policy.target_fps > 0 && fps + policy.target_fps * policy.tolerance_percent / 100 < policy.target_fps)) {
                r["pass_fail"] = "marginal"; r["reason"] = "acquisition fps below target tolerance or unavailable";
            }
        }
    } catch (const std::exception& ex) {
        r["status"] = "failed"; r["pass_fail"] = "fail";
        r["reason"] = std::string("crop-only recording validation: ") + ex.what();
    }
}
}
