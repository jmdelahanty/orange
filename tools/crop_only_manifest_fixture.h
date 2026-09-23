#pragma once
#include "moving_crop_completion_fixture.h"
#include "recording_crop_only_manifest.h"
#include "recording_media_plan.h"
#include "recording_registered_context.h"
#include "gui/spatial_layout/sha256.h"
#include "daily_registered_context_fixture.h"

namespace orange::test_crop_media {
struct CropOnlyFixture {
    Fixture first;
    std::unique_ptr<Fixture> second;
    json start, lifecycle;
    RegisteredContextSet contexts;
    explicit CropOnlyFixture(bool rolling, bool two_cameras = false, bool daily = false)
        : first(rolling ? 7 : 5) {
        first.Media(rolling); first.Finish(); FinalizeMovingCropMedia(first.root, first.serial, 256, 256);
        if (two_cameras) {
            second = std::make_unique<Fixture>(rolling ? 10 : 5, "02010094", first.root, "second-");
            second->Media(rolling); second->Finish(); FinalizeMovingCropMedia(first.root, second->serial, 256, 256);
        }
        std::vector<RegisteredContextCamera> cameras;
        std::vector<RecordingMediaCameraInput> planned;
        json serials = json::array(), masters = json::array(), bindings = json::array();
        json geometry = {{"schema_id", "orange.recording.geometry_contract"}, {"schema_version", 1}, {"cameras", json::object()}};
        for (Fixture* f : {&first, second.get()}) if (f) {
            const uint64_t id = cameras.size();
            cameras.push_back({f->serial, f->master.at("producer_instance_id"), id,
                f->master.at("stream_generation"), 8, 4});
            planned.push_back({f->serial, true, true}); serials.push_back(f->serial);
            masters.push_back({{"camera_serial", f->serial}, {"recording_id", f->master.at("recording_id")},
                {"producer_instance_id", f->master.at("producer_instance_id")}, {"stream_generation", f->master.at("stream_generation")},
                {"descriptor_relative_path", f->prefix + "_master_frames_v1.json"}});
            bindings.push_back({{"acquisition_camera_id", f->serial}, {"camera_serial", f->serial}, {"shaman_numeric_camera_id", id}});
            geometry["cameras"][f->serial]["physical_registration"] = {{"status", "selected_resolved"}, {"mode", "selected_physical_registration"},
                {"recording_snapshot_entry", {{"artifact_id", "accepted-rim"}, {"camera_serial", f->serial},
                    {"camera", {{"width", 8}, {"height", 4}, {"pixel_format", "Mono8"}}}}}};
        }
        const json declaration = {{"schema_id", "orange.recording.registered_scene_context.capture_declaration"}, {"schema_version", 1},
            {"registration_authority_status", "accepted_for_experiment"}, {"subject_presence", "present"},
            {"dish_setup_complete", true}, {"nir_illumination_fixed", true}, {"camera_configuration_fixed", true}, {"rig_fixed", true}};
        if (!daily) {
        contexts.Prepare(RegisteredContextConfig::Parse({{"schema_version", 1}, {"enabled", true},
            {"worker_cpu_ids", {0}}, {"declaration", declaration}}), first.root, cameras, geometry);
        for (const auto& c : cameras) {
            RegisteredContextFrame frame; frame.camera_serial = c.serial; frame.width = 8; frame.height = 4;
            frame.local_frame_id = 7000; frame.camera_frame_id = 14000;
            frame.camera_timestamp_ns = 1699999999999999999ULL; frame.timestamp_sys_ns = 1700000000000000099ULL;
            frame.mono8.resize(32, 45); contexts.Accept(frame);
        }
        }
        RecordingMediaSelection selection; selection.mode = RecordingMediaMode::RegisteredContextAndMovingCrops;
        const json identity = {{"schema_id", "orange.shaman_v2.camera_identity"}, {"schema_version", 1},
            {"canonicalization", "canonical_json_utf8_sort_keys_compact_v1"}, {"recording_id", first.root.filename().string()},
            {"camera_bindings", bindings}};
        start = {{"shaman_v2_camera_identity", identity},
            {"shaman_v2_camera_identity_sha256", "sha256:" + orange::gui::spatial_layout::checksum::sha256_hex(identity.dump())},
            {"session", {{"recording_media_plan", RecordingMediaPlan::Resolve(selection, planned).ToJson()},
                {"master_frame_journal", {{"schema_version", 1}, {"required", true}, {"profile", "headless_acquisition_loop_v1"}, {"cameras", masters}}},
                {"moving_crop_master_coverage", {{"schema_version", 1}, {"required", true}, {"profile", "external_moving_crop_full_rate_v1"}}},
                {"moving_crop_encoded_media", {{"schema_version", 1}, {"required", true}, {"profile", "returned_identity_v2_mux_and_full_hevc_decode_v1"}}},
                {"registered_scene_context", daily ? json::object() : contexts.StartEvidence()}}}};
        SaveStart();
        lifecycle = {{"schema_id", "orange.recording_session"}, {"schema_version", 1}, {"producer", "crop-only-fixture"},
            {"media_product_mode", kCropOnlyProduct}, {"session_id", first.root.filename().string()}, {"status", "completed"},
            {"mode", rolling ? "rolling_clips" : "single_clip"}, {"cameras", serials},
            {"recording_control", {{"clip_seconds", rolling ? 1 : 0}}},
            {"clips", json::array({{{"artifacts", {{"video", json::object()}, {"metadata", json::object()},
                {"keyframe", json::object()}}}, {"camera_artifacts", json::object()}, {"recording_outputs", json::object()}}})}};
        if (daily) UseDailyContext();
    }
    void SaveStart() {
        put(first.root / "recording_snapshot_start.json", start.dump());
        put(first.root / "recording_snapshot.json", start.dump());
    }
    void UseDailyContext() {
        check(second != nullptr, "daily fixture expects two cameras");
        orange::test_daily_context::Fixture daily;
        const auto ref = orange::calibration::WriteDailyRegisteredContext(daily.plan, daily.frames);
        const auto config = RegisteredContextConfig::Parse({{"schema_version", 2}, {"enabled", true},
            {"worker_cpu_ids", {0}}, {"declaration", daily.plan.declaration},
            {"source", {{"kind", "daily_registration"}, {"descriptor_path", (daily.plan.output_root / "context.json").string()},
                {"size_bytes", ref.at("size_bytes")}, {"sha256", ref.at("sha256")}, {"scene_unchanged_since_capture", true}}}});
        std::vector<RegisteredContextCamera> cameras;
        for (Fixture* f : {&first, second.get()}) cameras.push_back({f->serial, f->master.at("producer_instance_id"),
            daily.plan.cameras.at(f->serial).at("camera_id"), f->master.at("stream_generation"), 8, 4, daily.plan.cameras.at(f->serial)});
        RegisteredContextSet reused;
        reused.Prepare(config, first.root, cameras, daily.plan.geometry);
        start["session"]["registered_scene_context"] = reused.StartEvidence(); SaveStart();
        // Original daily asset is removed by its fixture destructor. Completion
        // must rely only on the recording-local archive and its use receipt.
    }
};
} // namespace orange::test_crop_media
