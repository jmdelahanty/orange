// Unit test for orange::session::build_realtime_products_json: a temp
// recording folder with two small event logs, perf CSVs and a snapshot.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
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

int main()
{
    const fs::path dir = fs::temp_directory_path() / ("realtime_products_tests_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const std::string yolo =
        R"({"schema_id":"orange.yolo_event","schema_version":1,"event_sequence":1,"event_kind":"yolo_result","frame":{"recording_frame_id":5},"yolo":{"status":"detections","detection_count":1},"detections":[{}]})" "\n"
        R"({"schema_id":"orange.yolo_event","schema_version":1,"event_sequence":2,"event_kind":"yolo_result","frame":{"recording_frame_id":6},"yolo":{"status":"zero_detections","detection_count":0},"detections":[]})" "\n"
        "not json\n"
        R"({"schema_id":"orange.yolo_event","schema_version":1,"event_sequence":3,"event_kind":"yolo_result","frame":{"recording_frame_id":9},"yolo":{"status":"timeout"},"detections":[]})" "\n";
    const std::string pose =
        R"({"schema_id":"orange.pose_event","schema_version":1,"event_kind":"session_header"})" "\n"
        R"({"schema_id":"orange.pose_event","schema_version":1,"event_sequence":1,"event_kind":"pose_result","frame":{"recording_frame_id":5},"pose":{"status":"poses"},"poses":[{}]})" "\n"
        R"({"schema_id":"orange.pose_event","schema_version":1,"event_sequence":2,"event_kind":"pose_result","frame":{"recording_frame_id":6},"pose":{"status":"no_result"},"poses":[]})" "\n";
    put(dir / "Cam2010093_yolo_events.jsonl", yolo);
    put(dir / "Cam2010093_pose_events.jsonl", pose);
    put(dir / "Cam2010093_yolo_perf.csv", "a,b\n1,2\n");
    put(dir / "Cam2010093_crop_meta.csv", "x\n");
    put(dir / "Cam2010093_owner_push.csv", "k\n");
    put(dir / "recording_snapshot.json", nlohmann::json{{"models", {{"2010093", {
        {"detect", {{"enabled", true}, {"runtime", {{"model_id", "det-model"}, {"engine_path", "/e/det.engine"}, {"engine_sha256", "ab"}}}}},
        {"pose", {{"enabled", true}, {"runtime", {{"model_id", "pose-model"}, {"engine_sha256", "cd"}, {"pose_crop_size_px", 192}}}}}}}}}}.dump());

    const nlohmann::json block = orange::session::build_realtime_products_json(dir.string(), {"2010093", "2010094"});
    require(block.value("schema_id", "") == "orange.recording_realtime_products" && block.value("schema_version", 0) == 1, "schema identity");
    const nlohmann::json& c = block.at("cameras").at("2010093");
    const nlohmann::json& d = c.at("detections");
    require(d.value("status", "") == "present" && d.value("row_count", 0) == 3 && d.value("parse_errors", 0) == 1, "detections rows and parse errors");
    require(d.at("rows_by_kind").value("result", 0) == 2 && d.at("rows_by_kind").value("failed", 0) == 1, "detections rows by kind");
    require(d.value("first_recording_frame_id", 0) == 5 && d.value("last_recording_frame_id", 0) == 9, "detections frame range");
    require(d.at("line_schema").value("schema_id", "") == "orange.yolo_event" && d.at("line_schema").value("schema_version", 0) == 1, "detections line schema");
    require(d.at("model_ref").value("models_key", "") == "models.2010093.detect" && d.at("model_ref").value("engine_sha256", "") == "ab", "detections model ref");
    require(d.at("files").size() == 2 && d.at("files")[0].value("path", "") == "Cam2010093_yolo_events.jsonl" &&
            d.at("files")[0].value("sha256", "") == sha256_hex(yolo) && d.at("files")[0].value("size_bytes", 0) == static_cast<int>(yolo.size()) &&
            d.at("files")[1].value("role", "") == "perf", "detections files with digests");
    const nlohmann::json& p = c.at("pose");
    require(p.value("row_count", 0) == 2 && p.value("header_rows", 0) == 1 && p.at("rows_by_kind").value("result", 0) == 1 &&
            p.at("rows_by_kind").value("no_result", 0) == 1 && p.value("pose_crop_size_px", 0) == 192, "pose rows, header, crop size");
    require(c.at("crop_files").size() == 1 && c.at("acquisition_files").size() == 1, "crop and acquisition files");
    const nlohmann::json& absent = block.at("cameras").at("2010094");
    require(absent.at("detections").value("status", "") == "absent" && absent.at("pose").value("status", "") == "absent" &&
            absent.at("detections").at("files").empty(), "absent camera declared absent");
    fs::remove_all(dir);
    if (failures == 0) std::cout << "realtime_products_tests passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
