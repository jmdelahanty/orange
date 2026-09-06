#include "recording_registered_context.h"
#include "fsuid_guard.h"
#include "gui/spatial_layout/sha256.h"
#include <fstream>
#include <sched.h>
#include <set>
#include <stdexcept>

namespace orange::recording {
namespace fs = std::filesystem;
namespace authority = session::spatial_roi;
using json = nlohmann::json;
namespace {
void require(bool ok, const std::string& error) { if (!ok) throw std::runtime_error(error); }
uint64_t number(const json& value) {
    require(value.is_number_unsigned() || (value.is_number_integer() && value.get<int64_t>() >= 0), "registered context: unsigned integer required");
    return value.get<uint64_t>();
}
void exact(const json& value, std::initializer_list<const char*> keys) {
    require(value.is_object() && value.size() == keys.size(), "registered context: unknown/missing fields");
    for (const auto* key : keys) require(value.contains(key), "registered context: missing field");
}
std::string prefix(const std::string& serial) {
    require(!serial.empty() && serial.size() <= 128 && serial.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-") == std::string::npos,
            "registered context: unsafe camera serial");
    return "Cam" + serial + "_registered_context";
}
json read_json(const fs::path& path) {
    require(fs::is_regular_file(fs::symlink_status(path)) && fs::file_size(path) <= 16 * 1024 * 1024, "registered context: invalid JSON file");
    std::ifstream in(path); json result; in >> result; in >> std::ws;
    require(in.eof(), "registered context: trailing JSON data"); return result;
}
authority::SpatialRoiSessionAuthorityReceipt receipt(const json& value) {
    exact(value, {"relative_path", "size_bytes", "sha256"});
    return {value.at("relative_path").get<std::string>(), number(value.at("size_bytes")), value.at("sha256").get<std::string>()};
}
json file_ref(const fs::path& root, const std::string& leaf) {
    require(fs::path(leaf).filename() == leaf && fs::is_regular_file(fs::symlink_status(root / leaf)), "registered context: required artifact missing");
    std::string hash, error;
    require(gui::spatial_layout::checksum::file_sha256(root / leaf, &hash, &error), "registered context hash: " + error);
    return {{"relative_path", leaf}, {"size_bytes", fs::file_size(root / leaf)}, {"sha256", hash}};
}
json verify_json(authority::SpatialRoiSessionAuthorityStore& store, const json& ref) {
    std::string bytes, error;
    require(store.ReadAndVerify(receipt(ref), &bytes, nullptr, &error), "registered context readback: " + error);
    return json::parse(bytes);
}
std::string registration_kind(const json& geometry, const RegisteredContextCamera& camera) {
    require(geometry.at("schema_id") == "orange.recording.geometry_contract" && number(geometry.at("schema_version")) == 1,
            "registered context: unsupported geometry contract");
    const auto& binding = geometry.at("cameras").at(camera.serial);
    const json* selected = nullptr;
    std::string kind;
    if (binding.contains("physical_registration") && binding.at("physical_registration").value("status", "") == "selected_resolved") {
        selected = &binding.at("physical_registration"); kind = "orange_physical_registration";
    } else if (binding.contains("daily_registration_geometry") && binding.at("daily_registration_geometry").value("status", "") == "resolved" &&
               binding.at("daily_registration_geometry").value("mode", "") == "selected_daily_registration") {
        // A selected invalid Orange pointer must not be silently replaced by
        // a different registration authority.
        const auto physical = binding.value("physical_registration", json::object());
        require(physical.value("mode", "not_selected") == "not_selected", "registered context: invalid selected physical registration");
        selected = &binding.at("daily_registration_geometry"); kind = "citrus_daily_registration";
    }
    require(selected != nullptr, "registered context: no accepted selected registration for " + camera.serial);
    const auto& entry = selected->at("recording_snapshot_entry");
    const auto& raster = entry.at("camera");
    require(entry.at("camera_serial") == camera.serial &&
            number(raster.at("width")) == static_cast<uint64_t>(camera.width) &&
            number(raster.at("height")) == static_cast<uint64_t>(camera.height) &&
            raster.at("pixel_format") == "Mono8", "registered context: registration camera/raster mismatch");
    require(entry.at("artifact_id").is_string() && !entry.at("artifact_id").get<std::string>().empty(), "registered context: registration artifact identity missing");
    return kind;
}
}

RegisteredContextConfig RegisteredContextConfig::Parse(const json& value) {
    require(value.is_object(), "fixed.registered_scene_context must be an object");
    for (const auto& item : value.items()) require(item.key() == "schema_version" || item.key() == "enabled" || item.key() == "timeout_ms" ||
        item.key() == "worker_cpu_ids" || item.key() == "declaration", "unknown registered_scene_context configuration field");
    require(number(value.at("schema_version")) == 1 && value.at("enabled").is_boolean(), "invalid registered_scene_context configuration version/enabled");
    RegisteredContextConfig result;
    result.enabled = value.at("enabled").get<bool>();
    if (value.contains("timeout_ms")) {
        const auto timeout = number(value.at("timeout_ms"));
        require(timeout >= 100 && timeout <= 60000, "registered_scene_context timeout_ms must be 100..60000"); result.timeout_ms = static_cast<int>(timeout);
    }
    if (value.contains("worker_cpu_ids")) {
        require(value.at("worker_cpu_ids").is_array(), "registered_scene_context worker_cpu_ids must be array");
        std::set<int> seen;
        for (const auto& cpu : value.at("worker_cpu_ids")) {
            const auto id = number(cpu); require(id < CPU_SETSIZE && seen.insert(static_cast<int>(id)).second, "invalid/duplicate context worker CPU");
            result.worker_cpu_ids.push_back(static_cast<int>(id));
        }
    }
    if (value.contains("declaration")) {
        require(number(value.at("declaration").at("schema_version")) == 1, "registered context declaration version must be 1");
        std::string error;
        require(authority::parse_registered_scene_context_capture_declaration(value.at("declaration"), &result.declaration, &error), error);
    }
    if (result.enabled) {
        require(!result.worker_cpu_ids.empty(), "enabled registered_scene_context needs explicit housekeeping worker_cpu_ids");
        require(authority::registered_scene_context_daily_registration_accepted(result.declaration), "registered_scene_context requires explicit accepted_for_experiment declaration");
    }
    return result;
}
json RegisteredContextConfig::ToJson() const {
    json result = {{"schema_version", 1}, {"enabled", enabled}, {"timeout_ms", timeout_ms}, {"worker_cpu_ids", worker_cpu_ids}};
    if (!declaration.registration_authority_status.empty()) result["declaration"] = authority::registered_scene_context_capture_declaration_to_json(declaration);
    return result;
}

void RegisteredContextSet::Prepare(const RegisteredContextConfig& config, const fs::path& root,
                                  const std::vector<RegisteredContextCamera>& cameras, const json& geometry) {
    require(!store_ && bindings_.empty(), "registered context already prepared");
    config_ = RegisteredContextConfig::Parse(config.ToJson());
    require(config_.enabled && !cameras.empty() && cameras.size() <= 64, "registered context camera set invalid");
    recording_id_ = root.filename().string();
    require(!recording_id_.empty() && recording_id_.size() <= 1024, "registered context parent identity invalid");
    std::vector<std::string> allowed = {"registered_context_geometry_v1.json"};
    for (const auto& camera : cameras) {
        const auto name = prefix(camera.serial);
        require(camera.width > 0 && camera.height > 0 && static_cast<uint64_t>(camera.width) * camera.height <= 64 * 1024 * 1024 &&
                !camera.producer_instance_id.empty() && camera.producer_instance_id.size() <= 1024, "registered context invalid raster/producer identity");
        require(captured_.emplace(camera.serial, false).second, "duplicate registered context camera");
        const auto kind = registration_kind(geometry, camera);
        allowed.push_back(name + ".raw"); allowed.push_back(name + "_v2.json");
        require(!fs::exists(root / (name + ".raw")) && !fs::exists(root / (name + "_v2.json")), "registered context exists before prearm");
        bindings_.push_back({{"camera_serial", camera.serial}, {"camera_id", camera.camera_id},
            {"producer_instance_id", camera.producer_instance_id}, {"stream_generation", camera.stream_generation},
            {"width", camera.width}, {"height", camera.height}, {"registration_kind", kind},
            {"descriptor_relative_path", name + "_v2.json"}});
    }
    ScopedFsuid guard;
    std::string error;
    require(authority::SpatialRoiSessionAuthorityStore::OpenExisting(root, allowed, &store_, &error), error);
    require(store_->PublishJson("registered_context_geometry_v1.json", geometry, &geometry_, &error), error);
}
json RegisteredContextSet::StartEvidence() const {
    require(store_ != nullptr, "registered context not prepared");
    return {{"schema_version", 1}, {"required", true}, {"profile", "native_registered_pre_recording_v1"},
        {"recording_id", recording_id_}, {"config", config_.ToJson()}, {"geometry_contract", geometry_.ToJson()}, {"cameras", bindings_}};
}
bool RegisteredContextSet::Complete() const {
    if (captured_.empty()) return false;
    for (const auto& item : captured_) if (!item.second) return false;
    return true;
}
void RegisteredContextSet::Accept(const RegisteredContextFrame& frame) {
    require(store_ && captured_.count(frame.camera_serial) && !captured_.at(frame.camera_serial), "unexpected/duplicate registered context frame");
    const json* binding = nullptr;
    for (const auto& camera : bindings_) if (camera.at("camera_serial") == frame.camera_serial) binding = &camera;
    require(binding && frame.width == binding->at("width") && frame.height == binding->at("height") &&
            frame.mono8.size() == static_cast<uint64_t>(frame.width) * frame.height &&
            frame.recording_frame_id == 0 && frame.local_frame_id > 0 && frame.camera_timestamp_ns > 0 && frame.timestamp_sys_ns > 0,
            "registered context requires one exact native pre-recording frame");
    ScopedFsuid guard;
    std::string error;
    require(store_->ReadAndVerify(geometry_, nullptr, nullptr, &error), error);
    authority::SpatialRoiSessionAuthorityReceipt image_ref, descriptor_ref;
    const std::string bytes(reinterpret_cast<const char*>(frame.mono8.data()), frame.mono8.size());
    require(store_->PublishBytes(prefix(frame.camera_serial) + ".raw", bytes, &image_ref, &error), error);
    const json descriptor = {{"schema_id", "orange.recording.registered_scene_context"}, {"schema_version", 2},
        {"profile", "camera_native_mono8_pre_recording_v1"}, {"status", "captured"}, {"recording_id", recording_id_},
        {"camera_binding", *binding}, {"geometry_contract", geometry_.ToJson()},
        {"scene_declaration", authority::registered_scene_context_capture_declaration_to_json(config_.declaration)},
        {"native_raster", {{"width", frame.width}, {"height", frame.height}, {"stride_bytes", frame.width},
            {"pixel_format", "Mono8"}, {"coordinate_space", "camera_native_pixels"}}},
        {"source_frame", {{"local_frame_id", frame.local_frame_id}, {"camera_frame_id", frame.camera_frame_id},
            {"recording_frame_id", frame.recording_frame_id}, {"camera_timestamp_ns", frame.camera_timestamp_ns}, {"timestamp_sys_ns", frame.timestamp_sys_ns}}},
        {"image", image_ref.ToJson()}};
    require(store_->PublishJson(prefix(frame.camera_serial) + "_v2.json", descriptor, &descriptor_ref, &error), error);
    require(verify_json(*store_, descriptor_ref.ToJson()) == descriptor, "registered context descriptor readback mismatch");
    captured_.at(frame.camera_serial) = true;
}

void ApplyRequiredRegisteredContextGate(const fs::path& root, json* parent) {
    const auto start_path = fs::exists(root / "recording_snapshot_start.json") ? root / "recording_snapshot_start.json" : root / "recording_snapshot.json";
    if (!fs::exists(start_path)) return;
    const auto snapshot = read_json(start_path);
    if (!snapshot.contains("session") || !snapshot.at("session").contains("registered_scene_context")) return;
    const auto& start = snapshot.at("session").at("registered_scene_context");
    json result = {{"schema_version", 1}, {"required", true}, {"profile", "native_registered_pre_recording_v1"},
        {"status", "pending"}, {"cameras", json::array()}};
    const auto state = parent->value("status", std::string());
    if (state == "completed" || state == "incomplete" || state == "failed" || state == "interrupted") {
        try {
            exact(start, {"schema_version", "required", "profile", "recording_id", "config", "geometry_contract", "cameras"});
            require(number(start.at("schema_version")) == 1 && start.at("required") == true &&
                    start.at("profile") == "native_registered_pre_recording_v1" && start.at("recording_id") == parent->at("session_id"),
                    "registered context prearm identity mismatch");
            const auto config = RegisteredContextConfig::Parse(start.at("config"));
            require(config.enabled && start.at("cameras").is_array() && !start.at("cameras").empty() && start.at("cameras").size() <= 64,
                    "registered context invalid required camera set");
            std::vector<std::string> allowed = {"registered_context_geometry_v1.json"};
            std::set<std::string> camera_serials;
            for (const auto& camera : start.at("cameras")) {
                const auto serial = camera.at("camera_serial").get<std::string>();
                require(camera_serials.insert(serial).second, "duplicate required context camera");
                const auto name = prefix(serial);
                allowed.push_back(name + ".raw"); allowed.push_back(name + "_v2.json");
            }
            std::string error;
            std::unique_ptr<authority::SpatialRoiSessionAuthorityStore> store;
            require(authority::SpatialRoiSessionAuthorityStore::OpenExisting(root, allowed, &store, &error), error);
            require(start.at("geometry_contract").at("relative_path") == "registered_context_geometry_v1.json", "unexpected context geometry path");
            const auto geometry = verify_json(*store, start.at("geometry_contract"));
            for (const auto& camera : start.at("cameras")) {
                exact(camera, {"camera_serial", "camera_id", "producer_instance_id", "stream_generation", "width", "height", "registration_kind", "descriptor_relative_path"});
                const auto serial = camera.at("camera_serial").get<std::string>();
                const auto name = prefix(serial);
                require(camera.at("descriptor_relative_path") == name + "_v2.json", "unexpected context descriptor path");
                const auto ref = file_ref(root, name + "_v2.json");
                const auto descriptor = verify_json(*store, ref);
                exact(descriptor, {"schema_id", "schema_version", "profile", "status", "recording_id", "camera_binding", "geometry_contract", "scene_declaration", "native_raster", "source_frame", "image"});
                require(descriptor.at("schema_id") == "orange.recording.registered_scene_context" && number(descriptor.at("schema_version")) == 2 &&
                        descriptor.at("profile") == "camera_native_mono8_pre_recording_v1" && descriptor.at("status") == "captured" &&
                        descriptor.at("recording_id") == start.at("recording_id") && descriptor.at("camera_binding") == camera &&
                        descriptor.at("geometry_contract") == start.at("geometry_contract") &&
                        descriptor.at("scene_declaration") == start.at("config").at("declaration"), "registered context descriptor/prearm mismatch");
                const auto w = number(camera.at("width")), h = number(camera.at("height"));
                require(w > 0 && h > 0 && w <= 64 * 1024 * 1024 && h <= 64 * 1024 * 1024 / w, "registered context invalid raster");
                require(descriptor.at("native_raster") == json({{"width", w}, {"height", h}, {"stride_bytes", w},
                    {"pixel_format", "Mono8"}, {"coordinate_space", "camera_native_pixels"}}), "registered context raster mismatch");
                const auto& frame = descriptor.at("source_frame");
                exact(frame, {"local_frame_id", "camera_frame_id", "recording_frame_id", "camera_timestamp_ns", "timestamp_sys_ns"});
                require(number(frame.at("recording_frame_id")) == 0 && number(frame.at("local_frame_id")) > 0 &&
                        number(frame.at("camera_timestamp_ns")) > 0 && number(frame.at("timestamp_sys_ns")) > 0,
                        "registered context was not captured before recording");
                number(frame.at("camera_frame_id"));
                const RegisteredContextCamera native = {serial, camera.at("producer_instance_id").get<std::string>(), number(camera.at("camera_id")),
                    number(camera.at("stream_generation")), static_cast<int>(w), static_cast<int>(h)};
                require(camera.at("registration_kind") == registration_kind(geometry, native), "registered context registration mismatch");
                const auto image_ref = receipt(descriptor.at("image"));
                require(image_ref.relative_path == name + ".raw" && image_ref.size_bytes == w * h, "registered context image binding mismatch");
                require(store->ReadAndVerify(image_ref, nullptr, nullptr, &error), error);
                // Match the master owner frozen in the same immutable start.
                const auto& masters = snapshot.at("session").at("master_frame_journal").at("cameras");
                std::size_t matches = 0;
                for (const auto& master : masters) if (master.at("camera_serial") == serial && master.at("recording_id") == start.at("recording_id") &&
                    master.at("producer_instance_id") == camera.at("producer_instance_id") && master.at("stream_generation") == camera.at("stream_generation")) ++matches;
                require(matches == 1 && masters.size() == start.at("cameras").size(), "registered context/master identity mismatch");
                result["cameras"].push_back({{"camera_serial", serial}, {"descriptor", ref}, {"image", descriptor.at("image")}});
            }
            result["status"] = "captured";
        } catch (const std::exception& ex) {
            result["status"] = "failed"; result["reason"] = ex.what();
            if (state == "completed") (*parent)["status"] = "failed";
        }
    }
    (*parent)["registered_scene_context"] = std::move(result);
}
} // namespace orange::recording
