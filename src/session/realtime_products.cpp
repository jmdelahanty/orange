#include "session/realtime_products.h"
#include "session/snapshot_models.h"
#include "event_log_reader.h"
#include "timing/log_writer.h"
#include "timing/phase_timer.h"
#include "timing/written_file_digest.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <map>
#include <optional>
#include <vector>

#include "gui/spatial_layout/sha256.h"

namespace orange::session {
namespace {

namespace fs = std::filesystem;
using event_log_reader::EventLogProduct;
using orange::timing::EventLogTally;
using DigestReport = orange::timing::DigestSourceReport;

// One declared file: its digest and, for an event log, its row tally.
struct DigestedFile {
    bool present = false;
    uint64_t size_bytes = 0;
    std::string sha256;
    EventLogTally tally;
};

// Reads the file once: the SHA-256 of every byte and, for an event log, every line through
// event_log_reader::tally_event_line (the count the loggers keep as they write).
bool reread_file(const fs::path& path, const EventLogProduct* product, DigestedFile* out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    orange::gui::spatial_layout::checksum::StreamingSha256 sha;
    std::vector<char> buffer(4 << 20);
    std::string partial;  // a line split across reads
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const size_t count = static_cast<size_t>(in.gcount());
        if (count == 0) {
            break;
        }
        sha.update(reinterpret_cast<const std::uint8_t*>(buffer.data()), count);
        out->size_bytes += count;
        if (!product) {
            continue;
        }
        size_t start = 0;
        while (start < count) {
            const char* begin = buffer.data() + start;
            const char* newline = static_cast<const char*>(std::memchr(begin, '\n', count - start));
            if (!newline) {
                partial.append(begin, count - start);
                break;
            }
            partial.append(begin, static_cast<size_t>(newline - begin));
            event_log_reader::tally_event_line(*product, partial, &out->tally);
            partial.clear();
            start = static_cast<size_t>(newline - buffer.data()) + 1;
        }
    }
    if (in.bad()) {
        return false;
    }
    if (product && !partial.empty()) {
        event_log_reader::tally_event_line(*product, partial, &out->tally);
    }
    out->sha256 = sha.final_hex();
    out->present = true;
    return true;
}

// The digest (and tally) of folder/name. A file this process's writers hashed while writing
// it, sealed, and still on disk exactly as they closed it (timing/written_file_digest.h) is
// not read again; anything else is read in full as before: no running digest (a writer outside
// the log writer, a file from an earlier process), or a failed check.
DigestedFile digest_file(const fs::path& folder, const std::string& name,
                         const std::optional<EventLogProduct>& product, DigestReport* report)
{
    DigestedFile out;
    const fs::path path = folder / name;
    if (!fs::exists(path)) {
        return out;
    }
    std::string reason = "no sealed running digest";
    if (const auto written = orange::timing::WrittenFileDigestRegistry::Instance().Sealed(path.string())) {
        if (product && !written->tally) {
            reason = "running digest has no event tally";
        } else if (orange::timing::VerifyAgainstDisk(*written, path.string(), &reason)) {
            out.present = true;
            out.size_bytes = written->bytes;
            out.sha256 = written->sha256;
            if (written->tally) out.tally = *written->tally;
            report->Add(name, true, out.size_bytes, {});
            return out;
        }
    }
    if (!reread_file(path, product ? &*product : nullptr, &out)) {
        return DigestedFile{};
    }
    report->Add(name, false, out.size_bytes, reason);
    return out;
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

nlohmann::json file_entry(const fs::path& folder, const std::string& name, const std::string& role,
                          DigestReport* report)
{
    const DigestedFile file = digest_file(folder, name, std::nullopt, report);
    if (!file.present) {
        return nullptr;
    }
    return {{"role", role}, {"path", name}, {"size_bytes", file.size_bytes}, {"sha256", file.sha256}};
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

// product: "detections" (Cam<serial>_yolo_events.jsonl) or "pose" (Cam<serial>_pose_events.jsonl).
// Every line is counted through the generated parser of orange.yolo_event / orange.pose_event v3
// (event_log_reader::tally_event_line); a line that is not JSON or does not match the schema
// counts as a parse error, never as a row. line_schema reports what the first line declares.
nlohmann::json product_json(const fs::path& folder, const std::string& serial, const std::string& product,
                            const std::string& events_name, const std::string& perf_name,
                            const SnapshotModels& models, DigestReport* report)
{
    const DigestedFile log = digest_file(
        folder, events_name,
        product == "detections" ? EventLogProduct::kDetections : EventLogProduct::kPose, report);
    nlohmann::json out = {
        {"status", log.present ? "present" : "absent"},
        {"model_ref", model_ref(models, serial, product == "detections" ? "detect" : "pose")},
        {"files", nlohmann::json::array()}
    };
    if (!log.present) {
        return out;
    }
    std::map<std::string, uint64_t> rows_by_kind;
    for (const char* kind : {"result", "no_result", "failed", "other"}) {
        rows_by_kind[kind] = 0;
    }
    for (const auto& [status, count] : log.tally.rows_by_status) {
        rows_by_kind[kind_for_status(product, status)] += count;
    }
    out["line_schema"] = {{"schema_id", log.tally.schema_id}, {"schema_version", log.tally.schema_version}};
    out["frame_identity_key"] = "frame.recording_frame_id";
    out["row_count"] = log.tally.row_count;
    out["header_rows"] = log.tally.header_rows;
    out["parse_errors"] = log.tally.parse_errors;
    out["first_recording_frame_id"] = log.tally.first_recording_frame_id;
    out["last_recording_frame_id"] = log.tally.last_recording_frame_id;
    out["rows_by_kind"] = rows_by_kind;
    out["rows_by_status"] = log.tally.rows_by_status;
    out["files"].push_back({{"role", "events"}, {"path", events_name},
                            {"size_bytes", log.size_bytes}, {"sha256", log.sha256}});
    const nlohmann::json perf = file_entry(folder, perf_name, "perf", report);
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
    // releases the folder, which is after this manifest is written). A
    // completed seal also makes the writers' running digests usable here.
    if (!orange::timing::LogWriter::Instance().SealFolder(recording_folder)) {
        std::cerr << "[realtime_products] log writer did not flush within its timeout; "
                     "per-frame log digests may describe partial files" << std::endl;
    }
    orange::timing::MarkPhase("realtime_products.seal");
    const fs::path folder(recording_folder);
    std::vector<std::string> model_errors;
    const SnapshotModels models = load_snapshot_models(folder / "recording_snapshot.json", &model_errors);
    for (const std::string& error : model_errors) {
        std::cerr << "[realtime_products] " << error << std::endl;
    }
    DigestReport report;
    nlohmann::json cameras = nlohmann::json::object();
    for (const std::string& serial : camera_serials) {
        const std::string prefix = "Cam" + serial;
        nlohmann::json camera = {
            {"detections", product_json(folder, serial, "detections",
                                        prefix + "_yolo_events.jsonl", prefix + "_yolo_perf.csv", models, &report)},
            {"pose", product_json(folder, serial, "pose",
                                  prefix + "_pose_events.jsonl", prefix + "_pose_perf.csv", models, &report)},
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
            const nlohmann::json entry = file_entry(folder, name, role, &report);
            if (!entry.is_null()) camera["crop_files"].push_back(entry);
        }
        for (const auto& [name, role] : std::vector<std::pair<std::string, std::string>>{
                 {prefix + "_acquisition_cadence_probe.csv", "diagnostic"},
                 {prefix + "_ring_release.csv", "diagnostic"},
                 {prefix + "_owner_push.csv", "diagnostic"},
                 {prefix + "_pipeline_perf.csv", "perf"}}) {
            const nlohmann::json entry = file_entry(folder, name, role, &report);
            if (!entry.is_null()) camera["acquisition_files"].push_back(entry);
        }
        cameras[serial] = std::move(camera);
    }
    // How each digest was obtained: Orange-internal, in the finalize timing
    // report (the published block above is the same either way).
    std::cout << "[realtime_products] digests: " << report.writer_files << " file(s) from the writers' running digests ("
              << report.writer_bytes << " bytes), " << report.reread_files << " re-read (" << report.reread_bytes
              << " bytes)" << std::endl;
    orange::timing::NotePhase("realtime_products_digests", report.ToJson());
    return {
        {"schema_id", kRealtimeProductsSchemaId},
        {"schema_version", kRealtimeProductsSchemaVersion},
        {"paths_relative_to", "recording_folder"},
        {"digest", "sha256 hex of the file bytes, as in the transfer inventory"},
        {"cameras", std::move(cameras)}
    };
}

}  // namespace orange::session
