#include "daily_registered_context.h"
#include "fsuid_guard.h"
#include "gui/spatial_layout/daily_registration_runtime_selection.h"
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace orange::calibration {
namespace fs = std::filesystem;
namespace authority = session::spatial_roi;
using json = nlohmann::json;
namespace {
void require(bool ok, const std::string& reason) { if (!ok) throw std::runtime_error("daily context: " + reason); }
uint64_t number(const json& value) {
    require(value.is_number_unsigned() || (value.is_number_integer() && value.get<int64_t>() >= 0), "unsigned integer required");
    return value.get<uint64_t>();
}
void safe_id(const std::string& value) {
    require(!value.empty() && value.size() <= 128 &&
        value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos, "unsafe identity");
}
std::set<std::string> camera_set(const json& cameras) {
    require(cameras.is_object() && !cameras.empty() && cameras.size() <= 64, "invalid camera set");
    std::set<std::string> result;
    for (const auto& camera : cameras.items()) { safe_id(camera.key()); result.insert(camera.key()); }
    return result;
}
}
void ValidateDailyContextSelection(const json& status, const fs::path& registration,
                                  const std::string& sha256, const json& cameras) {
    const auto assessment = gui::spatial_layout::assess_daily_registration_runtime_selection(
        status, "", registration.string(), sha256, true);
    require(assessment.disposition == gui::spatial_layout::DailyRegistrationRuntimeSelectionDisposition::kSelectedValid,
            "exact accepted registration is not selected and applied: " + assessment.error);
    std::set<std::string> selected;
    for (const auto& target : status.at("runtime").at("targets"))
        require(selected.insert(target.at("camera_id").get<std::string>()).second, "ambiguous selected camera");
    require(selected == camera_set(cameras), "selected runtime camera set differs from capture");
}
json WriteDailyRegisteredContext(const DailyContextPlan& plan, const std::vector<DailyContextFrame>& frames) {
    const auto expected = camera_set(plan.cameras);
    safe_id(plan.capture_id);
    require(plan.output_root.is_absolute() && plan.output_root.lexically_normal() == plan.output_root &&
            plan.output_root.filename() == plan.capture_id &&
            !plan.requested_at_utc.empty(), "invalid output root/capture identity/time");
    // Reuse the closed scene declaration and strict integer/CPU admission.
    recording::RegisteredContextConfig::Parse({{"schema_version", 1}, {"enabled", true},
        {"worker_cpu_ids", {plan.housekeeping_cpu}}, {"declaration", plan.declaration}});
    ValidateDailyContextSelection(plan.runtime_before, plan.registration_path, plan.registration_sha256, plan.cameras);
    ValidateDailyContextSelection(plan.runtime_after, plan.registration_path, plan.registration_sha256, plan.cameras);
    require(plan.registration_path.is_absolute() && fs::is_regular_file(fs::symlink_status(plan.registration_path)),
            "accepted registration file missing or aliased");
    ScopedFsuid guard;
    std::string error, registration_bytes;
    std::unique_ptr<authority::SpatialRoiSessionAuthorityStore> source_store;
    const auto leaf = plan.registration_path.filename().string();
    require(authority::SpatialRoiSessionAuthorityStore::OpenExisting(plan.registration_path.parent_path(), {leaf}, &source_store, &error), error);
    const authority::SpatialRoiSessionAuthorityReceipt source_ref{leaf, fs::file_size(plan.registration_path), plan.registration_sha256};
    require(source_store->ReadAndVerify(source_ref, &registration_bytes, nullptr, &error), error);
    const auto registration = json::parse(registration_bytes);
    require(registration.at("schema_id") == "citrus.calibration.daily_registration" &&
        number(registration.at("schema_version")) == 1 && registration.at("status") == "accepted" &&
        registration.at("registration_id").is_string() && !registration.at("registration_id").get<std::string>().empty(),
        "source is not an accepted registration");
    std::set<std::string> registered;
    for (const auto& target : registration.at("targets"))
        require(registered.insert(target.at("camera_id").get<std::string>()).second, "duplicate registration camera");
    require(registered == expected, "accepted registration camera membership differs");
    require(plan.geometry.at("schema_id") == "orange.recording.geometry_contract" && number(plan.geometry.at("schema_version")) == 1,
        "unsupported frozen geometry");
    require(frames.size() == expected.size(), "incomplete source camera set");
    std::set<std::string> seen;
    uint64_t total_bytes = 0;
    for (const auto& frame : frames) {
        const auto& source = frame.source;
        require(expected.count(source.camera_serial) && seen.insert(source.camera_serial).second, "unexpected or duplicate source");
        const auto& settings = plan.cameras.at(source.camera_serial);
        require(settings.is_object() && settings.size() == 9, "unknown or missing camera settings");
        for (const auto* key : {"camera_id", "width", "height", "frame_rate_hz", "exposure_us", "gain", "focus", "iris"}) number(settings.at(key));
        require(settings.at("pixel_format") == "Mono8" && source.width > 0 && source.height > 0 &&
            settings.at("width") == source.width && settings.at("height") == source.height, "native raster mismatch");
        const uint64_t bytes = static_cast<uint64_t>(source.width) * source.height;
        require(bytes <= 64 * 1024 * 1024 && source.mono8.size() == bytes &&
            (total_bytes += bytes) <= 256 * 1024 * 1024, "native byte count or capture budget exceeded");
        require(source.recording_frame_id == 0 && source.local_frame_id > 0 && source.camera_timestamp_ns > 0 && source.timestamp_sys_ns > 0,
            "context is not one identified pre-recording frame");
        require(frame.source_storage == "analytics_owned_device" || frame.source_storage == "pool_owned_ring_device", "unowned source storage");
        const auto& daily = plan.geometry.at("cameras").at(source.camera_serial).at("daily_registration_geometry");
        require(daily.at("mode") == "selected_daily_registration" && daily.at("status") == "resolved" &&
            daily.at("registration_id") == registration.at("registration_id") &&
            daily.at("registration").at("source_path") == plan.registration_path.string() &&
            daily.at("registration").at("sha256") == plan.registration_sha256, "geometry/registration mismatch");
        const auto& entry = daily.at("recording_snapshot_entry");
        require(entry.at("camera_serial") == source.camera_serial && number(entry.at("camera").at("width")) == static_cast<uint64_t>(source.width) &&
            number(entry.at("camera").at("height")) == static_cast<uint64_t>(source.height) && entry.at("camera").at("pixel_format") == "Mono8",
            "registered native raster mismatch");
    }
    std::vector<std::string> allowed = {"context.json", "registration.json", "geometry.json", "runtime_before.json", "runtime_after.json"};
    for (const auto& serial : expected) allowed.push_back("Cam" + serial + "_native.raw");
    // Resolve the parent without following symlinks before creating anything;
    // mkdirat is exclusive and cannot redirect creation through a path alias.
    std::unique_ptr<authority::SpatialRoiSessionAuthorityStore> parent_store;
    require(authority::SpatialRoiSessionAuthorityStore::OpenExisting(plan.output_root.parent_path(), {plan.capture_id}, &parent_store, &error), error);
    require(::mkdirat(parent_store->borrowed_recording_root_fd(), plan.capture_id.c_str(), 0700) == 0,
            "capture directory creation failed; recapture requires a new identity");
    require(::fsync(parent_store->borrowed_recording_root_fd()) == 0 && parent_store->VerifyRootBinding(&error), "capture parent changed or could not sync: " + error);
    std::unique_ptr<authority::SpatialRoiSessionAuthorityStore> store;
    require(authority::SpatialRoiSessionAuthorityStore::OpenExisting(plan.output_root, allowed, &store, &error), error);
    const auto publish_json = [&](const std::string& path, const json& value) {
        authority::SpatialRoiSessionAuthorityReceipt ref;
        require(store->PublishJson(path, value, &ref, &error), error); return ref.ToJson();
    };
    authority::SpatialRoiSessionAuthorityReceipt registration_ref;
    require(store->PublishBytes("registration.json", registration_bytes, &registration_ref, &error), error);
    const auto geometry_ref = publish_json("geometry.json", plan.geometry);
    const auto before_ref = publish_json("runtime_before.json", plan.runtime_before);
    const auto after_ref = publish_json("runtime_after.json", plan.runtime_after);
    json images = json::array();
    for (const auto& frame : frames) {
        const auto& source = frame.source;
        authority::SpatialRoiSessionAuthorityReceipt image;
        require(store->PublishBytes("Cam" + source.camera_serial + "_native.raw",
            std::string(source.mono8.begin(), source.mono8.end()), &image, &error), error);
        images.push_back({{"camera_serial", source.camera_serial}, {"camera_configuration", plan.cameras.at(source.camera_serial)},
            {"source_storage", frame.source_storage}, {"stride_bytes", source.width}, {"image", image.ToJson()},
            {"source_frame", {{"local_frame_id", source.local_frame_id}, {"camera_frame_id", source.camera_frame_id},
                {"recording_frame_id", 0}, {"camera_timestamp_ns", source.camera_timestamp_ns}, {"timestamp_sys_ns", source.timestamp_sys_ns}}}});
    }
    const json descriptor = {{"schema_id", "orange.calibration.registered_native_context"}, {"schema_version", 1},
        {"capture_id", plan.capture_id}, {"requested_at_utc", plan.requested_at_utc}, {"status", "captured"},
        {"scope", "daily_registration"}, {"recording_binding", "unbound"}, {"coordinate_space", "camera_native_pixels"},
        {"camera_configuration_authority", "orange_runtime_configuration"}, {"housekeeping_cpu", plan.housekeeping_cpu},
        {"scene_declaration", plan.declaration}, {"registration_id", registration.at("registration_id")},
        {"source_registration_path", plan.registration_path.string()}, {"registration", registration_ref.ToJson()},
        {"geometry", geometry_ref}, {"runtime_before", before_ref}, {"runtime_after", after_ref}, {"cameras", images}};
    return publish_json("context.json", descriptor);
}
}
