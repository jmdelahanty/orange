#include "recording_crop_only_manifest.h"
#include "recording_master_acquisition.h"
#include "recording_master_crop_coverage.h"
#include "recording_media_plan.h"
#include "recording_registered_context.h"
#include "spatial_roi_session_authority_store.h"
#include "gui/spatial_layout/sha256.h"
#include "fsuid_guard.h"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace orange::recording {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;
namespace custody = orange::session::spatial_roi;
namespace checksum = orange::gui::spatial_layout::checksum;
void need(bool ok, const std::string& reason) { if (!ok) throw std::runtime_error(reason); }
struct Document { json value, reference; };
Document read(const fs::path& root, const std::string& name) {
    std::unique_ptr<custody::SpatialRoiSessionAuthorityStore> store;
    std::string error, digest, bytes;
    need(custody::SpatialRoiSessionAuthorityStore::OpenExisting(root, {name}, &store, &error), error);
    // JSON evidence only; video hashing/validation stays in the shared media gate.
    const auto size = fs::file_size(root / name);
    need(size <= 16 * 1024 * 1024, "crop-only JSON evidence exceeds limit");
    need(checksum::file_sha256(root / name, &digest, &error), error);
    custody::SpatialRoiSessionAuthorityReceipt ref{name, size, digest};
    need(store->ReadAndVerify(ref, &bytes, nullptr, &error), error);
    return {json::parse(bytes), ref.ToJson()};
}
std::set<std::string> camera_set(const json& cameras) {
    need(cameras.is_array() && !cameras.empty() && cameras.size() <= 64, "crop-only logical camera set invalid");
    std::set<std::string> result;
    for (const auto& item : cameras) {
        need(item.is_string(), "crop-only camera serial must be a string");
        const auto serial = item.get<std::string>();
        need(!serial.empty() && serial.size() <= 128 &&
            serial.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-") == std::string::npos &&
            result.insert(serial).second, "crop-only camera serial invalid or duplicated");
    }
    return result;
}
json camera_evidence(const json& gate, const std::string& serial) {
    json result;
    for (const auto& item : gate.at("cameras")) if (item.at("camera_serial") == serial) {
        need(result.is_null(), "duplicate crop-only camera evidence"); result = item;
    }
    need(!result.is_null(), "required crop-only camera evidence missing");
    return result;
}
void no_media(const json& envelope) {
    for (const auto* key : {"camera_artifacts", "recording_outputs"})
        need(!envelope.contains(key) || envelope.at(key).empty(), "crop-only lifecycle must not contain prebuilt media artifacts");
    for (const auto& clip : envelope.value("clips", json::array())) {
        for (const auto* key : {"camera_artifacts", "recording_outputs"})
            need(!clip.contains(key) || clip.at(key).empty(), "crop-only lifecycle contains existing clip media");
        const auto artifacts = clip.value("artifacts", json::object());
        for (const auto& artifact : artifacts.items())
            need(artifact.value().empty(), "crop-only lifecycle contains full-frame artifact paths");
    }
}
json output(const json& clip, const json& media) {
    return {{"schema_version", 2}, {"camera_serial", clip.at("camera_serial")}, {"output_kind", "crop"},
        {"role", "runtime_derived_acquisition_input"}, {"backend", "external_ipc"}, {"status", "completed"},
        {"video", clip.at("video").at("relative_path")}, {"metadata", clip.at("crop_metadata").at("relative_path")},
        {"width", media.at("width")}, {"height", media.at("height")}, {"codec", "hevc"}, {"container", "mp4"},
        {"tuning", "lossless"}, {"frame_count", clip.at("frame_count")},
        {"first_recording_frame_id", clip.at("first_recording_frame_id")}, {"last_recording_frame_id", clip.at("last_recording_frame_id")},
        {"recording_frame_id_gaps", 0}, {"packet_count", clip.at("frame_count")}, {"packet_count_source", "validated_crop_media_receipt"},
        {"coordinate_space", "full_frame_pixels"}, {"video_pixel_coordinate_space", "crop_frame_pixels"},
        {"source_geometry_coordinate_space", "full_frame_pixels"}};
}
json publish(custody::SpatialRoiSessionAuthorityStore& store, const std::string& name, const std::string& bytes) {
    custody::SpatialRoiSessionAuthorityReceipt ref;
    std::string error;
    need(store.PublishBytes(name, bytes, &ref, &error), error);
    return ref.ToJson();
}
std::string quote(const json& value) {
    const auto text = value.is_string() ? value.get<std::string>() : value.dump();
    std::string result = "\"";
    for (char c : text) { if (c == '"') result += '"'; result += c; }
    return result + '"';
}
}

bool IsCropOnlyRecordingManifest(const json& j) {
    return j.is_object() && j.value("media_product_mode", std::string()) == kCropOnlyProduct;
}

json BuildCropOnlyRecordingManifest(const fs::path& root, const json& lifecycle) {
    ScopedFsuid fsuid_guard;
    need(IsCropOnlyRecordingManifest(lifecycle), "crop-only lifecycle product mismatch");
    need(lifecycle.at("schema_id") == "orange.recording_session" && lifecycle.at("schema_version") == 1,
         "crop-only lifecycle schema mismatch");
    const auto status = lifecycle.at("status").get<std::string>();
    need(status == "completed" || status == "failed" || status == "interrupted" || status == "incomplete",
         "crop-only lifecycle must be terminal");
    need(!lifecycle.contains("crop_only_inventory") && !lifecycle.contains("crop_clip_index"),
         "crop-only lifecycle already contains an inventory/index");
    no_media(lifecycle);
    const auto cameras = camera_set(lifecycle.at("cameras"));
    need(lifecycle.at("session_id") == root.filename().string(), "crop-only parent identity mismatch");
    const auto mode = lifecycle.at("mode").get<std::string>();
    need(mode == "single_clip" || mode == "rolling_clips", "crop-only clip mode invalid");
    json parent = lifecycle;
    parent["camera_artifacts"] = json::object(); // Reserved for actual full-frame outputs.
    parent["recording_outputs"] = json::object();
    parent["clips"] = json::array();
    parent["clip_index_scope"] = "camera_stream";
    // Full-frame v1 CSV aliases and clock seals are not claims about this source.
    for (const auto* key : {"frame_identity_contract", "acquisition_index_mapping", "acquisition_index_mapping_sha256", "timestamp_clock_contract"})
        need(!lifecycle.contains(key), "crop-only lifecycle contains a full-frame authority contract");
    json inventory = {{"schema_id", "orange.recording.crop_only_inventory"}, {"schema_version", 1},
        {"recording_id", parent.at("session_id")}, {"mode", mode}, {"status", "failed"},
        {"path_base", "parent_recording_directory"}, {"clip_index_scope", "camera_stream"}, {"cameras", json::object()}};
    try {
        const auto start = read(root, "recording_snapshot_start.json");
        const auto& session = start.value.at("session");
        const auto& plan = session.at("recording_media_plan");
        auto selection = RecordingMediaSelection::Parse(plan.at("selection"));
        need(selection.RequiresContext(), "immutable start did not select crop-only");
        std::vector<RecordingMediaCameraInput> planned;
        json planned_serials = json::array();
        for (const auto& item : plan.at("cameras")) {
            const auto serial = item.at("camera_serial").get<std::string>();
            planned.push_back({serial, true, true}); planned_serials.push_back(serial);
        }
        need(plan == RecordingMediaPlan::Resolve(selection, planned).ToJson() && camera_set(planned_serials) == cameras,
            "crop-only membership differs from immutable media plan");
        for (const auto* key : {"master_frame_journal", "registered_scene_context", "moving_crop_master_coverage", "moving_crop_encoded_media"})
            need(session.at(key).at("required") == true, "crop-only required product marker missing");
        // Evaluate evidence even for an interrupted/failed lifecycle, without
        // promoting its parent status. Preserve valid products as evidence only.
        json proof = {{"session_id", parent.at("session_id")}, {"status", "completed"}};
        ApplyRequiredMasterJournalGate(root, &proof);
        ApplyRequiredMovingCropMetadataGate(root, &proof);
        ApplyRequiredMovingCropMediaGate(root, &proof);
        ApplyRequiredRegisteredContextGate(root, &proof);
        need(proof.at("status") == "completed" && proof.at("master_frame_journal").at("status") == "complete" &&
            proof.at("moving_crop_encoded_media").at("status") == "complete" &&
            proof.at("registered_scene_context").at("status") == "captured", "crop-only required source/context/media evidence incomplete");
        json master_serials = json::array();
        for (const auto& item : proof.at("master_frame_journal").at("cameras")) master_serials.push_back(item.at("camera_serial"));
        need(camera_set(master_serials) == cameras, "crop-only master camera set differs from planned membership");
        json clips = json::array(), outputs = json::object(), bindings = json::object();
        for (const auto& serial : cameras) {
            const auto master = camera_evidence(proof.at("master_frame_journal"), serial);
            const auto& context_gate = proof.at("registered_scene_context");
            const auto context = context_gate.at("profile") == "daily_registered_context_reuse_v1"
                ? json{{"camera_serial", serial}, {"profile", context_gate.at("profile")},
                    {"daily_context", context_gate.at("daily_context")}, {"use_receipt", context_gate.at("use_receipt")}}
                : camera_evidence(context_gate, serial);
            const auto media_ref = camera_evidence(proof.at("moving_crop_encoded_media"), serial).at("receipt");
            const auto media = read(root, media_ref.at("relative_path").get<std::string>());
            need(media.reference == media_ref && media.value.at("mode") == mode, "crop-only media receipt/mode mismatch");
            const auto descriptor = read(root, master.at("descriptor").at("relative_path").get<std::string>());
            need(descriptor.reference == master.at("descriptor"), "crop-only master changed");
            json per_camera = json::array();
            for (const auto& video : media.value.at("videos")) {
                json clip = video;
                clip["schema_id"] = "orange.recording.moving_crop_clip"; clip["schema_version"] = 1;
                clip["recording_id"] = parent.at("session_id"); clip["camera_serial"] = serial;
                clip["path_base"] = "parent_recording_directory";
                clip["clip_index_scope"] = "camera_stream";
                clip["first_video_frame_index"] = 0;
                clip["last_video_frame_index"] = video.at("frame_count").get<uint64_t>() - 1;
                clip["master_descriptor"] = master.at("descriptor");
                clip["media_receipt"] = media_ref;
                clip["registered_context"] = context;
                clip["recording_outputs"] = {{serial, {{"crop", output(clip, media.value)}}}};
                per_camera.push_back(clip); clips.push_back(clip);
            }
            bindings[serial] = {{"master", master}, {"registered_context", context}, {"media_receipt", media_ref},
                {"assigned_frame_count", media.value.at("frame_count")}, {"clips", per_camera}};
            // No session-wide video path exists in this collection, including
            // when it currently contains only one clip. A consumer must choose
            // the explicitly indexed video and its own local frame domain.
            outputs[serial]["crop"] = {{"schema_version", 3}, {"camera_serial", serial}, {"output_kind", "crop"},
                {"role", "runtime_derived_acquisition_input"}, {"backend", "external_ipc"}, {"representation", "clip_collection"},
                {"status", "completed"}, {"frame_count", media.value.at("frame_count")}, {"clips", per_camera}};
        }
        need(start.reference == read(root, "recording_snapshot_start.json").reference, "crop-only immutable start changed");
        inventory["status"] = "complete"; inventory["start_snapshot"] = start.reference;
        inventory["cameras"] = std::move(bindings);
        parent["clips"] = std::move(clips); parent["recording_outputs"] = std::move(outputs);
        for (const auto* key : {"master_frame_journal", "registered_scene_context", "moving_crop_master_coverage", "moving_crop_encoded_media"})
            parent[key] = proof.at(key);
    } catch (const std::exception& ex) {
        inventory["reason"] = ex.what(); parent["status"] = "failed";
    }
    parent["crop_only_inventory"] = std::move(inventory);
    return parent;
}

void PublishCropOnlyRecordingIndex(const fs::path& root, json* parent) {
    need(parent && IsCropOnlyRecordingManifest(*parent), "invalid crop-only index parent");
    if (parent->at("crop_only_inventory").at("status") != "complete" || parent->at("status") != "completed") return;
    ScopedFsuid fsuid_guard;
    // Re-resolve before publication: neither a changed artifact nor edited
    // in-memory clip paths may enter a successful immutable index. This runs
    // on the finalization worker, never on acquisition/encoder threads.
    json envelope;
    for (const auto* key : {"schema_id", "schema_version", "media_product_mode", "session_id", "status", "mode", "cameras"})
        envelope[key] = parent->at(key);
    const auto verified = BuildCropOnlyRecordingManifest(root, envelope);
    need(verified.at("status") == "completed", "crop-only evidence changed before index publication");
    for (const auto* key : {"crop_only_inventory", "clips", "recording_outputs", "camera_artifacts", "clip_index_scope"})
        need(parent->at(key) == verified.at(key), "crop-only index differs from verified inventory");
    std::vector<std::string> names{"recording_crop_clip_index_v1.json", "recording_crop_clip_index_v1.csv"};
    for (const auto& clip : parent->at("clips")) names.push_back("Cam" + clip.at("camera_serial").get<std::string>() +
        "_crop_clip_" + std::to_string(clip.at("clip_index").get<uint64_t>()) + "_manifest_v1.json");
    std::unique_ptr<custody::SpatialRoiSessionAuthorityStore> store;
    std::string error;
    need(custody::SpatialRoiSessionAuthorityStore::OpenExisting(root, names, &store, &error), error);
    json records = json::array();
    std::string csv = "recording_id,camera_serial,clip_index,clip_id,frame_count,first_recording_frame_id,last_recording_frame_id,first_video_frame_index,last_video_frame_index,video,crop_metadata,clip_manifest\n";
    std::size_t i = 2;
    for (const auto& clip : parent->at("clips")) {
        const auto& name = names.at(i++);
        const auto ref = publish(*store, name, clip.dump());
        records.push_back({{"camera_serial", clip.at("camera_serial")}, {"clip_index", clip.at("clip_index")},
            {"clip_id", clip.at("clip_id")}, {"manifest", ref}});
        for (const auto* key : {"recording_id", "camera_serial", "clip_index", "clip_id", "frame_count", "first_recording_frame_id",
                "last_recording_frame_id", "first_video_frame_index", "last_video_frame_index"}) csv += quote(clip.at(key)) + ',';
        csv += quote(clip.at("video").at("relative_path")) + ',' + quote(clip.at("crop_metadata").at("relative_path")) + ',' + quote(name) + '\n';
    }
    const auto csv_ref = publish(*store, names.at(1), csv);
    const json index = {{"schema_id", "orange.recording.moving_crop_clip_index"}, {"schema_version", 1},
        {"recording_id", parent->at("session_id")}, {"mode", parent->at("mode")},
        {"path_base", "parent_recording_directory"}, {"clip_index_scope", "camera_stream"},
        {"inventory", parent->at("crop_only_inventory")}, {"clips", records}, {"csv", csv_ref}};
    const auto ref = publish(*store, names.at(0), index.dump());
    (*parent)["crop_clip_index"] = {{"schema_version", 1}, {"json", ref}, {"csv", csv_ref}};
}
}
