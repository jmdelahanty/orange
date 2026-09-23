#include "recording_daily_context_reuse.h"
#include "daily_registered_context.h"
#include "fsuid_guard.h"
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace orange::recording {
namespace fs = std::filesystem;
namespace authority = session::spatial_roi;
using json = nlohmann::json;
namespace {
constexpr const char* kDirectory = "registered_daily_context";
constexpr const char* kProfile = "daily_registered_context_reuse_v1";
void require(bool value, const std::string& reason) { if (!value) throw std::runtime_error("daily context reuse: " + reason); }
uint64_t number(const json& v) {
    require(v.is_number_unsigned() || (v.is_number_integer() && v.get<int64_t>() >= 0), "unsigned integer required");
    return v.get<uint64_t>();
}
void exact(const json& value, std::initializer_list<const char*> keys) {
    require(value.is_object() && value.size() == keys.size(), "unknown/missing fields");
    for (const auto* key : keys) require(value.contains(key), "missing field");
}
authority::SpatialRoiSessionAuthorityReceipt receipt(const json& ref) {
    exact(ref, {"relative_path", "size_bytes", "sha256"});
    return {ref.at("relative_path").get<std::string>(), number(ref.at("size_bytes")), ref.at("sha256").get<std::string>()};
}
json read_json(authority::SpatialRoiSessionAuthorityStore& store, const json& ref, const char* leaf) {
    auto r = receipt(ref); require(r.relative_path == leaf && r.size_bytes <= 64 * 1024 * 1024, "unexpected artifact reference");
    std::string bytes, error; require(store.ReadAndVerify(r, &bytes, nullptr, &error), error);
    return json::parse(bytes);
}
void verify_binding(const calibration::DailyContextBundle& bundle, const json& cameras, const json& geometry) {
    require(geometry.at("schema_id") == "orange.recording.geometry_contract" && number(geometry.at("schema_version")) == 1,
        "unsupported current geometry");
    require(cameras.is_array() && cameras.size() == bundle.descriptor.at("cameras").size(), "camera membership changed");
    std::map<std::string, json> captured;
    for (const auto& camera : bundle.descriptor.at("cameras")) captured.emplace(camera.at("camera_serial").get<std::string>(), camera);
    std::set<std::string> seen;
    for (const auto& camera : cameras) {
        exact(camera, {"camera_serial", "camera_id", "producer_instance_id", "stream_generation", "width", "height", "camera_configuration"});
        const auto serial = camera.at("camera_serial").get<std::string>();
        require(captured.count(serial) && seen.insert(serial).second, "unexpected/duplicate recording camera");
        const auto& config = camera.at("camera_configuration");
        require(config == captured.at(serial).at("camera_configuration"), "camera settings changed since capture: " + serial);
        require(number(camera.at("camera_id")) == number(config.at("camera_id")) && number(camera.at("width")) == number(config.at("width")) &&
            number(camera.at("height")) == number(config.at("height")), "recording binding/config mismatch");
        const auto producer = camera.at("producer_instance_id").get<std::string>();
        require(!producer.empty() && producer.size() <= 1024, "missing recording producer identity"); number(camera.at("stream_generation"));
        const auto& current = geometry.at("cameras").at(serial);
        const auto physical = current.value("physical_registration", json::object());
        require(physical.value("mode", "not_selected") == "not_selected", "daily geometry superseded by physical registration selection");
        require(current.at("daily_registration_geometry") == bundle.geometry.at("cameras").at(serial).at("daily_registration_geometry"),
            "selected daily registration or numerical geometry changed: " + serial);
    }
}
json use_record(const json& start, const calibration::DailyContextBundle& bundle) {
    return {{"schema_id", "orange.recording.registered_context_use"}, {"schema_version", 1}, {"status", "captured"},
        {"recording_id", start.at("recording_id")}, {"capture_id", bundle.descriptor.at("capture_id")},
        {"source_selection", start.at("config").at("source")}, {"daily_context", start.at("daily_context")},
        {"recording_geometry", start.at("geometry_contract")}, {"recording_cameras", start.at("cameras")},
        {"reuse_declaration", start.at("config").at("declaration")}};
}
}

json PrepareDailyContextReuse(const RegisteredContextConfig& config, const fs::path& root,
                             const std::vector<RegisteredContextCamera>& cameras, const json& geometry) {
    require(config.schema_version == 2 && config.enabled && config.ReusesDailyContext(), "invalid reuse configuration");
    const auto source = fs::path(config.source.at("descriptor_path").get<std::string>());
    const authority::SpatialRoiSessionAuthorityReceipt descriptor_ref{"context.json", number(config.source.at("size_bytes")), config.source.at("sha256").get<std::string>()};
    const auto bundle = calibration::ReadDailyRegisteredContext(source.parent_path(), descriptor_ref);
    json bindings = json::array();
    for (const auto& camera : cameras) bindings.push_back({{"camera_serial", camera.serial}, {"camera_id", camera.camera_id},
        {"producer_instance_id", camera.producer_instance_id}, {"stream_generation", camera.stream_generation},
        {"width", camera.width}, {"height", camera.height}, {"camera_configuration", camera.camera_configuration}});
    verify_binding(bundle, bindings, geometry);
    require(!root.filename().empty() && root.filename().string().size() <= 1024, "invalid recording identity");
    ScopedFsuid guard;
    std::string error;
    std::unique_ptr<authority::SpatialRoiSessionAuthorityStore> store, archive;
    require(authority::SpatialRoiSessionAuthorityStore::OpenExisting(root,
        {"registered_context_geometry_v1.json", "registered_context_use_v1.json"}, &store, &error), error);
    // Never adopt an existing directory (even if apparently identical).
    require(::mkdirat(store->borrowed_recording_root_fd(), kDirectory, 0700) == 0, "archive already exists or cannot be created");
    require(::fsync(store->borrowed_recording_root_fd()) == 0 && store->VerifyRootBinding(&error), "archive parent changed or sync failed: " + error);
    std::vector<std::string> allowed;
    for (const auto& item : bundle.files) allowed.push_back(item.first);
    require(authority::SpatialRoiSessionAuthorityStore::OpenExisting(root / kDirectory, allowed, &archive, &error), error);
    authority::SpatialRoiSessionAuthorityReceipt written;
    for (const auto& item : bundle.files) if (item.first != "context.json")
        require(archive->PublishBytes(item.first, item.second, &written, &error), error);
    require(archive->PublishBytes("context.json", bundle.files.at("context.json"), &written, &error) && written == descriptor_ref, error);
    // Reopen and validate the copied evidence, not only the source assets.
    calibration::ReadDailyRegisteredContext(root / kDirectory, descriptor_ref);
    authority::SpatialRoiSessionAuthorityReceipt geometry_ref, use_ref;
    require(store->PublishJson("registered_context_geometry_v1.json", geometry, &geometry_ref, &error), error);
    json start = {{"schema_version", 2}, {"required", true}, {"profile", kProfile}, {"recording_id", root.filename().string()},
        {"config", config.ToJson()}, {"geometry_contract", geometry_ref.ToJson()}, {"cameras", bindings},
        {"daily_context", {{"directory", kDirectory}, {"descriptor", descriptor_ref.ToJson()}}}};
    require(store->PublishJson("registered_context_use_v1.json", use_record(start, bundle), &use_ref, &error), error);
    start["use_receipt"] = use_ref.ToJson();
    return start;
}

void ApplyDailyContextReuseGate(const fs::path& root, const json& snapshot, json* parent) {
    json result = {{"schema_version", 2}, {"required", true}, {"profile", kProfile}, {"status", "pending"}};
    const auto state = parent->value("status", "");
    if (state == "completed" || state == "incomplete" || state == "failed" || state == "interrupted") {
        try {
            const auto& start = snapshot.at("session").at("registered_scene_context");
            exact(start, {"schema_version", "required", "profile", "recording_id", "config", "geometry_contract", "cameras", "daily_context", "use_receipt"});
            require(number(start.at("schema_version")) == 2 && start.at("required") == true && start.at("profile") == kProfile &&
                start.at("recording_id") == parent->at("session_id") && start.at("recording_id") == root.filename().string(), "prearm recording identity mismatch");
            const auto config = RegisteredContextConfig::Parse(start.at("config"));
            require(config.schema_version == 2 && config.enabled && config.ReusesDailyContext(), "required reuse configuration missing");
            const auto& daily = start.at("daily_context"); exact(daily, {"directory", "descriptor"});
            require(daily.at("directory") == kDirectory, "unexpected archive path");
            const auto ref = receipt(daily.at("descriptor"));
            require(ref.size_bytes == number(config.source.at("size_bytes")) && ref.sha256 == config.source.at("sha256"), "source selection digest differs");
            const auto bundle = calibration::ReadDailyRegisteredContext(root / kDirectory, ref);
            std::unique_ptr<authority::SpatialRoiSessionAuthorityStore> store;
            std::string error;
            require(authority::SpatialRoiSessionAuthorityStore::OpenExisting(root,
                {"registered_context_geometry_v1.json", "registered_context_use_v1.json"}, &store, &error), error);
            const auto geometry = read_json(*store, start.at("geometry_contract"), "registered_context_geometry_v1.json");
            verify_binding(bundle, start.at("cameras"), geometry);
            const auto use = read_json(*store, start.at("use_receipt"), "registered_context_use_v1.json");
            require(use == use_record(start, bundle), "recording use receipt differs");
            const auto& masters = snapshot.at("session").at("master_frame_journal").at("cameras");
            require(masters.is_array() && masters.size() == start.at("cameras").size(), "required master membership differs");
            for (const auto& camera : start.at("cameras")) {
                std::size_t matches = 0;
                for (const auto& master : masters) if (master.at("camera_serial") == camera.at("camera_serial") &&
                    master.at("recording_id") == start.at("recording_id") && master.at("producer_instance_id") == camera.at("producer_instance_id") &&
                    master.at("stream_generation") == camera.at("stream_generation")) ++matches;
                require(matches == 1, "recording use/master identity differs");
            }
            result["daily_context"] = daily; result["use_receipt"] = start.at("use_receipt"); result["status"] = "captured";
        } catch (const std::exception& ex) {
            result["status"] = "failed"; result["reason"] = ex.what();
            if (state == "completed") (*parent)["status"] = "failed";
        }
    }
    (*parent)["registered_scene_context"] = std::move(result);
}
}
