// src/timing/diagnostics_index.h
//
// <recording>/diagnostics_index.json: every timing/diagnostic file in the
// recording folder, what each time column measures (time base, unit), how
// many rows the log writer wrote and dropped, and monotonic/realtime clock
// anchors for the folder. Orange-internal (Jeremy 2026-10-11): written into
// the folder, not declared in recording_session.json.
// Design: docs/unified_timing_and_diagnostics_design_2026_10_11.md, 5.3.
//
// Time bases (src/timing/clock.h): camera, camera_latched, host_realtime,
// host_monotonic, gpu_elapsed, mixed, utc_string.
#pragma once

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "fsuid_guard.h"
#include "json.hpp"
#include "timing/clock.h"
#include "timing/log_writer.h"

namespace orange::timing {

struct ColumnDeclaration {
    const char* name;
    const char* time_base;
    const char* unit;
    const char* kind;  // "timestamp", "duration", "offset" or "label"
    const char* note;
};

struct FileDeclaration {
    const char* suffix;          // file name suffix; the longest match wins
    const char* format;          // csv, jsonl, json
    const char* level;           // contract, declared_diagnostic, internal
    const char* missing_value;   // sentinel for "not measured", or "" when none
    const char* default_ms_time_base;  // time base of *_ms columns not listed
    std::vector<ColumnDeclaration> columns;
    const char* note;
};

inline const std::vector<FileDeclaration>& diagnostics_catalog()
{
    static const std::vector<FileDeclaration> catalog = {
        {"_pipeline_perf.csv", "csv", "declared_diagnostic", "", "",
         {{"timestamp_utc", "utc_string", "s", "timestamp", "host realtime, one-second resolution"},
          {"detect_priority_wait_total_ns", "host_monotonic", "ns", "duration", "cumulative"},
          {"detect_priority_wait_max_ns", "host_monotonic", "ns", "duration", ""}},
         "1 Hz per-camera pipeline counters"},
        {"_acquisition_cadence_probe.csv", "csv", "declared_diagnostic", "", "",
         {{"timestamp_utc", "utc_string", "s", "timestamp", "host realtime, one-second resolution"},
          {"camera_timestamp_ns", "camera", "ns", "timestamp", ""},
          {"camera_timestamp_delta_ns", "camera", "ns", "duration", "previous frame to this frame"},
          {"receive_host_ns", "host_monotonic", "ns", "timestamp", "after EVT_CameraGetFrame returned"},
          {"receive_delta_ns", "host_monotonic", "ns", "duration", ""},
          {"get_frame_wait_ns", "host_monotonic", "ns", "duration", "inside EVT_CameraGetFrame"},
          {"latched_ptp_time_ns", "camera_latched", "ns", "timestamp", "GevTimestampValue register"},
          {"latch_minus_frame_ns", "camera", "ns", "duration", "latched register minus frame timestamp"},
          {"latch_delta_ns", "camera_latched", "ns", "duration", ""},
          {"recording_submit_host_ns", "host_monotonic", "ns", "timestamp", ""},
          {"receive_to_submit_ns", "host_monotonic", "ns", "duration", ""},
          {"helper_enqueue_delay_ns", "host_monotonic", "ns", "duration", ""},
          {"ptp_latch_ns", "host_monotonic", "ns", "duration", "register latch round trips"}},
         "recording frames 80-160, or every frame with ORANGE_ACQ_CADENCE_PROBE_ALL"},
        {"_ring_release.csv", "csv", "declared_diagnostic", "", "",
         {{"receive_host_ns", "host_monotonic", "ns", "timestamp", ""},
          {"copy_ready_seen_ns", "host_monotonic", "ns", "timestamp", ""},
          {"consumer_done_seen_ns", "host_monotonic", "ns", "timestamp", ""},
          {"requeue_host_ns", "host_monotonic", "ns", "timestamp", ""},
          {"hold_ns", "host_monotonic", "ns", "duration", "requeue minus receive"}},
         "ORANGE_ACQ_RING_RELEASE_LOG"},
        {"_owner_push.csv", "csv", "declared_diagnostic", "", "",
         {{"capture_sys_ns", "host_realtime", "ns", "timestamp", "same value as timestamp_sys"},
          {"push_start_steady_ns", "host_monotonic", "ns", "timestamp", "0 on non-push rows"},
          {"push_done_steady_ns", "host_monotonic", "ns", "timestamp", "0 on non-push rows"},
          {"wait_ms", "host_monotonic", "ms", "duration", "push done minus start"},
          {"age_at_push_ms", "host_realtime", "ms", "duration",
           "CLOCK_REALTIME now minus timestamp_sys (a wall-clock difference)"}},
         ""},
        {"_yolo_perf.csv", "csv", "declared_diagnostic", "-1", "host_monotonic",
         {{"timestamp", "camera", "ns", "timestamp", ""},
          {"timestamp_sys", "host_realtime", "ns", "timestamp",
           "read on the acquisition thread after the PTP check"},
          {"wait_ms", "gpu_elapsed", "ms", "duration", "cudaEventElapsedTime"},
          {"pre_ms", "gpu_elapsed", "ms", "duration", "cudaEventElapsedTime"},
          {"gap_ms", "gpu_elapsed", "ms", "duration", "cudaEventElapsedTime"},
          {"infer_ms", "gpu_elapsed", "ms", "duration", "cudaEventElapsedTime"},
          {"early_copy_ms", "gpu_elapsed", "ms", "duration", "cudaEventElapsedTime"},
          {"queue_ms", "mixed", "ms", "duration", "host sync_ms minus GPU infer_ms, clamped at 0"}},
         "every other *_ms column is a host monotonic duration"},
        {"_pose_perf.csv", "csv", "declared_diagnostic", "", "host_monotonic",
         {{"device_stage_gpu_mean_ms", "gpu_elapsed", "ms", "duration", "also _p50/_p95/_p99/_max"}},
         "one summary row per recording; *_ms are host monotonic latency statistics"},
        {"_pose_events.jsonl", "jsonl", "contract", "0.0", "host_monotonic",
         {{"timestamps.camera_timestamp", "camera", "ns", "timestamp", ""},
          {"timestamps.timestamp_sys_ns", "host_realtime", "ns", "timestamp", ""},
          {"timestamps.event_epoch_us", "host_realtime", "us", "timestamp",
           "when the pose result was built (since 9e36a2e)"},
          {"timestamps.event_monotonic_us", "host_monotonic", "us", "timestamp", "same moment"}},
         "latency_ms.* are host monotonic durations; 0.0 also means not measured"},
        {"_yolo_events.jsonl", "jsonl", "contract", "", "",
         {{"timestamps.camera_timestamp", "camera", "ns", "timestamp", ""},
          {"timestamps.timestamp_sys_ns", "host_realtime", "ns", "timestamp", ""},
          {"timestamps.event_epoch_us", "host_realtime", "us", "timestamp", "when the result was built"},
          {"timestamps.event_monotonic_us", "host_monotonic", "us", "timestamp", "same moment"}},
         ""},
        {"_crop_meta.csv", "csv", "contract", "", "",
         {{"timestamp", "camera", "ns", "timestamp", ""},
          {"timestamp_sys", "host_realtime", "ns", "timestamp", ""}},
         ""},
        {"_crop_perf.csv", "csv", "declared_diagnostic", "", "host_monotonic",
         {{"worker_start_steady_ns", "host_monotonic", "ns", "timestamp", "crop producer worker start"},
          {"crop_copy_gpu_ms", "gpu_elapsed", "ms", "duration", "-1 unless ORANGE_CROP_COPY_TIMING"}},
         "other *_ms columns are host monotonic durations"},
        {"_crop_sidecar_perf.csv", "csv", "declared_diagnostic", "", "", {}, "counters, no time columns"},
        {"_meta.csv", "csv", "contract", "", "",
         {{"timestamp", "camera", "ns", "timestamp", ""},
          {"timestamp_sys", "host_realtime", "ns", "timestamp", ""}},
         ""},
        {"_master_frames_v1.csv", "csv", "contract", "", "",
         {{"camera_timestamp_ns", "camera", "ns", "timestamp", ""},
          {"host_receive_steady_ns", "host_monotonic", "ns", "timestamp", ""},
          {"host_realtime_ns", "host_realtime", "ns", "timestamp", "same value as timestamp_sys"}},
         ""},
        {"ptp_sync_summary.json", "json", "contract", "", "",
         {{"cameras.*.last_frame_timestamp_ns", "camera", "ns", "timestamp", ""},
          {"cameras.*.last_latched_ptp_time_ns", "camera_latched", "ns", "timestamp", ""},
          {"*_utc", "utc_string", "s", "timestamp", ""}},
         ""},
        {"recording_startup_audit.jsonl", "jsonl", "internal", "", "",
         {{"t_steady_ns", "host_monotonic", "ns", "timestamp", ""},
          {"session_steady_ns", "host_monotonic", "ns", "timestamp", ""},
          {"recording_enabled_steady_ns", "host_monotonic", "ns", "timestamp", ""},
          {"session_realtime_ns", "host_realtime", "ns", "timestamp", "read with session_steady_ns"},
          {"t_rel_enabled_ms", "host_monotonic", "ms", "offset", "from recording enabled"}},
         "detail strings embed further values (receive_realtime_ns is host realtime)"},
        {"gui_timing_windows.csv", "csv", "internal", "", "host_monotonic",
         {{"window_start_offset_ms", "host_monotonic", "ms", "offset", "from the timing session start"},
          {"window_end_offset_ms", "host_monotonic", "ms", "offset", ""}},
         "*_ms statistics are host monotonic durations"},
        {"nic_thermal_monitor.csv", "csv", "declared_diagnostic", "", "",
         {{"monotonic_elapsed_ns", "host_monotonic", "ns", "offset", "from the helper's start"},
          {"realtime_posix_ns", "host_realtime", "ns", "timestamp", ""}},
         ""},
        {"orange_local_control.events.jsonl", "jsonl", "contract", "", "",
         {{"*_at_utc", "utc_string", "s", "timestamp", ""}}, ""},
        {"_external_meta.csv", "csv", "contract", "", "",
         {{"timestamp", "camera", "ns", "timestamp", "passed through from Orange"},
          {"timestamp_sys", "host_realtime", "ns", "timestamp", "passed through from Orange"}},
         ""},
        {"_external_encode.csv", "csv", "declared_diagnostic", "", "host_monotonic",
         {{"stage_copy_ms", "gpu_elapsed", "ms", "duration", ""},
          {"phase_wait_ms", "host_realtime", "ms", "duration", "hold until timestamp_sys + phase"},
          {"lock_bitstream_ms", "host_monotonic", "ms", "duration", "packets of earlier frames"},
          {"bitstream_fetch_ms", "host_monotonic", "ms", "duration", "packets of earlier frames"}},
         "recorder process; returned_packets/bytes belong to earlier frames"},
        {"_external_detach.csv", "csv", "declared_diagnostic", "", "host_monotonic",
         {{"copy_ms", "host_monotonic", "ms", "duration",
           "CPU time to queue the copy, not the copy itself"}},
         "recorder process"},
        {"_external_gop_routing.csv", "csv", "declared_diagnostic", "", "", {}, "no time columns"},
        {"_external_status.json", "json", "declared_diagnostic", "", "",
         {{"steady_clock_ns", "host_monotonic", "ns", "timestamp", "recorder process"}},
         ""},
    };
    return catalog;
}

inline const FileDeclaration* find_declaration(const std::string& file_name)
{
    const FileDeclaration* best = nullptr;
    size_t best_length = 0;
    for (const auto& declaration : diagnostics_catalog()) {
        const std::string suffix = declaration.suffix;
        if (file_name.size() >= suffix.size() &&
            file_name.compare(file_name.size() - suffix.size(), suffix.size(), suffix) == 0 &&
            suffix.size() > best_length) {
            best = &declaration;
            best_length = suffix.size();
        }
    }
    return best;
}

inline nlohmann::json declaration_json(const FileDeclaration& d)
{
    nlohmann::json columns = nlohmann::json::object();
    for (const auto& c : d.columns) {
        nlohmann::json column = {{"time_base", c.time_base}, {"unit", c.unit}, {"kind", c.kind}};
        if (c.note && *c.note) column["note"] = c.note;
        columns[c.name] = column;
    }
    nlohmann::json out = {{"format", d.format}, {"level", d.level}, {"time_columns", columns}};
    if (d.missing_value && *d.missing_value) out["missing_value"] = d.missing_value;
    if (d.default_ms_time_base && *d.default_ms_time_base) {
        out["other_ms_columns_time_base"] = d.default_ms_time_base;
    }
    if (d.note && *d.note) out["note"] = d.note;
    return out;
}

inline nlohmann::json build_diagnostics_index(const std::string& folder)
{
    namespace fs = std::filesystem;
    const fs::path root(folder);
    const std::vector<LogFileRecord> written = LogWriter::Instance().FilesUnder(folder);
    nlohmann::json files = nlohmann::json::array();
    std::error_code ec;
    std::vector<fs::path> paths;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec) && (name == "citrus" || name == "models")) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && find_declaration(name)) {
            paths.push_back(it->path());
        }
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        const FileDeclaration* d = find_declaration(path.filename().string());
        nlohmann::json entry = declaration_json(*d);
        entry["path"] = fs::relative(path, root, ec).generic_string();
        entry["size_bytes"] = fs::file_size(path, ec);
        for (const auto& record : written) {
            if (fs::path(record.path) == path) {
                entry["log_writer"] = {
                    {"channel", record.channel},
                    {"rows_written", record.rows_written},
                    {"rows_dropped_queue_full", record.rows_dropped_queue_full},
                    {"write_failures", record.write_failures},
                    {"opened_at_utc", utc_iso8601(RealtimeNs{record.opened_realtime_ns}, 3)},
                    {"closed_at_utc", utc_iso8601(RealtimeNs{record.closed_realtime_ns}, 3)}};
            }
        }
        files.push_back(entry);
    }
    // Files the writer was asked to open but could not.
    nlohmann::json failed = nlohmann::json::array();
    for (const auto& record : written) {
        if (record.open_failed) {
            failed.push_back({{"path", fs::relative(record.path, root, ec).generic_string()},
                              {"channel", record.channel}});
        }
    }
    nlohmann::json anchors = nlohmann::json::array();
    for (const auto& a : LogWriter::Instance().AnchorsFor(folder)) {
        anchors.push_back({{"label", a.label},
                           {"host_monotonic_ns", a.anchor.monotonic.value},
                           {"host_realtime_ns", a.anchor.realtime.value},
                           {"uncertainty_ns", a.anchor.uncertainty_ns},
                           {"utc", utc_iso8601(a.anchor.realtime, 6)}});
    }
    return {
        {"schema_id", "orange.recording.diagnostics_index"},
        {"schema_version", 1},
        {"generated_at_utc", utc_iso8601_now(3)},
        {"time_bases",
         {{"camera", "Emergent frame timestamp (camera clock; PTP/TAI when the recording's "
                     "timestamp_clock_contract classifies it so)"},
          {"camera_latched", "GevTimestampValue register, same clock as camera"},
          {"host_realtime", "host CLOCK_REALTIME (POSIX ns since 1970 UTC); can step"},
          {"host_monotonic", "host CLOCK_MONOTONIC (steady_clock); this boot only"},
          {"gpu_elapsed", "cudaEventElapsedTime durations"},
          {"mixed", "computed from more than one time base; see note"},
          {"utc_string", "host realtime formatted as YYYY-MM-DDTHH:MM:SSZ"}}},
        {"clock_anchors", anchors},
        {"files", files},
        {"open_failures", failed}};
}

// Writes <folder>/diagnostics_index.json (temp + rename, owned by the sudo
// invoker). Not for hot threads; call after LogWriter::FlushAll.
inline bool write_diagnostics_index(const std::string& folder, std::string* error = nullptr)
{
    if (folder.empty()) {
        return false;
    }
    LogWriter::Instance().AddAnchor(folder, "index_written");
    const nlohmann::json index = build_diagnostics_index(folder);
    orange::ScopedFsuid fsuid_guard;
    const std::filesystem::path final_path = std::filesystem::path(folder) / "diagnostics_index.json";
    const std::filesystem::path temp_path = final_path.string() + ".tmp";
    {
        std::ofstream out(temp_path, std::ios::out | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write " + temp_path.string();
            return false;
        }
        out << index.dump(2) << '\n';
        if (!out) {
            if (error) *error = "write failed: " + temp_path.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp_path, final_path, ec);
    if (ec) {
        if (error) *error = "rename failed: " + ec.message();
        return false;
    }
    return true;
}

}  // namespace orange::timing
