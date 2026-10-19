// Unit test for orange::session::build_realtime_products_json: a temp
// recording folder with two small event logs, perf CSVs and a snapshot
// (re-read at finalize); then logs written through the real writers, whose
// running digests finalize uses without re-reading, and which must give the
// same block as a full re-read; then the cost of both paths.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "gui/spatial_layout/sha256.h"
using orange::gui::spatial_layout::checksum::sha256_hex;
#include "event_log_reader.h"
#include "pose_event_log.h"
#include "session/realtime_products.h"
#include "timing/log_writer.h"
#include "timing/phase_timer.h"
#include "yolo_event_log.h"

// Filesystem seam normally provided by the application-wide project.cpp.
bool make_folder(std::string path)
{
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

namespace fs = std::filesystem;

static int failures = 0;
static void require(bool ok, const char* what)
{
    if (!ok) { std::cerr << "[FAIL] " << what << std::endl; ++failures; }
    else { std::cout << "[PASS] " << what << std::endl; }
}

static void put(const fs::path& p, const std::string& s) { std::ofstream(p) << s; }

const std::string kDetSha(64, 'a');

static yolo_event_log::YoloResultRecord yolo_record(const fs::path& folder, uint64_t frame, const std::string& status)
{
    yolo_event_log::YoloResultRecord r;
    r.recording_folder = folder.string();
    r.recording_frame_id = frame;
    r.local_frame_id = 1000 + frame;
    r.camera_frame_id = 1000 + frame;
    r.record_active = true;
    r.camera_timestamp = 1791000000000000000ULL + frame * 10000000ULL;
    r.event_epoch_us = 1791000000000000ULL + frame * 10000ULL;
    r.source_width = 4512;
    r.source_height = 4512;
    r.status = status;
    if (status == "detections") {
        pose::Object o{};
        o.rect.x = 100.0f + static_cast<float>(frame % 50);
        o.rect.y = 200.0f;
        o.rect.width = 30.0f;
        o.rect.height = 40.0f;
        o.prob = 0.9f;
        r.detections = {o};
        r.spatial_mask.raw_detection_count = 1;
        r.spatial_mask.downstream_detection_count = 1;
    }
    return r;
}

static pose_event_log::PoseResultRecord pose_record(const fs::path& folder, uint64_t frame, const std::string& status)
{
    pose_event_log::PoseResultRecord r;
    r.recording_folder = folder.string();
    r.backend = "tensorrt";
    r.mode = "real";
    r.model_id = "pose-model";
    r.keypoint_labels = {"head", "mid", "tail"};
    r.recording_frame_id = frame;
    r.local_frame_id = 1000 + frame;
    r.camera_frame_id = 1000 + frame;
    r.record_active = true;
    r.event_epoch_us = 1791000000000000ULL + frame * 10000ULL;
    r.event_monotonic_us = 5000000ULL + frame * 10000ULL;
    r.source_width = 4512;
    r.source_height = 4512;
    r.crop_width_px = 192;
    r.crop_height_px = 192;
    r.status = status;
    r.timing.capture_to_pose_done_ms = 2.4321;
    if (status == "poses") {
        pose_event_log::PoseInstanceRecord p;
        p.confidence = 0.8;
        for (int k = 0; k < 3; ++k) p.keypoints.push_back({"", 10.0 + k, 20.0 + k, 0.9, true});
        r.poses = {p};
    }
    return r;
}

struct PerfRow {
    uint64_t frame = 0;
};
static void format_perf(const PerfRow& row, std::string& out)
{
    out += std::to_string(row.frame);
    out += ",1.25,0.5\n";
}

// Writes `frames` frames of each log for camera 2010093 under `folder` through the real
// writers (YOLO and pose event loggers, a LogChannel CSV), the way a recording does, with a
// rotation away and back (the event logs reopen in append mode). Frame 7 of the detector log
// carries a status outside the schema's enum and frame 9 of the pose log a NaN latency (written
// as null): valid to the writer, refused by the schema, so both count as parse errors.
static void write_with_real_writers(const fs::path& folder, const fs::path& elsewhere, uint64_t frames)
{
    yolo_event_log::YoloEventLogger yolo("2010093", 0, "YoloWorker");
    pose_event_log::PoseEventLogger pose("2010093", 0, "PoseWorker");
    auto perf = orange::timing::LogChannel<PerfRow>::Create({"pipeline_perf", 1 << 14}, &format_perf);
    perf->Open((folder / "Cam2010093_pipeline_perf.csv").string(), "frame,a_ms,b_ms\n");
    const char* yolo_statuses[] = {"detections", "zero_detections", "timeout", "failed"};
    const char* pose_statuses[] = {"poses", "no_result", "failed"};
    for (uint64_t frame = 1; frame <= frames; ++frame) {
        yolo.Enqueue(yolo_record(folder, frame, frame == 7 ? "bogus" : yolo_statuses[frame % 4]));
        auto pose_line = pose_record(folder, frame, pose_statuses[frame % 3]);
        if (frame == 9) pose_line.timing.capture_to_pose_done_ms = std::nan("");
        pose.Enqueue(pose_line);
        perf->Emit({frame});
        if (frame == frames / 2) {
            yolo.Enqueue(yolo_record(elsewhere, 1, "detections"));
            pose.Enqueue(pose_record(elsewhere, 1, "poses"));
        }
        if (frame % 2000 == 0) {  // the loggers' queues hold 8192 events
            yolo.Flush(std::chrono::seconds(30));
            pose.Flush(std::chrono::seconds(30));
        }
    }
    perf->Retire();
    yolo.Close();
    pose.Close();
    yolo.Flush(std::chrono::seconds(30));
    pose.Flush(std::chrono::seconds(30));
}

static nlohmann::json build_with_report(const fs::path& folder, nlohmann::json* report, double* ms)
{
    orange::timing::PhaseTimer timer;
    nlohmann::json block;
    const auto start = std::chrono::steady_clock::now();
    {
        orange::timing::ScopedPhaseTimer scoped(&timer);
        block = orange::session::build_realtime_products_json(folder.string(), {"2010093"});
    }
    *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    *report = timer.ToJson().at("notes").at("realtime_products_digests");
    return block;
}

static bool all_sources(const nlohmann::json& report, const std::string& source, size_t count)
{
    if (report.at("files").size() != count) return false;
    for (const auto& file : report.at("files")) {
        if (file.value("digest_source", "") != source) return false;
    }
    return true;
}

static void bump_mtimes(const fs::path& folder)
{
    for (const auto& entry : fs::directory_iterator(folder)) {
        if (entry.is_regular_file()) fs::last_write_time(entry.path(), fs::last_write_time(entry.path()) + std::chrono::seconds(1));
    }
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: realtime_products_tests <recording_snapshot_models_excerpt.json> <golden dir>" << std::endl;
        return 2;
    }
    const fs::path dir = fs::temp_directory_path() / ("realtime_products_tests_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    // The golden v3 logs (tools/fixtures/generated_outputs/golden), plus two lines that must count
    // as parse errors and never as rows: not JSON, and a frame line of another schema version.
    auto slurp = [](const fs::path& path) { std::ifstream in(path); std::stringstream b; b << in.rdbuf(); return b.str(); };
    const std::string golden_yolo = slurp(fs::path(argv[2]) / "Cam2010093_yolo_events.jsonl");
    nlohmann::json older = nlohmann::json::parse(golden_yolo.substr(golden_yolo.find('\n') + 1,
        golden_yolo.find('\n', golden_yolo.find('\n') + 1) - golden_yolo.find('\n') - 1));
    older["schema_version"] = 2;
    const std::string yolo = golden_yolo + "not json\n" + older.dump() + "\n";
    const std::string pose = slurp(fs::path(argv[2]) / "Cam2010093_pose_events.jsonl");
    put(dir / "Cam2010093_yolo_events.jsonl", yolo);
    put(dir / "Cam2010093_pose_events.jsonl", pose);
    put(dir / "Cam2010093_yolo_perf.csv", "a,b\n1,2\n");
    put(dir / "Cam2010093_crop_meta.csv", "x\n");
    put(dir / "Cam2010093_owner_push.csv", "k\n");
    // Model blocks: the real excerpt (tools/fixtures/generated_outputs) with this test's values.
    std::ifstream excerpt_in(argv[1]);
    const nlohmann::json excerpt = nlohmann::json::parse(excerpt_in).at("models").begin().value();
    nlohmann::json detect = excerpt.at("detect"), pose_block = excerpt.at("pose");
    detect.merge_patch({{"enabled", true}, {"runtime", {{"model_id", "det-model"}, {"engine_path", "/e/det.engine"}, {"engine_sha256", kDetSha}}}});
    pose_block.merge_patch({{"enabled", true}, {"runtime", {{"model_id", "pose-model"}, {"engine_sha256", std::string(64, 'c')}, {"pose_crop_size_px", 192}}}});
    put(dir / "recording_snapshot.json", nlohmann::json{{"models", {{"2010093", {{"detect", detect}, {"pose", pose_block}}}}}}.dump());

    orange::timing::PhaseTimer timer;
    nlohmann::json block;
    {
        orange::timing::ScopedPhaseTimer scoped(&timer);
        block = orange::session::build_realtime_products_json(dir.string(), {"2010093", "2010094"});
    }
    const nlohmann::json reread_report = timer.ToJson().at("notes").at("realtime_products_digests");
    require(all_sources(reread_report, "finalize_reread", 5) &&
            reread_report.at("files")[0].value("reason", "") == "no sealed running digest",
            "files no writer of this process wrote are re-read (reported in the finalize timing notes)");
    require(block.value("schema_id", "") == "orange.recording_realtime_products" && block.value("schema_version", 0) == 1, "schema identity");
    const nlohmann::json& c = block.at("cameras").at("2010093");
    const nlohmann::json& d = c.at("detections");
    // Golden detector log: header, 3 frame lines, a policy line, a frame line outside recording.
    require(d.value("status", "") == "present" && d.value("row_count", 0) == 4 && d.value("header_rows", 0) == 2 &&
            d.value("parse_errors", 0) == 2, "detections rows, header rows and parse errors");
    require(d.at("rows_by_kind").value("result", 0) == 4 && d.at("rows_by_kind").value("failed", 0) == 0 &&
            d.at("rows_by_status").value("zero_detections", 0) == 1 && d.at("rows_by_status").value("detections", 0) == 3,
            "detections rows by kind and status");
    require(d.value("first_recording_frame_id", 0) == 1 && d.value("last_recording_frame_id", 0) == 3, "detections frame range");
    require(d.at("line_schema").value("schema_id", "") == "orange.yolo_event" && d.at("line_schema").value("schema_version", 0) == 3, "detections line schema");
    require(d.at("model_ref").value("models_key", "") == "models.2010093.detect" && d.at("model_ref").value("engine_sha256", "") == kDetSha, "detections model ref");
    require(d.at("files").size() == 2 && d.at("files")[0].value("path", "") == "Cam2010093_yolo_events.jsonl" &&
            d.at("files")[0].value("sha256", "") == sha256_hex(yolo) && d.at("files")[0].value("size_bytes", 0) == static_cast<int>(yolo.size()) &&
            d.at("files")[1].value("role", "") == "perf", "detections files with digests");
    const nlohmann::json& p = c.at("pose");
    require(p.value("row_count", 0) == 3 && p.value("header_rows", 0) == 1 && p.value("parse_errors", 0) == 0 &&
            p.at("rows_by_kind").value("result", 0) == 1 && p.at("rows_by_kind").value("no_result", 0) == 1 &&
            p.at("rows_by_kind").value("failed", 0) == 1 && p.value("pose_crop_size_px", 0) == 192, "pose rows, header, crop size");
    require(c.at("crop_files").size() == 1 && c.at("acquisition_files").size() == 1, "crop and acquisition files");
    const nlohmann::json& absent = block.at("cameras").at("2010094");
    require(absent.at("detections").value("status", "") == "absent" && absent.at("pose").value("status", "") == "absent" &&
            absent.at("detections").at("files").empty(), "absent camera declared absent");

    // 2. Logs written through the real writers: finalize takes the writers' running digests
    //    and tallies (no re-read), and the block equals the one a full re-read gives.
    {
        const fs::path written = dir / "written";
        fs::create_directories(written);
        put(written / "recording_snapshot.json", slurp(dir / "recording_snapshot.json"));
        write_with_real_writers(written, dir / "elsewhere", 40);
        nlohmann::json from_writer_report, reread_report_2;
        double ms = 0.0;
        const nlohmann::json from_writer = build_with_report(written, &from_writer_report, &ms);
        require(all_sources(from_writer_report, "writer_running_digest", 3),
                "event logs and the LogChannel CSV come from the writers' running digests");
        const nlohmann::json& wd = from_writer.at("cameras").at("2010093").at("detections");
        const nlohmann::json& wp = from_writer.at("cameras").at("2010093").at("pose");
        require(wd.value("row_count", 0) == 39 && wd.value("header_rows", 0) == 1 && wd.value("parse_errors", 0) == 1 &&
                wd.value("first_recording_frame_id", 0) == 1 && wd.value("last_recording_frame_id", 0) == 40 &&
                wd.at("rows_by_status").value("bogus", 0) == 0 && wd.at("rows_by_kind").value("failed", 0) == 19,
                "detector tally: frame rows across the append reopen, header, the enum violation as a parse error");
        require(wp.value("row_count", 0) == 39 && wp.value("header_rows", 0) == 1 && wp.value("parse_errors", 0) == 1,
                "pose tally: the NaN latency (written as null) is a parse error");
        require(wd.at("files")[0].value("sha256", "") == sha256_hex(slurp(written / "Cam2010093_yolo_events.jsonl")) &&
                wp.at("files")[0].value("sha256", "") == sha256_hex(slurp(written / "Cam2010093_pose_events.jsonl")) &&
                from_writer.at("cameras").at("2010093").at("acquisition_files")[0].value("sha256", "") ==
                    sha256_hex(slurp(written / "Cam2010093_pipeline_perf.csv")),
                "running digests equal the SHA-256 of the files on disk");
        bump_mtimes(written);
        const nlohmann::json reread = build_with_report(written, &reread_report_2, &ms);
        require(all_sources(reread_report_2, "finalize_reread", 3) &&
                reread_report_2.at("files")[0].value("reason", "") == "modified since the writer closed it",
                "a file touched after the seal is re-read");
        require(reread == from_writer, "the block from the running digests equals the block from a full re-read");
        require(!orange::timing::WrittenFileDigestRegistry::Instance().Sealed((dir / "elsewhere" / "Cam2010093_yolo_events.jsonl").string()),
                "an unsealed folder's digests are not offered");
    }

    // 3. Cost: the same block from the running digests and from a full re-read of logs
    //    about the size of 2.5 minutes of one camera (printed; bounded loosely).
    {
        const fs::path large = dir / "large";
        fs::create_directories(large);
        put(large / "recording_snapshot.json", slurp(dir / "recording_snapshot.json"));
        write_with_real_writers(large, dir / "elsewhere_large", 15000);
        uint64_t bytes = 0;
        for (const auto& entry : fs::directory_iterator(large)) bytes += entry.file_size();
        nlohmann::json report;
        double writer_ms = 0.0, reread_ms = 0.0;
        const nlohmann::json a = build_with_report(large, &report, &writer_ms);
        require(all_sources(report, "writer_running_digest", 3), "large: running digests used");
        bump_mtimes(large);
        const nlohmann::json b = build_with_report(large, &report, &reread_ms);
        require(all_sources(report, "finalize_reread", 3) && a == b, "large: re-read gives the same block");
        const double mib = static_cast<double>(bytes) / (1 << 20);
        std::cout << "realtime products over " << mib << " MiB: running digests " << writer_ms << " ms, full re-read "
                  << reread_ms << " ms (" << reread_ms / mib << " ms per MiB)" << std::endl;
        require(writer_ms < reread_ms, "running digests are cheaper than a re-read");
        // The per-line tally the loggers now run as they write.
        const std::string yolo_log = slurp(large / "Cam2010093_yolo_events.jsonl");
        const std::string pose_log = slurp(large / "Cam2010093_pose_events.jsonl");
        for (const auto& [label, text, product] :
             {std::tuple<const char*, const std::string*, event_log_reader::EventLogProduct>{
                  "detector", &yolo_log, event_log_reader::EventLogProduct::kDetections},
              std::tuple<const char*, const std::string*, event_log_reader::EventLogProduct>{
                  "pose", &pose_log, event_log_reader::EventLogProduct::kPose}}) {
            orange::timing::EventLogTally tally;
            uint64_t lines = 0;
            const auto start = std::chrono::steady_clock::now();
            for (size_t begin = 0; begin < text->size();) {
                const size_t end = text->find('\n', begin);
                event_log_reader::tally_event_line(product, text->substr(begin, end - begin), &tally);
                ++lines;
                begin = end + 1;
            }
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
            std::cout << label << " tally: " << us / static_cast<double>(lines) << " us per line ("
                      << static_cast<double>(text->size()) / static_cast<double>(lines) << " bytes per line)" << std::endl;
            require(us / static_cast<double>(lines) < 500.0, "tally per line is bounded");
        }
    }

    fs::remove_all(dir);
    if (failures == 0) std::cout << "realtime_products_tests passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
