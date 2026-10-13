#include "session/realtime_products.h"
#include "session/snapshot_models.h"
#include "event_log_reader.h"
#include "timing/log_writer.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <map>
#include <variant>
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

// The v3 schemas' status enums (detections: detections, zero_detections, timeout, failed; pose:
// poses, no_result, failed) leave "other" empty; it stays in rows_by_kind as a declared count.
std::string kind_for_status(const std::string& product, const std::string& status)
{
    if (product == "detections") {
        if (status == "detections" || status == "zero_detections") return "result";
        if (status == "failed" || status == "timeout") return "failed";
        return "other";
    }
    if (status == "poses") return "result";
    if (status == "no_result") return "no_result";
    if (status == "failed") return "failed";
    return "other";
}

// One frame line of either log: its frame id and the product's status.
void count_frame_line(ScannedLog* log, const std::string& product, uint64_t frame_id, const std::string& status)
{
    log->row_count++;
    if (frame_id > 0) {
        if (log->first_recording_frame_id == 0 || frame_id < log->first_recording_frame_id) {
            log->first_recording_frame_id = frame_id;
        }
        if (frame_id > log->last_recording_frame_id) {
            log->last_recording_frame_id = frame_id;
        }
    }
    log->rows_by_status[status]++;
    log->rows_by_kind[kind_for_status(product, status)]++;
}

// product: "detections" (Cam<serial>_yolo_events.jsonl) or "pose" (Cam<serial>_pose_events.jsonl).
// Every line is read with the generated parser of orange.yolo_event / orange.pose_event v3
// (src/event_log_reader.h); a line that is not JSON or does not match the schema counts as a
// parse error, never as a row. line_schema reports what the first line declares.
ScannedLog scan_event_log(const fs::path& path, const std::string& product)
{
    namespace yolo = event_log_reader::yolo;
    namespace pose = event_log_reader::pose;
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
    bool first_line = true;
    size_t start = 0;
    while (start < bytes.size()) {
        size_t end = bytes.find('\n', start);
        if (end == std::string::npos) end = bytes.size();
        if (end > start) {
            const std::string line = bytes.substr(start, end - start);
            if (first_line) {
                first_line = false;
                const nlohmann::json declared = nlohmann::json::parse(line, nullptr, false);
                if (declared.is_object()) {
                    log.schema_id = declared.value("schema_id", std::string());
                    log.schema_version = declared.value("schema_version", 0);
                }
            }
            std::string error;
            if (product == "detections") {
                yolo::OrangeYoloEventV3 event;
                if (event_log_reader::parse_yolo_line(line, &event, &error) != event_log_reader::LineStatus::kOk) {
                    log.parse_errors++;
                } else if (const auto* result = std::get_if<yolo::OrangeYoloEventV3YoloResult>(&event)) {
                    count_frame_line(&log, product, result->frame.recording_frame_id, result->yolo.status);
                } else {
                    log.header_rows++;  // session_header, spatial_mask_policy
                }
            } else {
                pose::OrangePoseEventV3 event;
                if (event_log_reader::parse_pose_line(line, &event, &error) != event_log_reader::LineStatus::kOk) {
                    log.parse_errors++;
                } else if (const auto* result = std::get_if<pose::OrangePoseEventV3PoseResult>(&event)) {
                    count_frame_line(&log, product, result->frame.recording_frame_id, result->pose.status);
                } else {
                    log.header_rows++;  // session_header
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

// The product's model, from the snapshot block it names (models.<serial>.<kind>); empty and
// disabled when the block is absent or did not match its published schema.
nlohmann::json model_ref(const SnapshotModels& models, const std::string& serial, const std::string& kind)
{
    nlohmann::json ref = {{"models_key", "models." + serial + "." + kind}};
    auto fill = [&](const auto* block) {
        ref["enabled"] = block ? block->enabled : false;
        ref["model_id"] = block ? block->runtime.model_id : std::string();
        ref["engine_path"] = block ? block->runtime.engine_path : std::string();
        ref["engine_sha256"] = block ? block->runtime.engine_sha256 : std::string();
        ref["weights_sha256"] = block ? block->runtime.weights_sha256 : std::string();
    };
    const auto camera = models.find(serial);
    if (kind == "detect") {
        fill(camera != models.end() && camera->second.detect ? &*camera->second.detect : nullptr);
    } else {
        fill(camera != models.end() && camera->second.pose ? &*camera->second.pose : nullptr);
    }
    return ref;
}

nlohmann::json product_json(const fs::path& folder, const std::string& serial, const std::string& product,
                            const std::string& events_name, const std::string& perf_name,
                            const SnapshotModels& models)
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
    // The per-frame logs below are written by the process log writer thread
    // (and the YOLO/pose event loggers' own threads). Seal the folder: every
    // row queued before finalize is on disk, and no writer appends after the
    // hash (the acquisition thread samples pipeline perf until the finalizer
    // releases the folder, which is after this manifest is written).
    if (!orange::timing::LogWriter::Instance().SealFolder(recording_folder)) {
        std::cerr << "[realtime_products] log writer did not flush within its timeout; "
                     "per-frame log digests may describe partial files" << std::endl;
    }
    const fs::path folder(recording_folder);
    std::vector<std::string> model_errors;
    const SnapshotModels models = load_snapshot_models(folder / "recording_snapshot.json", &model_errors);
    for (const std::string& error : model_errors) {
        std::cerr << "[realtime_products] " << error << std::endl;
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
            const auto models_camera = models.find(serial);
            camera["pose"]["pose_crop_size_px"] =
                models_camera != models.end() && models_camera->second.pose
                    ? models_camera->second.pose->runtime.pose_crop_size_px : 0;
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
