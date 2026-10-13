// Unit test for orange::session::build_realtime_products_json: a temp
// recording folder with two small event logs, perf CSVs and a snapshot.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "gui/spatial_layout/sha256.h"
using orange::gui::spatial_layout::checksum::sha256_hex;
#include "session/realtime_products.h"

namespace fs = std::filesystem;

static int failures = 0;
static void require(bool ok, const char* what)
{
    if (!ok) { std::cerr << "[FAIL] " << what << std::endl; ++failures; }
    else { std::cout << "[PASS] " << what << std::endl; }
}

static void put(const fs::path& p, const std::string& s) { std::ofstream(p) << s; }

const std::string kDetSha(64, 'a');

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

    const nlohmann::json block = orange::session::build_realtime_products_json(dir.string(), {"2010093", "2010094"});
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
    fs::remove_all(dir);
    if (failures == 0) std::cout << "realtime_products_tests passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
