#include "session/realtime_products.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "gui/spatial_layout/sha256.h"
using orange::gui::spatial_layout::checksum::sha256_hex;

namespace orange::session {
namespace {

namespace fs = std::filesystem;

struct ScannedLog {
    bool present = false;
    uint64_t size_bytes = 0;
    std::string sha256;
    std::string schema_id;
    int schema_version = 0;
    uint64_t header_rows = 0;
    uint64_t row_count = 0;      // frame rows (event rows that are not a header)
    uint64_t parse_errors = 0;
    uint64_t first_recording_frame_id = 0;
    uint64_t last_recording_frame_id = 0;
    std::map<std::string, uint64_t> rows_by_status;
    std::map<std::string, uint64_t> rows_by_kind;  // result | no_result | failed | other
};

bool read_file(const fs::path& path, std::string* out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    *out = buffer.str();
    return static_cast<bool>(in) || in.eof();
}

std::string kind_for_status(const std::string& product, const std::string& status)
{
    if (product == "detections") {
        if (status == "detections" || status == "zero_detections") return "result";
        if (status == "failed" || status == "timeout" || status == "error") return "failed";
        return "other";
    }
    if (status == "poses") return "result";
    if (status == "no_result") return "no_result";
    if (status == "failed") return "failed";
    return "other";
}

// product: "detections" (status under "yolo") or "pose" (status under "pose").
ScannedLog scan_event_log(const fs::path& path, const std::string& product)
{
    ScannedLog log;
    std::string bytes;
    if (!fs::exists(path) || !read_file(path, &bytes)) {
        return log;
    }
    log.present = true;
    log.size_bytes = bytes.size();
    log.sha256 = sha256_hex(bytes);
    for (const char* kind : {"result", "no_result", "failed", "other"}) {
        log.rows_by_kind[kind] = 0;
    }
    const std::string status_block = product == "detections" ? "yolo" : "pose";
    size_t start = 0;
    while (start < bytes.size()) {
        size_t end = bytes.find('\n', start);
        if (end == std::string::npos) end = bytes.size();
        if (end > start) {
            const nlohmann::json event = nlohmann::json::parse(
                bytes.begin() + static_cast<std::ptrdiff_t>(start),
                bytes.begin() + static_cast<std::ptrdiff_t>(end), nullptr, false);
            if (event.is_discarded() || !event.is_object()) {
                log.parse_errors++;
            } else {
                if (log.schema_id.empty()) {
                    log.schema_id = event.value("schema_id", std::string());
                    log.schema_version = event.value("schema_version", 0);
                }
                const std::string event_kind = event.value("event_kind", std::string());
                // v2: session_header and spatial_mask_policy lines (any
                // non-result kind) are header rows, never frame rows.
                if (event_kind != "yolo_result" && event_kind != "pose_result") {
                    log.header_rows++;
                } else {
                    log.row_count++;
                    const nlohmann::json frame = event.value("frame", nlohmann::json::object());
                    const uint64_t frame_id = frame.is_object()
                        ? frame.value("recording_frame_id", 0ULL) : 0ULL;
                    if (frame_id > 0) {
                        if (log.first_recording_frame_id == 0 || frame_id < log.first_recording_frame_id) {
                            log.first_recording_frame_id = frame_id;
                        }
                        if (frame_id > log.last_recording_frame_id) {
                            log.last_recording_frame_id = frame_id;
                        }
                    }
                    const nlohmann::json block = event.value(status_block, nlohmann::json::object());
                    const std::string status = block.is_object()
                        ? block.value("status", std::string()) : std::string();
                    log.rows_by_status[status.empty() ? "missing" : status]++;
                    log.rows_by_kind[kind_for_status(product, status)]++;
                }
            }
        }
        start = end + 1;
    }
    return log;
}

nlohmann::json file_entry(const fs::path& folder, const std::string& name, const std::string& role,
                          const std::string& sha256_known = std::string(), uint64_t size_known = 0)
{
    const fs::path path = folder / name;
    if (!fs::exists(path)) {
        return nullptr;
    }
    std::string sha = sha256_known;
    uint64_t size = size_known;
    if (sha.empty()) {
        std::string bytes;
        if (!read_file(path, &bytes)) {
            return nullptr;
        }
        sha = sha256_hex(bytes);
        size = bytes.size();
    }
    return {{"role", role}, {"path", name}, {"size_bytes", size}, {"sha256", sha}};
}

nlohmann::json model_ref(const nlohmann::json& models, const std::string& serial, const std::string& kind)
{
    nlohmann::json ref = {{"models_key", "models." + serial + "." + kind}};
    const nlohmann::json camera = models.is_object() ? models.value(serial, nlohmann::json::object()) : nlohmann::json::object();
    const nlohmann::json block = camera.is_object() ? camera.value(kind, nlohmann::json::object()) : nlohmann::json::object();
    const nlohmann::json runtime = block.is_object() ? block.value("runtime", nlohmann::json::object()) : nlohmann::json::object();
    ref["enabled"] = block.is_object() ? block.value("enabled", false) : false;
    ref["model_id"] = runtime.is_object() ? runtime.value("model_id", std::string()) : std::string();
    ref["engine_path"] = runtime.is_object() ? runtime.value("engine_path", std::string()) : std::string();
    ref["engine_sha256"] = runtime.is_object() ? runtime.value("engine_sha256", std::string()) : std::string();
    ref["weights_sha256"] = runtime.is_object() ? runtime.value("weights_sha256", std::string()) : std::string();
    return ref;
}

nlohmann::json product_json(const fs::path& folder, const std::string& serial, const std::string& product,
                            const std::string& events_name, const std::string& perf_name,
                            const nlohmann::json& models)
{
    const ScannedLog log = scan_event_log(folder / events_name, product);
    nlohmann::json out = {
        {"status", log.present ? "present" : "absent"},
        {"model_ref", model_ref(models, serial, product == "detections" ? "detect" : "pose")},
        {"files", nlohmann::json::array()}
    };
    if (!log.present) {
        return out;
    }
    out["line_schema"] = {{"schema_id", log.schema_id}, {"schema_version", log.schema_version}};
    out["frame_identity_key"] = "frame.recording_frame_id";
    out["row_count"] = log.row_count;
    out["header_rows"] = log.header_rows;
    out["parse_errors"] = log.parse_errors;
    out["first_recording_frame_id"] = log.first_recording_frame_id;
    out["last_recording_frame_id"] = log.last_recording_frame_id;
    out["rows_by_kind"] = log.rows_by_kind;
    out["rows_by_status"] = log.rows_by_status;
    out["files"].push_back({{"role", "events"}, {"path", events_name},
                            {"size_bytes", log.size_bytes}, {"sha256", log.sha256}});
    const nlohmann::json perf = file_entry(folder, perf_name, "perf");
    if (!perf.is_null()) {
        out["files"].push_back(perf);
    }
    return out;
}

}  // namespace

nlohmann::json build_realtime_products_json(const std::string& recording_folder,
                                            const std::vector<std::string>& camera_serials)
{
    const fs::path folder(recording_folder);
    nlohmann::json models = nlohmann::json::object();
    {
        std::string bytes;
        if (read_file(folder / "recording_snapshot.json", &bytes)) {
            const nlohmann::json snapshot = nlohmann::json::parse(bytes, nullptr, false);
            if (snapshot.is_object()) {
                models = snapshot.value("models", nlohmann::json::object());
            }
        }
    }
    nlohmann::json cameras = nlohmann::json::object();
    for (const std::string& serial : camera_serials) {
        const std::string prefix = "Cam" + serial;
        nlohmann::json camera = {
            {"detections", product_json(folder, serial, "detections",
                                        prefix + "_yolo_events.jsonl", prefix + "_yolo_perf.csv", models)},
            {"pose", product_json(folder, serial, "pose",
                                  prefix + "_pose_events.jsonl", prefix + "_pose_perf.csv", models)},
            {"crop_files", nlohmann::json::array()},
            {"acquisition_files", nlohmann::json::array()}
        };
        {
            const nlohmann::json pose_block = models.is_object()
                ? models.value(serial, nlohmann::json::object()).value("pose", nlohmann::json::object())
                : nlohmann::json::object();
            const nlohmann::json runtime = pose_block.is_object()
                ? pose_block.value("runtime", nlohmann::json::object()) : nlohmann::json::object();
            camera["pose"]["pose_crop_size_px"] = runtime.is_object()
                ? runtime.value("pose_crop_size_px", 0) : 0;
        }
        for (const auto& [name, role] : std::vector<std::pair<std::string, std::string>>{
                 {prefix + "_crop_meta.csv", "crop_ledger_session"},
                 {prefix + "_crop_perf.csv", "perf"},
                 {prefix + "_crop_sidecar_perf.csv", "perf"}}) {
            const nlohmann::json entry = file_entry(folder, name, role);
            if (!entry.is_null()) camera["crop_files"].push_back(entry);
        }
        for (const auto& [name, role] : std::vector<std::pair<std::string, std::string>>{
                 {prefix + "_acquisition_cadence_probe.csv", "diagnostic"},
                 {prefix + "_ring_release.csv", "diagnostic"},
                 {prefix + "_owner_push.csv", "diagnostic"},
                 {prefix + "_pipeline_perf.csv", "perf"}}) {
            const nlohmann::json entry = file_entry(folder, name, role);
            if (!entry.is_null()) camera["acquisition_files"].push_back(entry);
        }
        cameras[serial] = std::move(camera);
    }
    return {
        {"schema_id", kRealtimeProductsSchemaId},
        {"schema_version", kRealtimeProductsSchemaVersion},
        {"paths_relative_to", "recording_folder"},
        {"digest", "sha256 hex of the file bytes, as in the transfer inventory"},
        {"cameras", std::move(cameras)}
    };
}

}  // namespace orange::session
