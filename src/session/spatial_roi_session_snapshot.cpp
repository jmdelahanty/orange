#include "session/spatial_roi_session_snapshot.h"

#include "shaman_v2_recording_identity.h"
#include "spatial_roi_finalized_session_receipt.h"
#include "spatial_roi_recorder_artifact_root.h"

#include <array>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace orange::session::spatial_roi {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::size_t kExpectedStreamCount = 4;

constexpr std::array<const char*, 12> kFinalizedReceiptArtifactKinds = {
    "video", "metadata", "keyframes", "perf", "summary", "status",
    "video_sanity", "finalization", "recorder_log", "transport_sidecar",
    "evidence", "evidence_manifest"};

constexpr std::array<const char*, 16> kFinalizedReceiptCountKeys = {
    "detach_successes", "dispatch_admitted", "dispatch_rejected",
    "ack_attempted", "ack_sent", "ack_accepted", "release_attempted",
    "release_sent", "encoded_frames", "failed_frames", "packet_count",
    "encoded_bytes", "keyframes", "ack_write_failures",
    "release_write_failures", "lifecycle_failures"};

constexpr std::array<const char*, 4> kFinalizedReceiptRangeKeys = {
    "recording_frame_id", "roi_stream_frame_index", "has_frames",
    "frame_count"};

constexpr std::array<const char*, 2> kFinalizedReceiptRangeEndpointKeys = {
    "first", "last"};

// Keep the session snapshot's profile admission self-contained.  The
// snapshot test target intentionally links only this translation unit, and a
// session snapshot must not depend on a mutable/default profile factory at
// validation time.  These are the three immutable profiles currently
// admitted by spatial-ROI schema v3; schema-v2 projects to the first one.
struct SnapshotEncodeProfileSpec final {
    const char* profile_id;
    const char* codec;
    const char* preset;
    const char* tuning;
    bool lossless;
    const char* rate_control_mode;
    std::uint64_t quality_value;
    std::uint64_t gop_length;
    bool aq;
    bool temporal_aq;
    bool lookahead;
    std::uint64_t lookahead_depth;
};

constexpr std::array<SnapshotEncodeProfileSpec, 3>
    kSnapshotEncodeProfileSpecs = {{
        {"hevc_p7_lossless_cqp0_gop1_v1", "hevc", "p7", "lossless",
         true, "cqp", 0, 1, false, false, false, 0},
        {"hevc_p1_low_latency_vbr_q20_gop1_v1", "hevc", "p1", "ll",
         false, "vbr", 20, 1, false, false, false, 0},
        {"hevc_p1_low_latency_vbr_q20_gop25_v1", "hevc", "p1", "ll",
         false, "vbr", 20, 25, false, false, false, 0},
    }};

std::uint64_t expected_keyframe_count(const std::uint64_t frame_count,
                                      const std::uint64_t gop_length)
{
    return frame_count == 0 || gop_length == 0
        ? 0
        : 1U + ((frame_count - 1U) / gop_length);
}

bool canonical_sha256(const std::string& value);
bool safe_recording_relative_path(const std::string& value);

bool fail(std::string* error_out, std::string message)
{
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
    return false;
}

bool exact_keys(const json& value,
                std::initializer_list<const char*> expected_keys)
{
    if (!value.is_object() || value.size() != expected_keys.size()) {
        return false;
    }
    for (const char* key : expected_keys) {
        if (!value.contains(key)) {
            return false;
        }
    }
    return true;
}

bool read_nonnegative_u64(const json& object,
                          const char* key,
                          std::uint64_t* value_out)
{
    if (!object.is_object() || !object.contains(key)) {
        return false;
    }
    const json& value = object.at(key);
    if (value.is_number_unsigned()) {
        if (value_out) {
            *value_out = value.get<std::uint64_t>();
        }
        return true;
    }
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
        if (value_out) {
            *value_out = value.get<std::uint64_t>();
        }
        return true;
    }
    return false;
}

bool read_string(const json& object,
                 const char* key,
                 std::string* value_out)
{
    if (!object.is_object() || !object.contains(key) ||
        !object.at(key).is_string()) {
        return false;
    }
    if (value_out) {
        *value_out = object.at(key).get<std::string>();
    }
    return true;
}

bool string_field_equals(const json& object,
                         const char* key,
                         const std::string& expected)
{
    std::string actual;
    return read_string(object, key, &actual) && actual == expected;
}

bool validate_counts_and_ranges(const json& stream,
                                const std::uint64_t expected_gop_length,
                                std::string* error_out)
{
    if (!stream.contains("counts") || !stream.at("counts").is_object() ||
        !stream.contains("ranges") || !stream.at("ranges").is_object()) {
        return fail(error_out,
                    "finalized receipt stream counts/ranges must be objects");
    }
    const json& counts = stream.at("counts");
    if (counts.size() != kFinalizedReceiptCountKeys.size()) {
        return fail(error_out,
                    "finalized receipt stream counts have unexpected keys");
    }
    for (const char* key : kFinalizedReceiptCountKeys) {
        if (!read_nonnegative_u64(counts, key, nullptr)) {
            return fail(error_out,
                        "finalized receipt stream counts contain a non-integer value");
        }
    }

    const json& ranges = stream.at("ranges");
    if (ranges.size() != kFinalizedReceiptRangeKeys.size() ||
        !ranges.contains("recording_frame_id") ||
        !ranges.contains("roi_stream_frame_index") ||
        !ranges.at("recording_frame_id").is_object() ||
        !ranges.at("roi_stream_frame_index").is_object() ||
        !ranges.contains("has_frames") ||
        !ranges.at("has_frames").is_boolean() ||
        !ranges.contains("frame_count") ||
        !read_nonnegative_u64(ranges, "frame_count", nullptr) ||
        ranges.at("recording_frame_id").size() !=
            kFinalizedReceiptRangeEndpointKeys.size() ||
        ranges.at("roi_stream_frame_index").size() !=
            kFinalizedReceiptRangeEndpointKeys.size()) {
        return fail(error_out,
                    "finalized receipt stream ranges have unexpected shape");
    }
    for (const char* range_key : {"recording_frame_id", "roi_stream_frame_index"}) {
        const json& range = ranges.at(range_key);
        for (const char* endpoint : kFinalizedReceiptRangeEndpointKeys) {
            if (!read_nonnegative_u64(range, endpoint, nullptr)) {
                return fail(error_out,
                            "finalized receipt stream range endpoints are invalid");
            }
        }
    }
    const std::uint64_t frame_count =
        ranges.at("frame_count").get<std::uint64_t>();
    const bool has_frames = ranges.at("has_frames").get<bool>();
    const auto count = [&](const char* key) {
        return counts.at(key).get<std::uint64_t>();
    };
    if (!has_frames || frame_count == 0 || expected_gop_length == 0) {
        return fail(error_out,
                    "complete finalized receipt streams must contain frames");
    }
    const std::uint64_t expected_keyframes =
        expected_keyframe_count(frame_count, expected_gop_length);
    if (count("dispatch_rejected") != 0 || count("failed_frames") != 0 ||
        count("ack_write_failures") != 0 ||
        count("release_write_failures") != 0 ||
        count("lifecycle_failures") != 0 || count("encoded_bytes") == 0 ||
        count("detach_successes") != frame_count ||
        count("dispatch_admitted") != frame_count ||
        count("ack_attempted") != frame_count ||
        count("ack_sent") != frame_count ||
        count("ack_accepted") != frame_count ||
        count("release_attempted") != frame_count ||
        count("release_sent") != frame_count ||
        count("encoded_frames") != frame_count ||
        count("packet_count") != frame_count ||
        count("keyframes") != expected_keyframes) {
        return fail(error_out,
                    "complete finalized receipt stream counts are inconsistent");
    }
    return true;
}

bool snapshot_validate_storage_preflight(const json& value)
{
    if (!exact_keys(value,
                    {"schema_id", "schema_version", "checked", "passed",
                     "status", "error", "policy", "artifact_root",
                     "filesystem", "budgets"}) ||
        !string_field_equals(
            value, "schema_id",
            kSpatialRoiRecorderStoragePreflightSchemaId) ||
        value.at("schema_version") !=
            kSpatialRoiRecorderStoragePreflightSchemaVersion ||
        !value.at("checked").is_boolean() ||
        !value.at("checked").get<bool>() ||
        !value.at("passed").is_boolean() ||
        !value.at("passed").get<bool>() ||
        !string_field_equals(value, "status", "passed") ||
        !string_field_equals(value, "error", "")) {
        return false;
    }
    const json& policy = value.at("policy");
    if (!exact_keys(policy,
                    {"schema_id", "schema_version", "required",
                     "reserved_free_bytes"}) ||
        !string_field_equals(
            policy, "schema_id",
            kSpatialRoiRecorderStoragePreflightPolicySchemaId) ||
        policy.at("schema_version") !=
            kSpatialRoiRecorderStoragePreflightPolicySchemaVersion ||
        !policy.at("required").is_boolean() ||
        !policy.at("required").get<bool>() ||
        !read_nonnegative_u64(policy, "reserved_free_bytes", nullptr) ||
        policy.at("reserved_free_bytes").get<std::uint64_t>() == 0) {
        return false;
    }
    const json& artifact_root = value.at("artifact_root");
    if (!exact_keys(artifact_root, {"device", "inode"}) ||
        !read_nonnegative_u64(artifact_root, "device", nullptr) ||
        !read_nonnegative_u64(artifact_root, "inode", nullptr)) {
        return false;
    }
    const json& filesystem = value.at("filesystem");
    if (!exact_keys(filesystem,
                    {"block_size_bytes", "total_blocks", "available_blocks",
                     "capacity_bytes", "available_bytes"}) ||
        !read_nonnegative_u64(filesystem, "block_size_bytes", nullptr) ||
        !read_nonnegative_u64(filesystem, "total_blocks", nullptr) ||
        !read_nonnegative_u64(filesystem, "available_blocks", nullptr) ||
        !read_nonnegative_u64(filesystem, "capacity_bytes", nullptr) ||
        !read_nonnegative_u64(filesystem, "available_bytes", nullptr)) {
        return false;
    }
    const std::uint64_t block_size =
        filesystem.at("block_size_bytes").get<std::uint64_t>();
    const std::uint64_t total_blocks =
        filesystem.at("total_blocks").get<std::uint64_t>();
    const std::uint64_t available_blocks =
        filesystem.at("available_blocks").get<std::uint64_t>();
    const std::uint64_t capacity =
        filesystem.at("capacity_bytes").get<std::uint64_t>();
    const std::uint64_t available =
        filesystem.at("available_bytes").get<std::uint64_t>();
    if (block_size == 0 || available_blocks > total_blocks ||
        total_blocks > std::numeric_limits<std::uint64_t>::max() / block_size ||
        available_blocks > std::numeric_limits<std::uint64_t>::max() / block_size ||
        capacity != total_blocks * block_size ||
        available != available_blocks * block_size || available > capacity) {
        return false;
    }
    const json& budgets = value.at("budgets");
    if (!exact_keys(budgets,
                    {"max_media_bytes_total", "max_evidence_bytes_total",
                     "reserved_free_bytes", "required_bytes"}) ||
        !read_nonnegative_u64(budgets, "max_media_bytes_total", nullptr) ||
        !read_nonnegative_u64(budgets, "max_evidence_bytes_total", nullptr) ||
        !read_nonnegative_u64(budgets, "reserved_free_bytes", nullptr) ||
        !read_nonnegative_u64(budgets, "required_bytes", nullptr) ||
        budgets.at("reserved_free_bytes") !=
            policy.at("reserved_free_bytes")) {
        return false;
    }
    const std::uint64_t media =
        budgets.at("max_media_bytes_total").get<std::uint64_t>();
    const std::uint64_t evidence =
        budgets.at("max_evidence_bytes_total").get<std::uint64_t>();
    const std::uint64_t reserve =
        budgets.at("reserved_free_bytes").get<std::uint64_t>();
    if (media > std::numeric_limits<std::uint64_t>::max() - evidence ||
        media + evidence > std::numeric_limits<std::uint64_t>::max() - reserve ||
        budgets.at("required_bytes") != media + evidence + reserve ||
        budgets.at("required_bytes").get<std::uint64_t>() > available) {
        return false;
    }
    return true;
}

bool snapshot_validate_complete_process_status(
    const json& value, const json* expected_artifact_root = nullptr)
{
    constexpr std::array<const char*, 11> child_keys = {
        "event", "status", "state", "ready", "clean_eof", "completed",
        "failed", "first_failure_stream_id", "first_failure", "error",
        "payload"};
    const auto validate_child = [&](const json& child) {
        if (!exact_keys(child, {child_keys.at(0), child_keys.at(1),
                                child_keys.at(2), child_keys.at(3),
                                child_keys.at(4), child_keys.at(5),
                                child_keys.at(6), child_keys.at(7),
                                child_keys.at(8), child_keys.at(9),
                                child_keys.at(10)}) ||
            !child.at("event").is_string() ||
            !child.at("status").is_string() ||
            !child.at("state").is_string() ||
            !child.at("ready").is_boolean() ||
            !child.at("clean_eof").is_boolean() ||
            !child.at("completed").is_boolean() ||
            !child.at("failed").is_boolean() ||
            !child.at("first_failure_stream_id").is_string() ||
            !child.at("first_failure").is_string() ||
            !child.at("error").is_string() ||
            !child.at("payload").is_object()) {
            return false;
        }
        // The wrapper fields above are copied from the recorder's raw
        // lifecycle payload when the supervisor observes it.  Treat a raw
        // duplicate as authenticated evidence, rather than trusting the
        // wrapper if the two disagree.  Some older lifecycle events omitted
        // optional fields, so only fields actually present in the payload are
        // coupled here.
        const json& payload = child.at("payload");
        const auto matches_if_present = [&](const char* key) {
            return !payload.contains(key) || payload.at(key) == child.at(key);
        };
        for (const char* key : {"event", "status", "state", "ready",
                                "clean_eof", "completed",
                                "first_failure_stream_id", "first_failure",
                                "error"}) {
            if (!matches_if_present(key)) return false;
        }
        if (payload.contains("failed") &&
            !matches_if_present("failed")) {
            return false;
        }
        return true;
    };
    if (!exact_keys(value,
                    {"schema_id", "schema_version", "session_state",
                     "process_state", "pid", "started", "sockets_bound",
                     "ready", "terminal_seen", "exited", "reaped",
                     "exit_code", "term_signal", "stdout_bytes_read",
                     "cleanup_complete", "first_failure", "error", "starting",
                     "ready_snapshot", "heartbeat", "terminal", "last"}) ||
        !string_field_equals(
            value, "schema_id",
            "orange.spatial_roi_recording.headless_process_status") ||
        value.at("schema_version") != 1 ||
        !string_field_equals(value, "session_state", "finished") ||
        !string_field_equals(value, "process_state", "exited") ||
        !read_nonnegative_u64(value, "pid", nullptr) ||
        value.at("pid").get<std::uint64_t>() == 0 ||
        !value.at("started").is_boolean() || !value.at("started").get<bool>() ||
        !value.at("sockets_bound").is_boolean() ||
        !value.at("sockets_bound").get<bool>() ||
        !value.at("ready").is_boolean() || !value.at("ready").get<bool>() ||
        !value.at("terminal_seen").is_boolean() ||
        !value.at("terminal_seen").get<bool>() ||
        !value.at("exited").is_boolean() || !value.at("exited").get<bool>() ||
        !value.at("reaped").is_boolean() || !value.at("reaped").get<bool>() ||
        !value.at("exit_code").is_number_integer() ||
        value.at("exit_code").get<std::int64_t>() != 0 ||
        !value.at("term_signal").is_number_integer() ||
        value.at("term_signal").get<std::int64_t>() != 0 ||
        !read_nonnegative_u64(value, "stdout_bytes_read", nullptr) ||
        value.at("stdout_bytes_read").get<std::uint64_t>() == 0 ||
        !value.at("cleanup_complete").is_boolean() ||
        !value.at("cleanup_complete").get<bool>() ||
        !string_field_equals(value, "first_failure", "") ||
        !string_field_equals(value, "error", "") ||
        !validate_child(value.at("starting")) ||
        !validate_child(value.at("ready_snapshot")) ||
        !validate_child(value.at("heartbeat")) ||
        !validate_child(value.at("terminal")) ||
        !validate_child(value.at("last"))) {
        return false;
    }
    const json& ready_snapshot = value.at("ready_snapshot");
    if (!string_field_equals(ready_snapshot, "event", "ready") ||
        !string_field_equals(ready_snapshot, "status", "ready") ||
        !string_field_equals(ready_snapshot, "state", "ready") ||
        !ready_snapshot.at("ready").get<bool>() ||
        ready_snapshot.at("clean_eof").get<bool>() ||
        ready_snapshot.at("completed").get<bool>() ||
        ready_snapshot.at("failed").get<bool>() ||
        !string_field_equals(ready_snapshot, "first_failure_stream_id", "") ||
        !string_field_equals(ready_snapshot, "first_failure", "") ||
        !string_field_equals(ready_snapshot, "error", "")) {
        return false;
    }
    const json& terminal = value.at("terminal");
    if (!string_field_equals(terminal, "event", "terminal") ||
        !string_field_equals(terminal, "status", "complete") ||
        !string_field_equals(terminal, "state", "completed") ||
        !terminal.at("ready").get<bool>() ||
        !terminal.at("clean_eof").get<bool>() ||
        !terminal.at("completed").get<bool>() ||
        terminal.at("failed").get<bool>() ||
        !string_field_equals(terminal, "first_failure_stream_id", "") ||
        !string_field_equals(terminal, "first_failure", "") ||
        !string_field_equals(terminal, "error", "")) {
        return false;
    }
    if (value.at("last") != terminal) {
        return false;
    }
    const auto preflight_from = [](const json& snapshot) -> const json* {
        if (!snapshot.contains("payload") ||
            !snapshot.at("payload").is_object() ||
            !snapshot.at("payload").contains("storage_preflight")) {
            return nullptr;
        }
        return &snapshot.at("payload").at("storage_preflight");
    };
    const json* ready_preflight =
        preflight_from(value.at("ready_snapshot"));
    const json* terminal_preflight = preflight_from(value.at("terminal"));
    if (ready_preflight == nullptr || terminal_preflight == nullptr ||
        !snapshot_validate_storage_preflight(*ready_preflight) ||
        !snapshot_validate_storage_preflight(*terminal_preflight) ||
        *ready_preflight != *terminal_preflight) {
        return false;
    }
    return expected_artifact_root == nullptr ||
           ready_preflight->at("artifact_root") == *expected_artifact_root;
}

bool snapshot_validate_complete_receipt_budgets(
    const json& receipt, const json& process_status)
{
    if (!receipt.is_object() || !receipt.contains("streams") ||
        !receipt.at("streams").is_array() ||
        !process_status.is_object() ||
        !process_status.at("ready_snapshot").is_object() ||
        !process_status.at("ready_snapshot").at("payload").is_object() ||
        !process_status.at("ready_snapshot").at("payload")
             .contains("storage_preflight")) {
        return false;
    }
    const json& budgets = process_status.at("ready_snapshot")
                              .at("payload")
                              .at("storage_preflight")
                              .at("budgets");
    if (!exact_keys(budgets,
                    {"max_media_bytes_total", "max_evidence_bytes_total",
                     "reserved_free_bytes", "required_bytes"}) ||
        !read_nonnegative_u64(budgets, "max_media_bytes_total", nullptr) ||
        !read_nonnegative_u64(budgets, "max_evidence_bytes_total", nullptr)) {
        return false;
    }
    std::uint64_t media_bytes = 0;
    std::uint64_t evidence_bytes = 0;
    for (const json& stream : receipt.at("streams")) {
        if (!stream.is_object() || !stream.contains("artifacts") ||
            !stream.at("artifacts").is_array()) {
            return false;
        }
        for (const json& artifact : stream.at("artifacts")) {
            if (!artifact.is_object() || !artifact.contains("kind") ||
                !artifact.at("kind").is_string() ||
                !read_nonnegative_u64(artifact, "size_bytes", nullptr)) {
                return false;
            }
            const std::uint64_t size =
                artifact.at("size_bytes").get<std::uint64_t>();
            std::uint64_t* total =
                artifact.at("kind").get<std::string>() == "video"
                    ? &media_bytes
                    : &evidence_bytes;
            if (*total > std::numeric_limits<std::uint64_t>::max() - size) {
                return false;
            }
            *total += size;
        }
    }
    return media_bytes <=
               budgets.at("max_media_bytes_total").get<std::uint64_t>() &&
           evidence_bytes <=
               budgets.at("max_evidence_bytes_total").get<std::uint64_t>();
}

bool snapshot_validate_complete_producer_status(
    const json& value, const json& session, const json& receipt)
{
    if (!exact_keys(value,
                    {"schema_id", "schema_version", "state", "recording_id",
                     "session_id", "recording_identity_token",
                     "producer_generation", "spatial_roi_plan_sha256", "camera_id",
                     "camera_serial", "stream_count", "submit_attempted",
                     "submitted", "incomplete", "rejected", "acquisition_armed",
                     "first_failure"}) ||
        !string_field_equals(
            value, "schema_id",
            "orange.spatial_roi_recording.headless_producer_status") ||
        value.at("schema_version") != 1 ||
        !string_field_equals(value, "state", "stopped") ||
        !string_field_equals(value, "recording_id",
                             session.at("recording_id").get<std::string>()) ||
        !string_field_equals(value, "session_id",
                             session.at("session_id").get<std::string>()) ||
        !string_field_equals(value, "recording_identity_token",
                             session.at("recording_identity_token").get<std::string>()) ||
        !string_field_equals(value, "producer_generation",
                             session.at("producer_generation").get<std::string>()) ||
        !string_field_equals(value, "spatial_roi_plan_sha256",
                             session.at("spatial_roi_plan_sha256").get<std::string>()) ||
        value.at("camera_id") != session.at("camera_id") ||
        !string_field_equals(value, "camera_serial",
                             session.at("camera_serial").get<std::string>()) ||
        value.at("stream_count") != kExpectedStreamCount ||
        !read_nonnegative_u64(value, "submit_attempted", nullptr) ||
        !read_nonnegative_u64(value, "submitted", nullptr) ||
        !read_nonnegative_u64(value, "incomplete", nullptr) ||
        !read_nonnegative_u64(value, "rejected", nullptr) ||
        !value.at("acquisition_armed").is_boolean() ||
        value.at("acquisition_armed").get<bool>() ||
        !value.at("first_failure").is_string() ||
        !value.at("first_failure").get<std::string>().empty() ||
        !receipt.is_object() || !receipt.contains("streams") ||
        !receipt.at("streams").is_array() || receipt.at("streams").empty()) {
        return false;
    }
    const json& ranges = receipt.at("streams").at(0).at("ranges");
    const std::uint64_t frame_count = ranges.at("frame_count").get<std::uint64_t>();
    return value.at("submit_attempted").get<std::uint64_t>() == frame_count &&
           value.at("submitted").get<std::uint64_t>() == frame_count &&
           value.at("incomplete").get<std::uint64_t>() == 0 &&
           value.at("rejected").get<std::uint64_t>() == 0;
}

bool validate_root_authority(const json& receipt,
                             std::string* error_out)
{
    if (!receipt.contains("root_authority") ||
        !exact_keys(receipt.at("root_authority"),
                    {"artifact_root_relative", "recording_root_identity",
                     "artifact_root_identity", "root_continuity"})) {
        return fail(error_out,
                    "finalized receipt root authority is not closed");
    }
    const json& root = receipt.at("root_authority");
    if (!string_field_equals(
            root, "artifact_root_relative",
            orange::spatial_roi::recording::kSpatialRoiRecorderArtifactDirectory) ||
        !safe_recording_relative_path(
            root.at("artifact_root_relative").get<std::string>())) {
        return fail(error_out,
                    "finalized receipt root authority identity is substituted");
    }
    for (const char* identity_key : {"recording_root_identity",
                                     "artifact_root_identity"}) {
        const json& identity = root.at(identity_key);
        if (!exact_keys(identity, {"device", "inode"}) ||
            !read_nonnegative_u64(identity, "device", nullptr) ||
            !read_nonnegative_u64(identity, "inode", nullptr)) {
            return fail(error_out,
                        "finalized receipt root identity is invalid");
        }
    }
    const json& continuity = root.at("root_continuity");
    if (!exact_keys(continuity, {"proven", "not_proven"}) ||
        !continuity.at("proven").is_array() ||
        !continuity.at("not_proven").is_array()) {
        return fail(error_out,
                    "finalized receipt root continuity is invalid");
    }
    for (const char* key : {"proven", "not_proven"}) {
        for (const json& statement : continuity.at(key)) {
            if (!statement.is_string() || statement.get<std::string>().empty()) {
                return fail(error_out,
                            "finalized receipt root continuity contains an invalid statement");
            }
        }
    }
    return true;
}

bool validate_finalized_receipt_artifacts(
    const json& stream_receipt,
    const SpatialRoiRecorderStreamView& expected_stream,
    std::set<std::string>* all_paths,
    std::string* error_out)
{
    if (all_paths == nullptr || !stream_receipt.contains("artifacts") ||
        !stream_receipt.at("artifacts").is_array() ||
        stream_receipt.at("artifacts").size() !=
            kFinalizedReceiptArtifactKinds.size() ||
        expected_stream.artifacts.size() !=
            kFinalizedReceiptArtifactKinds.size()) {
        return fail(error_out,
                    "finalized receipt stream must contain exactly twelve artifacts");
    }
    std::set<std::string> stream_paths;
    std::set<std::string> stream_kinds;
    const json& artifacts = stream_receipt.at("artifacts");
    for (std::size_t index = 0; index < kFinalizedReceiptArtifactKinds.size();
         ++index) {
        const json& artifact = artifacts.at(index);
        if (!exact_keys(artifact, {"kind", "relative_path", "size_bytes", "sha256"}) ||
            !string_field_equals(artifact, "kind",
                                 kFinalizedReceiptArtifactKinds[index])) {
            return fail(error_out,
                        "finalized receipt artifact kind/order is substituted");
        }
        const std::string kind = artifact.at("kind").get<std::string>();
        const auto expected = expected_stream.artifacts.find(kind);
        if (expected == expected_stream.artifacts.end() ||
            !string_field_equals(artifact, "relative_path",
                                 expected->second.relative_path)) {
            return fail(error_out,
                        "finalized receipt artifact path is substituted");
        }
        const std::string path = artifact.at("relative_path").get<std::string>();
        if (!safe_recording_relative_path(path) ||
            !stream_paths.insert(path).second || !all_paths->insert(path).second ||
            !read_nonnegative_u64(artifact, "size_bytes", nullptr) ||
            artifact.at("size_bytes").get<std::uint64_t>() == 0 ||
            !artifact.at("sha256").is_string() ||
            !canonical_sha256(artifact.at("sha256").get<std::string>())) {
            return fail(error_out,
                        "finalized receipt artifact path/size/hash is invalid or duplicated");
        }
        stream_kinds.insert(kind);
    }
    if (stream_kinds.size() != kFinalizedReceiptArtifactKinds.size()) {
        return fail(error_out,
                    "finalized receipt stream artifact kinds are not unique");
    }
    return true;
}

bool validate_finalized_session_receipt(
    const SpatialRoiRecorderCameraContractView& camera_contract,
    const json& receipt,
    std::string* error_out)
{
    using orange::spatial_roi::recording::
        kSpatialRoiFinalizedSessionReceiptSchemaId;
    using orange::spatial_roi::recording::
        kSpatialRoiFinalizedSessionReceiptSchemaVersion;

    if (!exact_keys(receipt,
                    {"schema_id", "schema_version", "canonicalization",
                     "stream_kind", "status", "stream_count", "stream_order",
                     "identity", "root_authority", "streams"}) ||
        !string_field_equals(receipt, "schema_id",
                             kSpatialRoiFinalizedSessionReceiptSchemaId) ||
        receipt.at("schema_version") !=
            kSpatialRoiFinalizedSessionReceiptSchemaVersion ||
        !string_field_equals(
            receipt, "canonicalization",
            "canonical_json_utf8_sort_keys_compact_v1") ||
        !string_field_equals(receipt, "stream_kind", "fixed_region") ||
        !string_field_equals(receipt, "status", "complete") ||
        !read_nonnegative_u64(receipt, "stream_count", nullptr) ||
        receipt.at("stream_count").get<std::uint64_t>() !=
            kExpectedStreamCount) {
        return fail(error_out,
                    "finalized session receipt schema/status is invalid");
    }
    const json& stream_order = receipt.at("stream_order");
    if (!stream_order.is_array() ||
        stream_order.size() != kExpectedStreamCount ||
        stream_order != camera_contract.stream_order) {
        return fail(error_out,
                    "finalized session receipt stream order is substituted");
    }
    for (const json& stream_id : stream_order) {
        if (!stream_id.is_string() || stream_id.get<std::string>().empty()) {
            return fail(error_out,
                        "finalized session receipt stream order is invalid");
        }
    }

    const json& identity = receipt.at("identity");
    if (!exact_keys(identity,
                    {"recording_id", "session_id", "recording_identity_token",
                     "producer_generation", "spatial_roi_plan_sha256", "camera_id",
                     "camera_serial", "stream_count", "stream_order"}) ||
        !string_field_equals(identity, "recording_id", camera_contract.recording_id) ||
        !string_field_equals(identity, "session_id", camera_contract.session_id) ||
        !string_field_equals(identity, "recording_identity_token",
                             camera_contract.recording_identity_token) ||
        !string_field_equals(identity, "producer_generation",
                             camera_contract.producer_generation) ||
        !string_field_equals(identity, "spatial_roi_plan_sha256",
                             camera_contract.spatial_roi_plan_sha256) ||
        identity.at("camera_id") != camera_contract.camera_id ||
        !string_field_equals(identity, "camera_serial", camera_contract.camera_serial) ||
        !read_nonnegative_u64(identity, "stream_count", nullptr) ||
        identity.at("stream_count").get<std::uint64_t>() != kExpectedStreamCount ||
        identity.at("stream_order") != camera_contract.stream_order) {
        return fail(error_out,
                    "finalized session receipt recording/camera identity is substituted");
    }
    if (!validate_root_authority(receipt, error_out)) {
        return false;
    }

    const json& streams = receipt.at("streams");
    if (!streams.is_array() || streams.size() != kExpectedStreamCount) {
        return fail(error_out,
                    "finalized session receipt must contain four streams");
    }
    std::set<std::string> stream_ids;
    std::set<std::string> artifact_paths;
    for (std::size_t index = 0; index < kExpectedStreamCount; ++index) {
        const json& stream_receipt = streams.at(index);
        const SpatialRoiRecorderStreamView& expected_stream =
            camera_contract.streams.at(index);
        if (!exact_keys(stream_receipt,
                        {"logical_stream_id", "identity", "counts", "ranges",
                         "finalized_receipt_digest", "artifacts"}) ||
            !string_field_equals(stream_receipt, "logical_stream_id",
                                 expected_stream.logical_stream_id) ||
            !stream_ids.insert(expected_stream.logical_stream_id).second ||
            !stream_receipt.at("finalized_receipt_digest").is_string() ||
            !canonical_sha256(stream_receipt.at("finalized_receipt_digest")
                                  .get<std::string>())) {
            return fail(error_out,
                        "finalized session receipt stream identity/digest is invalid");
        }
        const json& stream_identity = stream_receipt.at("identity");
        const std::string expected_arena_group = expected_stream.arena_group_id;
        if (!exact_keys(stream_identity,
                        {"recording_id", "session_id", "recording_identity_token",
                         "producer_generation", "spatial_roi_plan_sha256", "camera_id",
                         "camera_serial", "roi_id", "region_id", "arena_group_id",
                         "logical_stream_id", "assigned_gpu_id", "assigned_shard_id"}) ||
            !string_field_equals(stream_identity, "recording_id",
                                 camera_contract.recording_id) ||
            !string_field_equals(stream_identity, "session_id",
                                 camera_contract.session_id) ||
            !string_field_equals(stream_identity, "recording_identity_token",
                                 camera_contract.recording_identity_token) ||
            !string_field_equals(stream_identity, "producer_generation",
                                 camera_contract.producer_generation) ||
            !string_field_equals(stream_identity, "spatial_roi_plan_sha256",
                                 camera_contract.spatial_roi_plan_sha256) ||
            stream_identity.at("camera_id") != camera_contract.camera_id ||
            !string_field_equals(stream_identity, "camera_serial",
                                 camera_contract.camera_serial) ||
            !string_field_equals(stream_identity, "roi_id", expected_stream.roi_id) ||
            !string_field_equals(stream_identity, "region_id",
                                 expected_stream.region_id) ||
            !string_field_equals(stream_identity, "arena_group_id",
                                 expected_arena_group) ||
            !string_field_equals(stream_identity, "logical_stream_id",
                                 expected_stream.logical_stream_id) ||
            stream_identity.at("assigned_gpu_id") != expected_stream.assigned_gpu_id ||
            stream_identity.at("assigned_shard_id") != 0) {
            return fail(error_out,
                        "finalized session receipt stream identity is substituted");
        }
        if (!validate_counts_and_ranges(
                stream_receipt, expected_stream.encode_profile.gop_length,
                error_out) ||
            !validate_finalized_receipt_artifacts(
                stream_receipt, expected_stream, &artifact_paths, error_out)) {
            return false;
        }
    }
    const json& reference_ranges = streams.at(0).at("ranges");
    for (std::size_t index = 1; index < kExpectedStreamCount; ++index) {
        const json& ranges = streams.at(index).at("ranges");
        if (ranges.at("frame_count") != reference_ranges.at("frame_count") ||
            ranges.at("recording_frame_id") !=
                reference_ranges.at("recording_frame_id")) {
            return fail(error_out,
                        "finalized receipt lanes do not share a frame range");
        }
    }
    if (stream_ids.size() != kExpectedStreamCount ||
        artifact_paths.size() != kExpectedStreamCount *
                                      kFinalizedReceiptArtifactKinds.size()) {
        return fail(error_out,
                    "finalized session receipt streams or artifacts are duplicated");
    }
    return true;
}

bool canonical_sha256(const std::string& value)
{
    if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0) {
        return false;
    }
    for (std::size_t index = 7; index < value.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool safe_identifier(const std::string& value)
{
    if (value.empty() || value.size() > 512) {
        return false;
    }
    for (const unsigned char byte : value) {
        if (byte < 0x20 || byte == 0x7f || byte == '/' || byte == '\\') {
            return false;
        }
    }
    return true;
}

bool safe_recording_relative_path(const std::string& value)
{
    if (value.empty() || value.find('\0') != std::string::npos ||
        value.front() == '/' || value.front() == '\\' ||
        value.find(':') != std::string::npos) {
        return false;
    }
    const fs::path path(value);
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory() || path == fs::path(".")) {
        return false;
    }
    for (const fs::path& component : path) {
        if (component == fs::path(".") || component == fs::path("..")) {
            return false;
        }
    }
    return path.lexically_normal().generic_string() == value;
}

bool same_authority(const SpatialRoiRecorderAuthorityView& lhs,
                    const SpatialRoiRecorderAuthorityView& rhs)
{
    return lhs.id == rhs.id && lhs.sha256 == rhs.sha256;
}

bool valid_authority(const SpatialRoiRecorderAuthorityView& authority)
{
    return safe_identifier(authority.id) && canonical_sha256(authority.sha256);
}

bool same_raster(const orange::spatial_roi::SpatialRoiFrameRaster& lhs,
                const orange::spatial_roi::SpatialRoiFrameRaster& rhs)
{
    return lhs.width == rhs.width && lhs.height == rhs.height;
}

bool rect_fits(const orange::spatial_roi::SpatialRoiFrameRect& rect,
               const orange::spatial_roi::SpatialRoiFrameRaster& raster)
{
    return rect.width > 0 && rect.height > 0 && rect.x <= raster.width &&
           rect.y <= raster.height && rect.width <= raster.width - rect.x &&
           rect.height <= raster.height - rect.y;
}

bool validate_artifact_reference(
    const SpatialRoiSessionArtifactReference& reference,
    std::set<std::string>* relative_paths,
    const char* name,
    std::string* error_out)
{
    if (relative_paths == nullptr || !safe_recording_relative_path(
                                        reference.relative_path)) {
        return fail(error_out,
                    std::string("spatial ROI session ") + name +
                        " artifact path must be recording-root relative");
    }
    if (!canonical_sha256(reference.sha256)) {
        return fail(error_out,
                    std::string("spatial ROI session ") + name +
                        " artifact sha256 is not canonical");
    }
    if (reference.size_bytes == 0) {
        return fail(error_out,
                    std::string("spatial ROI session ") + name +
                        " artifact size must be positive");
    }
    if (!relative_paths->insert(reference.relative_path).second) {
        return fail(error_out,
                    "spatial ROI session authority artifacts must have unique paths");
    }
    return true;
}

bool validate_optional_status(const nlohmann::json* status,
                              const char* name,
                              std::string* error_out)
{
    if (status != nullptr && !status->is_object()) {
        return fail(error_out,
                    std::string("spatial ROI session ") + name +
                        " status must be a JSON object");
    }
    return true;
}

json raster_json(const orange::spatial_roi::SpatialRoiFrameRaster& raster)
{
    return {{"width", raster.width}, {"height", raster.height}};
}

json rect_json(const orange::spatial_roi::SpatialRoiFrameRect& rect)
{
    return {{"x", rect.x},
            {"y", rect.y},
            {"width", rect.width},
            {"height", rect.height}};
}

json padding_json(const orange::spatial_roi::SpatialRoiFramePadding& padding)
{
    return { {"left", padding.left},
             {"top", padding.top},
             {"right", padding.right},
             {"bottom", padding.bottom},
             {"value_mono8", padding.value_mono8} };
}

json authority_json(const SpatialRoiRecorderAuthorityView& authority)
{
    return {{"id", authority.id}, {"sha256", authority.sha256}};
}

json geometry_json(const SpatialRoiRecorderGeometryView& geometry)
{
    return {
        {"layout", authority_json(geometry.layout)},
        {"materialization", authority_json(geometry.materialization)},
        {"registration", authority_json(geometry.registration)},
        {"native_raster", raster_json(geometry.native_raster)},
        {"content_rect", rect_json(geometry.content_rect)},
        {"encoded_raster", raster_json(geometry.encoded_raster)},
        {"encoded_content_rect", rect_json(geometry.encoded_content_rect)},
        {"content_offset", {{"x", geometry.content_offset_x},
                             {"y", geometry.content_offset_y}}},
        {"padding", padding_json(geometry.padding)},
        {"source_coordinate_space", geometry.source_coordinate_space},
        {"video_coordinate_space", geometry.video_coordinate_space},
    };
}

json encode_profile_json(const SpatialRoiRecorderEncodeProfileView& profile)
{
    return {
        {"profile_id", profile.profile_id},
        {"codec", profile.codec},
        {"preset", profile.preset},
        {"tuning", profile.tuning},
        {"lossless", profile.lossless},
        {"rate_control_mode", profile.rate_control_mode},
        {"quality_value", profile.quality_value},
        {"gop_length", profile.gop_length},
        {"aq", profile.aq},
        {"temporal_aq", profile.temporal_aq},
        {"lookahead", profile.lookahead},
        {"lookahead_depth", profile.lookahead_depth},
        {"frame_rate", profile.frame_rate},
        {"input_format", profile.input_format},
        {"encoded_format", profile.encoded_format},
        {"no_resize", profile.no_resize},
        {"luma_preserved_exactly", profile.luma_preserved_exactly},
        {"neutral_chroma_value", profile.neutral_chroma_value},
    };
}

json artifact_reference_json(
    const SpatialRoiSessionArtifactReference& reference)
{
    return {{"relative_path", reference.relative_path},
            {"size_bytes", reference.size_bytes},
            {"sha256", reference.sha256}};
}

bool snapshot_nonempty_string(const json& object, const char* key)
{
    std::string value;
    return read_string(object, key, &value) && !value.empty();
}

bool snapshot_nonnegative_value(const json& value)
{
    return (value.is_number_unsigned()) ||
           (value.is_number_integer() && value.get<std::int64_t>() >= 0);
}

bool snapshot_validate_raster(const json& value)
{
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    return exact_keys(value, {"width", "height"}) &&
           read_nonnegative_u64(value, "width", &width) &&
           read_nonnegative_u64(value, "height", &height) && width > 0 &&
           height > 0;
}

bool snapshot_validate_rect(const json& value)
{
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    return exact_keys(value, {"x", "y", "width", "height"}) &&
           read_nonnegative_u64(value, "x", nullptr) &&
           read_nonnegative_u64(value, "y", nullptr) &&
           read_nonnegative_u64(value, "width", &width) &&
           read_nonnegative_u64(value, "height", &height) && width > 0 &&
           height > 0;
}

bool snapshot_rect_fits(const json& rect, const json& raster)
{
    std::uint64_t x = 0;
    std::uint64_t y = 0;
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint64_t raster_width = 0;
    std::uint64_t raster_height = 0;
    return snapshot_validate_rect(rect) && snapshot_validate_raster(raster) &&
           read_nonnegative_u64(rect, "x", &x) &&
           read_nonnegative_u64(rect, "y", &y) &&
           read_nonnegative_u64(rect, "width", &width) &&
           read_nonnegative_u64(rect, "height", &height) &&
           read_nonnegative_u64(raster, "width", &raster_width) &&
           read_nonnegative_u64(raster, "height", &raster_height) &&
           x <= raster_width && y <= raster_height &&
           width <= raster_width - x && height <= raster_height - y;
}

bool snapshot_validate_authority(const json& value)
{
    std::string id;
    std::string sha256;
    return exact_keys(value, {"id", "sha256"}) &&
           read_string(value, "id", &id) && !id.empty() &&
           read_string(value, "sha256", &sha256) && canonical_sha256(sha256);
}

bool snapshot_validate_artifact_reference(
    const json& value, std::set<std::string>* paths)
{
    std::string path;
    std::string sha256;
    return paths != nullptr && exact_keys(value, {"relative_path", "size_bytes", "sha256"}) &&
           read_string(value, "relative_path", &path) &&
           safe_recording_relative_path(path) &&
           read_nonnegative_u64(value, "size_bytes", nullptr) &&
           value.at("size_bytes").get<std::uint64_t>() > 0 &&
           read_string(value, "sha256", &sha256) && canonical_sha256(sha256) &&
           paths->insert(path).second;
}

bool snapshot_validate_profile(const json& value)
{
    if (!exact_keys(value,
                    {"profile_id", "codec", "preset", "tuning", "lossless",
                     "rate_control_mode", "quality_value", "gop_length",
                     "frame_rate", "input_format", "encoded_format",
                     "no_resize", "luma_preserved_exactly",
                     "neutral_chroma_value", "aq", "temporal_aq",
                     "lookahead", "lookahead_depth"})) {
        return false;
    }
    for (const char* key : {"profile_id", "codec", "preset", "tuning",
                            "rate_control_mode", "input_format",
                            "encoded_format"}) {
        if (!snapshot_nonempty_string(value, key)) {
            return false;
        }
    }
    for (const char* key : {"lossless", "no_resize", "luma_preserved_exactly",
                            "aq", "temporal_aq", "lookahead"}) {
        if (!value.at(key).is_boolean()) {
            return false;
        }
    }
    if (!read_nonnegative_u64(value, "quality_value", nullptr) ||
        !read_nonnegative_u64(value, "gop_length", nullptr) ||
        !read_nonnegative_u64(value, "frame_rate", nullptr) ||
        !read_nonnegative_u64(value, "neutral_chroma_value", nullptr) ||
        !read_nonnegative_u64(value, "lookahead_depth", nullptr) ||
        value.at("frame_rate") == 0 || value.at("neutral_chroma_value") != 128 ||
        value.at("codec") != "hevc" || value.at("input_format") != "mono8" ||
        value.at("encoded_format") != "nv12" ||
        !value.at("no_resize").get<bool>()) {
        return false;
    }
    for (const SnapshotEncodeProfileSpec& expected :
         kSnapshotEncodeProfileSpecs) {
        if (value.at("profile_id") == expected.profile_id &&
            value.at("codec") == expected.codec &&
            value.at("preset") == expected.preset &&
            value.at("tuning") == expected.tuning &&
            value.at("lossless") == expected.lossless &&
            value.at("rate_control_mode") == expected.rate_control_mode &&
            value.at("quality_value") == expected.quality_value &&
            value.at("gop_length") == expected.gop_length &&
            value.at("aq") == expected.aq &&
            value.at("temporal_aq") == expected.temporal_aq &&
            value.at("lookahead") == expected.lookahead &&
            value.at("lookahead_depth") == expected.lookahead_depth &&
            value.at("luma_preserved_exactly") == expected.lossless) {
            return true;
        }
    }
    return false;
}

bool snapshot_validate_geometry(const json& value)
{
    return exact_keys(value,
                      {"layout", "materialization", "registration", "native_raster",
                       "content_rect", "encoded_raster", "encoded_content_rect",
                       "content_offset", "padding", "source_coordinate_space",
                       "video_coordinate_space"}) &&
           snapshot_validate_authority(value.at("layout")) &&
           snapshot_validate_authority(value.at("materialization")) &&
           snapshot_validate_authority(value.at("registration")) &&
           snapshot_validate_raster(value.at("native_raster")) &&
           snapshot_validate_rect(value.at("content_rect")) &&
           snapshot_validate_raster(value.at("encoded_raster")) &&
           snapshot_validate_rect(value.at("encoded_content_rect")) &&
           exact_keys(value.at("content_offset"), {"x", "y"}) &&
           read_nonnegative_u64(value.at("content_offset"), "x", nullptr) &&
           read_nonnegative_u64(value.at("content_offset"), "y", nullptr) &&
           exact_keys(value.at("padding"),
                      {"left", "top", "right", "bottom", "value_mono8"}) &&
           read_nonnegative_u64(value.at("padding"), "left", nullptr) &&
           read_nonnegative_u64(value.at("padding"), "top", nullptr) &&
           read_nonnegative_u64(value.at("padding"), "right", nullptr) &&
           read_nonnegative_u64(value.at("padding"), "bottom", nullptr) &&
           read_nonnegative_u64(value.at("padding"), "value_mono8", nullptr) &&
           snapshot_nonempty_string(value, "source_coordinate_space") &&
           snapshot_nonempty_string(value, "video_coordinate_space");
}

bool snapshot_validate_raw_receipt(const json& receipt,
                                   const json& session,
                                   const std::vector<std::string>& stream_order,
                                   std::string* error_out)
{
    if (!exact_keys(receipt,
                    {"schema_id", "schema_version", "canonicalization",
                     "stream_kind", "status", "stream_count", "stream_order",
                     "identity", "root_authority", "streams"}) ||
        !string_field_equals(
            receipt, "schema_id",
            orange::spatial_roi::recording::kSpatialRoiFinalizedSessionReceiptSchemaId) ||
        receipt.at("schema_version") !=
            orange::spatial_roi::recording::kSpatialRoiFinalizedSessionReceiptSchemaVersion ||
        !string_field_equals(receipt, "canonicalization",
                             "canonical_json_utf8_sort_keys_compact_v1") ||
        !string_field_equals(receipt, "stream_kind", "fixed_region") ||
        !string_field_equals(receipt, "status", "complete") ||
        !read_nonnegative_u64(receipt, "stream_count", nullptr) ||
        receipt.at("stream_count").get<std::uint64_t>() != kExpectedStreamCount) {
        return fail(error_out, "spatial ROI finalized receipt schema/status is invalid");
    }
    const json& order = receipt.at("stream_order");
    if (!order.is_array() || order.size() != kExpectedStreamCount ||
        order != stream_order) {
        return fail(error_out, "spatial ROI finalized receipt stream order is substituted");
    }
    for (const json& stream_id : order) {
        if (!stream_id.is_string() || stream_id.get<std::string>().empty()) {
            return fail(error_out, "spatial ROI finalized receipt stream order is invalid");
        }
    }

    const json& identity = receipt.at("identity");
    if (!exact_keys(identity,
                    {"recording_id", "session_id", "recording_identity_token",
                     "producer_generation", "spatial_roi_plan_sha256", "camera_id",
                     "camera_serial", "stream_count", "stream_order"}) ||
        !string_field_equals(identity, "recording_id",
                             session.at("recording_id").get<std::string>()) ||
        !string_field_equals(identity, "session_id",
                             session.at("session_id").get<std::string>()) ||
        !string_field_equals(identity, "recording_identity_token",
                             session.at("recording_identity_token").get<std::string>()) ||
        !string_field_equals(identity, "producer_generation",
                             session.at("producer_generation").get<std::string>()) ||
        !string_field_equals(identity, "spatial_roi_plan_sha256",
                             session.at("spatial_roi_plan_sha256").get<std::string>()) ||
        identity.at("camera_id") != session.at("camera_id") ||
        !string_field_equals(identity, "camera_serial",
                             session.at("camera_serial").get<std::string>()) ||
        !read_nonnegative_u64(identity, "stream_count", nullptr) ||
        identity.at("stream_count").get<std::uint64_t>() != kExpectedStreamCount ||
        identity.at("stream_order") != stream_order ||
        !validate_root_authority(receipt, error_out)) {
        return fail(error_out, "spatial ROI finalized receipt identity is substituted");
    }

    const json& streams = receipt.at("streams");
    if (!streams.is_array() || streams.size() != kExpectedStreamCount) {
        return fail(error_out, "spatial ROI finalized receipt must contain four streams");
    }
    std::set<std::string> stream_ids;
    std::set<std::string> artifact_paths;
    for (std::size_t index = 0; index < kExpectedStreamCount; ++index) {
        const json& stream = streams.at(index);
        const json& roi = session.at("rois").at(index);
        if (!exact_keys(stream,
                        {"logical_stream_id", "identity", "counts", "ranges",
                         "finalized_receipt_digest", "artifacts"}) ||
            !string_field_equals(stream, "logical_stream_id", stream_order.at(index)) ||
            !stream_ids.insert(stream_order.at(index)).second ||
            !stream.at("finalized_receipt_digest").is_string() ||
            !canonical_sha256(stream.at("finalized_receipt_digest").get<std::string>())) {
            return fail(error_out, "spatial ROI finalized receipt stream identity is invalid");
        }
        const json& stream_identity = stream.at("identity");
        if (!exact_keys(stream_identity,
                        {"recording_id", "session_id", "recording_identity_token",
                         "producer_generation", "spatial_roi_plan_sha256", "camera_id",
                         "camera_serial", "roi_id", "region_id", "arena_group_id",
                         "logical_stream_id", "assigned_gpu_id", "assigned_shard_id"}) ||
            !string_field_equals(stream_identity, "recording_id",
                                 session.at("recording_id").get<std::string>()) ||
            !string_field_equals(stream_identity, "session_id",
                                 session.at("session_id").get<std::string>()) ||
            !string_field_equals(stream_identity, "recording_identity_token",
                                 session.at("recording_identity_token").get<std::string>()) ||
            !string_field_equals(stream_identity, "producer_generation",
                                 session.at("producer_generation").get<std::string>()) ||
            !string_field_equals(stream_identity, "spatial_roi_plan_sha256",
                                 session.at("spatial_roi_plan_sha256").get<std::string>()) ||
            stream_identity.at("camera_id") != session.at("camera_id") ||
            !string_field_equals(stream_identity, "camera_serial",
                                 session.at("camera_serial").get<std::string>()) ||
            !string_field_equals(stream_identity, "roi_id", roi.at("roi_id").get<std::string>()) ||
            !string_field_equals(stream_identity, "region_id", roi.at("region_id").get<std::string>()) ||
            !string_field_equals(stream_identity, "arena_group_id",
                                 roi.at("arena_group_id").get<std::string>()) ||
            !string_field_equals(stream_identity, "logical_stream_id", stream_order.at(index)) ||
            stream_identity.at("assigned_gpu_id") != roi.at("assigned_gpu_id") ||
            !read_nonnegative_u64(stream_identity, "assigned_shard_id", nullptr) ||
            stream_identity.at("assigned_shard_id") != 0) {
            return fail(error_out, "spatial ROI finalized receipt stream identity is substituted");
        }
        const std::uint64_t expected_gop_length =
            roi.at("encode_profile").at("gop_length").get<std::uint64_t>();
        if (!validate_counts_and_ranges(stream, expected_gop_length,
                                        error_out)) {
            return false;
        }
        const json& ranges = stream.at("ranges");
        const std::uint64_t frame_count = ranges.at("frame_count").get<std::uint64_t>();
        const std::uint64_t recording_first =
            ranges.at("recording_frame_id").at("first").get<std::uint64_t>();
        const std::uint64_t recording_last =
            ranges.at("recording_frame_id").at("last").get<std::uint64_t>();
        const std::uint64_t roi_first =
            ranges.at("roi_stream_frame_index").at("first").get<std::uint64_t>();
        const std::uint64_t roi_last =
            ranges.at("roi_stream_frame_index").at("last").get<std::uint64_t>();
        const bool has_frames = ranges.at("has_frames").get<bool>();
        if (has_frames &&
            (frame_count == 0 || recording_first == 0 || recording_last < recording_first ||
             recording_last - recording_first != frame_count - 1 || roi_first != 1 ||
             roi_last != frame_count)) {
            return fail(error_out, "spatial ROI finalized receipt stream ranges are not dense");
        }
        if (!has_frames &&
            (frame_count != 0 || recording_first != 0 || recording_last != 0 ||
             roi_first != 0 || roi_last != 0)) {
            return fail(error_out, "spatial ROI finalized receipt stream ranges are invalid");
        }

        const json& artifacts = stream.at("artifacts");
        if (!artifacts.is_array() || artifacts.size() != kFinalizedReceiptArtifactKinds.size()) {
            return fail(error_out, "spatial ROI finalized receipt must contain twelve artifacts per stream");
        }
        std::set<std::string> stream_kinds;
        std::set<std::string> stream_paths;
        for (std::size_t artifact_index = 0;
             artifact_index < kFinalizedReceiptArtifactKinds.size(); ++artifact_index) {
            const json& artifact = artifacts.at(artifact_index);
            std::string kind;
            std::string path;
            std::string sha256;
            if (!exact_keys(artifact, {"kind", "relative_path", "size_bytes", "sha256"}) ||
                !read_string(artifact, "kind", &kind) ||
                kind != kFinalizedReceiptArtifactKinds.at(artifact_index) ||
                !read_string(artifact, "relative_path", &path) ||
                !safe_recording_relative_path(path) ||
                !stream_kinds.insert(kind).second ||
                !stream_paths.insert(path).second ||
                !artifact_paths.insert(path).second ||
                !read_nonnegative_u64(artifact, "size_bytes", nullptr) ||
                artifact.at("size_bytes").get<std::uint64_t>() == 0 ||
                !read_string(artifact, "sha256", &sha256) ||
                !canonical_sha256(sha256)) {
                return fail(error_out, "spatial ROI finalized receipt artifact is invalid or duplicated");
            }
        }
    }
    if (stream_ids.size() != kExpectedStreamCount ||
        artifact_paths.size() != kExpectedStreamCount * kFinalizedReceiptArtifactKinds.size()) {
        return fail(error_out, "spatial ROI finalized receipt streams or artifacts are duplicated");
    }
    return true;
}

bool snapshot_validate_closed_object(const json& session,
                                     SpatialRoiSessionSnapshotValidation* result,
                                     std::string* error_out)
{
    if (!exact_keys(session,
                    {"schema_id", "schema_version", "status", "recording_id",
                     "session_id", "recording_identity_token", "producer_generation",
                     "spatial_roi_plan_sha256", "product_kind", "stream_count",
                     "stream_order", "identity", "camera", "camera_id",
                     "camera_serial", "native_raster", "authorities", "gpu_mapping",
                     "artifacts", "rois", "finalized_session_receipt",
                     "recorder_process_status", "producer_status"}) ||
        !string_field_equals(session, "schema_id", kSpatialRoiSessionSnapshotSchemaId) ||
        session.at("schema_version") != kSpatialRoiSessionSnapshotSchemaVersion ||
        !snapshot_nonempty_string(session, "status") ||
        (session.at("status") != "pending" && session.at("status") != "complete" &&
         session.at("status") != "failed") ||
        !snapshot_nonempty_string(session, "recording_id") ||
        !string_field_equals(session, "session_id",
                             session.at("recording_id").get<std::string>()) ||
        !session.at("recording_identity_token").is_string() ||
        !canonical_sha256(session.at("recording_identity_token").get<std::string>()) ||
        !snapshot_nonempty_string(session, "producer_generation") ||
        !session.at("spatial_roi_plan_sha256").is_string() ||
        !canonical_sha256(session.at("spatial_roi_plan_sha256").get<std::string>()) ||
        !string_field_equals(session, "product_kind", kSpatialRoiRecorderCameraProductKind) ||
        !read_nonnegative_u64(session, "stream_count", nullptr) ||
        session.at("stream_count").get<std::uint64_t>() != kExpectedStreamCount ||
        !session.at("stream_order").is_array() ||
        session.at("stream_order").size() != kExpectedStreamCount) {
        return fail(error_out, "spatial ROI session snapshot schema or identity is invalid");
    }

    std::set<std::string> stream_ids;
    result->stream_order.clear();
    for (const json& stream_id : session.at("stream_order")) {
        if (!stream_id.is_string() || stream_id.get<std::string>().empty() ||
            !stream_ids.insert(stream_id.get<std::string>()).second) {
            return fail(error_out, "spatial ROI session snapshot stream order is invalid");
        }
        result->stream_order.push_back(stream_id.get<std::string>());
    }
    const std::string recording_id = session.at("recording_id").get<std::string>();
    if (session.at("recording_identity_token").get<std::string>() !=
        orange::shaman_v2_recording_identity::token_for_recording_id(
            recording_id)) {
        return fail(error_out,
                    "spatial ROI session recording identity token is not derived from recording_id");
    }
    if (!exact_keys(session.at("identity"),
                    {"recording_id", "session_id", "recording_identity_token",
                     "producer_generation", "spatial_roi_plan_sha256"}) ||
        !string_field_equals(session.at("identity"), "recording_id", recording_id) ||
        !string_field_equals(session.at("identity"), "session_id", recording_id) ||
        !string_field_equals(session.at("identity"), "recording_identity_token",
                             session.at("recording_identity_token").get<std::string>()) ||
        !string_field_equals(session.at("identity"), "producer_generation",
                             session.at("producer_generation").get<std::string>()) ||
        !string_field_equals(session.at("identity"), "spatial_roi_plan_sha256",
                             session.at("spatial_roi_plan_sha256").get<std::string>())) {
        return fail(error_out, "spatial ROI session snapshot identity is substituted");
    }

    const json& camera = session.at("camera");
    if (!exact_keys(camera, {"camera_id", "camera_serial", "native_raster"}) ||
        !read_nonnegative_u64(camera, "camera_id", nullptr) ||
        !snapshot_nonempty_string(camera, "camera_serial") ||
        !snapshot_validate_raster(camera.at("native_raster")) ||
        !read_nonnegative_u64(session, "camera_id", nullptr) ||
        !snapshot_nonempty_string(session, "camera_serial") ||
        !snapshot_validate_raster(session.at("native_raster")) ||
        camera.at("camera_id") != session.at("camera_id") ||
        camera.at("camera_serial") != session.at("camera_serial") ||
        camera.at("native_raster") != session.at("native_raster")) {
        return fail(error_out, "spatial ROI session snapshot camera identity is invalid");
    }
    if (!exact_keys(session.at("authorities"), {"layout", "materialization", "registration"}) ||
        !snapshot_validate_authority(session.at("authorities").at("layout")) ||
        !snapshot_validate_authority(session.at("authorities").at("materialization")) ||
        !snapshot_validate_authority(session.at("authorities").at("registration"))) {
        return fail(error_out, "spatial ROI session snapshot authorities are invalid");
    }

    const json& gpu_mapping = session.at("gpu_mapping");
    const std::string camera_serial = session.at("camera_serial").get<std::string>();
    if (!exact_keys(gpu_mapping,
                    {"analytics_gpu_by_camera_serial", "recorder_gpu_by_logical_stream_id"}) ||
        !gpu_mapping.at("analytics_gpu_by_camera_serial").is_object() ||
        gpu_mapping.at("analytics_gpu_by_camera_serial").size() != 1U ||
        !gpu_mapping.at("analytics_gpu_by_camera_serial").contains(camera_serial) ||
        !read_nonnegative_u64(gpu_mapping.at("analytics_gpu_by_camera_serial"),
                              camera_serial.c_str(), nullptr) ||
        !gpu_mapping.at("recorder_gpu_by_logical_stream_id").is_object() ||
        gpu_mapping.at("recorder_gpu_by_logical_stream_id").size() != kExpectedStreamCount) {
        return fail(error_out, "spatial ROI session snapshot GPU mapping is invalid");
    }
    for (const std::string& stream_id : result->stream_order) {
        if (!gpu_mapping.at("recorder_gpu_by_logical_stream_id").contains(stream_id) ||
            !read_nonnegative_u64(gpu_mapping.at("recorder_gpu_by_logical_stream_id"),
                                  stream_id.c_str(), nullptr)) {
            return fail(error_out, "spatial ROI session snapshot recorder GPU mapping is invalid");
        }
    }

    const json& artifacts = session.at("artifacts");
    std::set<std::string> authority_paths;
    if (!exact_keys(artifacts, {"normalized_config", "verified_plan", "recorder_contract"}) ||
        !snapshot_validate_artifact_reference(artifacts.at("normalized_config"), &authority_paths) ||
        !snapshot_validate_artifact_reference(artifacts.at("verified_plan"), &authority_paths) ||
        !snapshot_validate_artifact_reference(artifacts.at("recorder_contract"), &authority_paths)) {
        return fail(error_out, "spatial ROI session snapshot authority artifacts are invalid");
    }

    const json& rois = session.at("rois");
    if (!rois.is_array() || rois.size() != kExpectedStreamCount) {
        return fail(error_out, "spatial ROI session snapshot must contain four ROIs");
    }
    std::set<std::string> roi_ids;
    std::set<std::string> region_ids;
    for (std::size_t index = 0; index < kExpectedStreamCount; ++index) {
        const json& roi = rois.at(index);
        if (!exact_keys(roi,
                        {"stream_id", "logical_stream_id", "roi_id", "region_id",
                         "arena_group_id", "arena_id", "geometry", "source_geometry",
                         "encoded_geometry", "encode_profile", "encode_fps", "codec",
                         "tuning", "analytics_gpu_id", "source_gpu_id", "recorder_gpu_id",
                         "assigned_gpu_id", "expected_shard_gpu_ids"}) ||
            !string_field_equals(roi, "stream_id", result->stream_order.at(index)) ||
            !string_field_equals(roi, "logical_stream_id", result->stream_order.at(index)) ||
            !snapshot_nonempty_string(roi, "roi_id") ||
            !snapshot_nonempty_string(roi, "region_id") ||
            !snapshot_nonempty_string(roi, "arena_group_id") ||
            !roi_ids.insert(roi.at("roi_id").get<std::string>()).second ||
            !region_ids.insert(roi.at("region_id").get<std::string>()).second ||
            !(roi.at("arena_id").is_null() ||
              (roi.at("arena_id").is_string() && !roi.at("arena_id").get<std::string>().empty())) ||
            !snapshot_validate_geometry(roi.at("geometry")) ||
            !exact_keys(roi.at("source_geometry"),
                        {"native_raster", "content_rect", "coordinate_space"}) ||
            !snapshot_validate_raster(roi.at("source_geometry").at("native_raster")) ||
            !snapshot_validate_rect(roi.at("source_geometry").at("content_rect")) ||
            !snapshot_nonempty_string(roi.at("source_geometry"), "coordinate_space") ||
            !exact_keys(roi.at("encoded_geometry"),
                        {"raster", "content_rect", "coordinate_space"}) ||
            !snapshot_validate_raster(roi.at("encoded_geometry").at("raster")) ||
            !snapshot_validate_rect(roi.at("encoded_geometry").at("content_rect")) ||
            !snapshot_nonempty_string(roi.at("encoded_geometry"), "coordinate_space") ||
            !snapshot_validate_profile(roi.at("encode_profile")) ||
            !read_nonnegative_u64(roi, "encode_fps", nullptr) ||
            !snapshot_nonempty_string(roi, "codec") ||
            !snapshot_nonempty_string(roi, "tuning") ||
            !read_nonnegative_u64(roi, "analytics_gpu_id", nullptr) ||
            !read_nonnegative_u64(roi, "source_gpu_id", nullptr) ||
            !read_nonnegative_u64(roi, "recorder_gpu_id", nullptr) ||
            !read_nonnegative_u64(roi, "assigned_gpu_id", nullptr) ||
            !roi.at("expected_shard_gpu_ids").is_array() ||
            roi.at("expected_shard_gpu_ids").size() != 1U ||
            !snapshot_nonnegative_value(roi.at("expected_shard_gpu_ids").at(0))) {
            return fail(error_out, "spatial ROI session snapshot ROI identity/geometry is invalid");
        }
        const json& geometry = roi.at("geometry");
        const json& source_geometry = roi.at("source_geometry");
        const json& encoded_geometry = roi.at("encoded_geometry");
        const json& padding = geometry.at("padding");
        const json& profile = roi.at("encode_profile");
        const std::uint64_t encoded_raster_width =
            geometry.at("encoded_raster").at("width").get<std::uint64_t>();
        const std::uint64_t encoded_raster_height =
            geometry.at("encoded_raster").at("height").get<std::uint64_t>();
        const std::uint64_t encoded_content_width =
            geometry.at("encoded_content_rect").at("width").get<std::uint64_t>();
        const std::uint64_t encoded_content_height =
            geometry.at("encoded_content_rect").at("height").get<std::uint64_t>();
        const json& session_authorities = session.at("authorities");
        const json& gpu_mapping = session.at("gpu_mapping");
        const json& analytics_mapping =
            gpu_mapping.at("analytics_gpu_by_camera_serial");
        const json& recorder_mapping =
            gpu_mapping.at("recorder_gpu_by_logical_stream_id");
        if (geometry.at("native_raster") != session.at("native_raster") ||
            source_geometry.at("native_raster") !=
                session.at("native_raster") ||
            source_geometry.at("native_raster") !=
                geometry.at("native_raster") ||
            source_geometry.at("content_rect") != geometry.at("content_rect") ||
            encoded_geometry.at("raster") != geometry.at("encoded_raster") ||
            encoded_geometry.at("content_rect") !=
                geometry.at("encoded_content_rect") ||
            !string_field_equals(
                geometry, "source_coordinate_space",
                "camera_native_full_frame_pixels") ||
            !string_field_equals(
                geometry, "video_coordinate_space",
                "spatial_roi_encoded_pixels") ||
            !string_field_equals(
                source_geometry, "coordinate_space",
                "camera_native_full_frame_pixels") ||
            !string_field_equals(
                encoded_geometry, "coordinate_space",
                "spatial_roi_encoded_pixels") ||
            !snapshot_rect_fits(geometry.at("content_rect"),
                                geometry.at("native_raster")) ||
            !snapshot_rect_fits(geometry.at("encoded_content_rect"),
                                geometry.at("encoded_raster")) ||
            geometry.at("content_rect").at("width") !=
                geometry.at("encoded_content_rect").at("width") ||
            geometry.at("content_rect").at("height") !=
                geometry.at("encoded_content_rect").at("height") ||
            geometry.at("encoded_content_rect").at("x") != 0 ||
            geometry.at("encoded_content_rect").at("y") != 0 ||
            geometry.at("content_offset").at("x") != 0 ||
            geometry.at("content_offset").at("y") != 0 ||
            padding.at("left") != 0 || padding.at("top") != 0 ||
            padding.at("value_mono8") != 0 ||
            padding.at("right") !=
                encoded_raster_width - encoded_content_width ||
            padding.at("bottom") !=
                encoded_raster_height - encoded_content_height ||
            geometry.at("layout") != session_authorities.at("layout") ||
            geometry.at("materialization") !=
                session_authorities.at("materialization") ||
            geometry.at("registration") !=
                session_authorities.at("registration") ||
            (index != 0 &&
             (geometry.at("layout") != rois.at(0).at("geometry").at("layout") ||
              geometry.at("materialization") !=
                  rois.at(0).at("geometry").at("materialization") ||
              geometry.at("registration") !=
                  rois.at(0).at("geometry").at("registration"))) ||
            analytics_mapping.at(camera_serial) != roi.at("analytics_gpu_id") ||
            roi.at("source_gpu_id") != roi.at("analytics_gpu_id") ||
            roi.at("assigned_gpu_id") != roi.at("recorder_gpu_id") ||
            recorder_mapping.at(result->stream_order.at(index)) !=
                roi.at("recorder_gpu_id") ||
            roi.at("expected_shard_gpu_ids").at(0) !=
                roi.at("recorder_gpu_id") ||
            roi.at("encode_fps") == 0 ||
            profile.at("frame_rate") != roi.at("encode_fps") ||
            roi.at("codec") != profile.at("codec") ||
            roi.at("tuning") != profile.at("tuning")) {
            return fail(error_out,
                        "spatial ROI session snapshot ROI geometry/profile/GPU invariants are invalid");
        }
    }
    if (session.at("status") == "complete") {
        if (!session.at("finalized_session_receipt").is_object() ||
            !snapshot_validate_raw_receipt(session.at("finalized_session_receipt"),
                                            session, result->stream_order, error_out)) {
            return false;
        }
        const json& receipt_streams =
            session.at("finalized_session_receipt").at("streams");
        const json& reference_ranges = receipt_streams.at(0).at("ranges");
        for (std::size_t index = 1; index < kExpectedStreamCount; ++index) {
            const json& ranges = receipt_streams.at(index).at("ranges");
            if (ranges.at("frame_count") != reference_ranges.at("frame_count") ||
                ranges.at("recording_frame_id") !=
                    reference_ranges.at("recording_frame_id")) {
                return fail(error_out,
                            "complete spatial ROI receipt lanes do not share a frame range");
            }
        }
        result->finalized_session_receipt = session.at("finalized_session_receipt");
    } else if (!session.at("finalized_session_receipt").is_null()) {
        return fail(error_out, "pending/failed spatial ROI session snapshot must have null receipt");
    } else {
        result->finalized_session_receipt = nullptr;
    }
    if (!(session.at("recorder_process_status").is_null() ||
          session.at("recorder_process_status").is_object()) ||
        !(session.at("producer_status").is_null() ||
          session.at("producer_status").is_object())) {
        return fail(error_out, "spatial ROI session snapshot status payload is invalid");
    }
    if (session.at("status") == "complete" &&
        !snapshot_validate_complete_process_status(
            session.at("recorder_process_status"),
            &session.at("finalized_session_receipt")
                 .at("root_authority")
                 .at("artifact_root_identity"))) {
        return fail(error_out,
                    "complete spatial ROI session snapshot requires checked storage preflight in ready and terminal process status");
    }
    if (session.at("status") == "complete" &&
        !snapshot_validate_complete_receipt_budgets(
            session.at("finalized_session_receipt"),
            session.at("recorder_process_status"))) {
        return fail(error_out,
                    "complete spatial ROI receipt artifacts exceed the authenticated storage budgets");
    }
    if (session.at("status") == "complete" &&
        !snapshot_validate_complete_producer_status(
            session.at("producer_status"), session,
            session.at("finalized_session_receipt"))) {
        return fail(error_out,
                    "complete spatial ROI producer status is not a stopped, fully submitted four-lane result");
    }
    result->status = session.at("status").get<std::string>();
    result->camera_serial = camera_serial;
    return true;
}

}  // namespace

bool validate_spatial_roi_session_snapshot_json(
    const nlohmann::json& snapshot,
    SpatialRoiSessionSnapshotValidation* validation_out,
    std::string* error_out)
{
    if (validation_out == nullptr) {
        return fail(error_out, "spatial ROI session snapshot validation destination is null");
    }
    *validation_out = SpatialRoiSessionSnapshotValidation{};
    if (error_out != nullptr) {
        error_out->clear();
    }
    return snapshot_validate_closed_object(snapshot, validation_out, error_out);
}

bool build_spatial_roi_session_snapshot_json(
    const SpatialRoiRecorderCameraContractView& camera_contract,
    const SpatialRoiSessionArtifactReference& normalized_config,
    const SpatialRoiSessionArtifactReference& verified_plan,
    const SpatialRoiSessionArtifactReference& recorder_contract,
    const std::string& status,
    const nlohmann::json* recorder_process_status,
    const nlohmann::json* producer_status,
    const nlohmann::json* finalized_session_receipt,
    nlohmann::json* snapshot_out,
    std::string* error_out)
{
    if (snapshot_out == nullptr) {
        return fail(error_out, "spatial ROI session snapshot destination is null");
    }
    *snapshot_out = json::object();
    if (error_out != nullptr) {
        error_out->clear();
    }

    if (status != "pending" && status != "complete" && status != "failed") {
        return fail(error_out,
                    "spatial ROI session snapshot status must be pending, complete, or failed");
    }
    if (status != "complete" && finalized_session_receipt != nullptr) {
        return fail(error_out,
                    "pending/failed spatial ROI session snapshots must not supply a finalized receipt");
    }
    if (!validate_optional_status(recorder_process_status,
                                  "recorder-process",
                                  error_out) ||
        !validate_optional_status(producer_status, "producer", error_out)) {
        return false;
    }
    if (status == "complete" &&
        (recorder_process_status == nullptr ||
         !snapshot_validate_complete_process_status(*recorder_process_status))) {
        return fail(error_out,
                    "complete spatial ROI session snapshot requires checked storage preflight in ready and terminal process status");
    }
    if (camera_contract.schema_id !=
            kSpatialRoiRecorderCameraContractSchemaId ||
        camera_contract.schema_version !=
            kSpatialRoiRecorderCameraContractSchemaVersion ||
        camera_contract.product_kind != kSpatialRoiRecorderCameraProductKind ||
        camera_contract.camera_id < 0 ||
        !safe_identifier(camera_contract.camera_serial) ||
        !safe_identifier(camera_contract.recording_id) ||
        camera_contract.session_id != camera_contract.recording_id ||
        !canonical_sha256(camera_contract.recording_identity_token) ||
        camera_contract.recording_identity_token !=
            orange::shaman_v2_recording_identity::token_for_recording_id(
                camera_contract.recording_id) ||
        !safe_identifier(camera_contract.producer_generation) ||
        !canonical_sha256(camera_contract.spatial_roi_plan_sha256) ||
        camera_contract.native_raster.width == 0 ||
        camera_contract.native_raster.height == 0 ||
        camera_contract.analytics_gpu_id < 0 ||
        camera_contract.stream_count != kExpectedStreamCount ||
        camera_contract.stream_order.size() != kExpectedStreamCount ||
        camera_contract.streams.size() != kExpectedStreamCount) {
        return fail(error_out,
                    "spatial ROI session camera contract has invalid shared identity");
    }

    std::set<std::string> authority_artifact_paths;
    if (!validate_artifact_reference(normalized_config,
                                     &authority_artifact_paths,
                                     "normalized-config",
                                     error_out) ||
        !validate_artifact_reference(verified_plan,
                                     &authority_artifact_paths,
                                     "verified-plan",
                                     error_out) ||
        !validate_artifact_reference(recorder_contract,
                                     &authority_artifact_paths,
                                     "recorder-contract",
                                     error_out)) {
        return false;
    }

    const auto analytics_gpu_it =
        camera_contract.analytics_gpu_by_camera_serial.find(
            camera_contract.camera_serial);
    if (camera_contract.analytics_gpu_by_camera_serial.size() != 1 ||
        analytics_gpu_it ==
            camera_contract.analytics_gpu_by_camera_serial.end() ||
        analytics_gpu_it->second != camera_contract.analytics_gpu_id ||
        camera_contract.recorder_gpu_by_logical_stream_id.size() !=
            kExpectedStreamCount) {
        return fail(error_out,
                    "spatial ROI session camera GPU mapping is not exact");
    }

    const SpatialRoiRecorderStreamView& first_stream =
        camera_contract.streams.front();
    if (!valid_authority(first_stream.geometry.layout) ||
        !valid_authority(first_stream.geometry.materialization) ||
        !valid_authority(first_stream.geometry.registration)) {
        return fail(error_out,
                    "spatial ROI session geometry authorities are incomplete");
    }

    std::set<std::string> logical_stream_ids;
    std::set<std::string> roi_ids;
    std::set<std::string> region_ids;
    json rois = json::array();
    for (std::size_t index = 0; index < kExpectedStreamCount; ++index) {
        const SpatialRoiRecorderStreamView& stream =
            camera_contract.streams[index];
        const std::string& ordered_stream_id =
            camera_contract.stream_order[index];
        const auto recorder_gpu_it =
            camera_contract.recorder_gpu_by_logical_stream_id.find(
                ordered_stream_id);
        const auto& geometry = stream.geometry;
        const bool geometry_valid =
            same_raster(geometry.native_raster,
                        camera_contract.native_raster) &&
            geometry.source_coordinate_space ==
                "camera_native_full_frame_pixels" &&
            geometry.video_coordinate_space ==
                "spatial_roi_encoded_pixels" &&
            rect_fits(geometry.content_rect, geometry.native_raster) &&
            geometry.encoded_raster.width > 0 &&
            geometry.encoded_raster.height > 0 &&
            rect_fits(geometry.encoded_content_rect,
                      geometry.encoded_raster) &&
            geometry.content_rect.width ==
                geometry.encoded_content_rect.width &&
            geometry.content_rect.height ==
                geometry.encoded_content_rect.height &&
            geometry.encoded_content_rect.x == 0 &&
            geometry.encoded_content_rect.y == 0 &&
            geometry.content_offset_x == 0 &&
            geometry.content_offset_y == 0 &&
            geometry.padding.left == 0 && geometry.padding.top == 0 &&
            geometry.padding.value_mono8 == 0 &&
            geometry.padding.right ==
                geometry.encoded_raster.width -
                    geometry.encoded_content_rect.width &&
            geometry.padding.bottom ==
                geometry.encoded_raster.height -
                    geometry.encoded_content_rect.height;
        if (ordered_stream_id.empty() ||
            !safe_identifier(ordered_stream_id) ||
            !logical_stream_ids.insert(ordered_stream_id).second ||
            stream.stream_id != ordered_stream_id ||
            stream.logical_stream_id != ordered_stream_id ||
            stream.stream_kind != kSpatialRoiRecordingOutputKind ||
            stream.output_kind != kSpatialRoiRecordingOutputKind ||
            stream.camera_id != camera_contract.camera_id ||
            stream.camera_serial != camera_contract.camera_serial ||
            stream.recording_id != camera_contract.recording_id ||
            stream.session_id != camera_contract.session_id ||
            stream.recording_identity_token !=
                camera_contract.recording_identity_token ||
            stream.producer_generation != camera_contract.producer_generation ||
            stream.spatial_roi_plan_sha256 !=
                camera_contract.spatial_roi_plan_sha256 ||
            stream.analytics_gpu_id != camera_contract.analytics_gpu_id ||
            stream.source_gpu_id != stream.analytics_gpu_id ||
            stream.recorder_gpu_id < 0 ||
            stream.assigned_gpu_id != stream.recorder_gpu_id ||
            recorder_gpu_it ==
                camera_contract.recorder_gpu_by_logical_stream_id.end() ||
            stream.recorder_gpu_id != recorder_gpu_it->second ||
            stream.expected_shard_gpu_ids.size() != 1 ||
            stream.expected_shard_gpu_ids.front() != stream.recorder_gpu_id ||
            !safe_identifier(stream.roi_id) ||
            !safe_identifier(stream.region_id) ||
            !safe_identifier(stream.arena_group_id) ||
            !roi_ids.insert(stream.roi_id).second ||
            !region_ids.insert(stream.region_id).second ||
            (stream.has_arena_id && !safe_identifier(stream.arena_id)) ||
            (!stream.has_arena_id && !stream.arena_id.empty()) ||
            !same_authority(geometry.layout, first_stream.geometry.layout) ||
            !same_authority(geometry.materialization,
                            first_stream.geometry.materialization) ||
            !same_authority(geometry.registration,
                            first_stream.geometry.registration) ||
            !geometry_valid ||
            stream.encode_fps == 0 ||
            stream.encode_profile.frame_rate != stream.encode_fps ||
            stream.codec != stream.encode_profile.codec ||
            stream.tuning != stream.encode_profile.tuning ||
            !snapshot_validate_profile(
                encode_profile_json(stream.encode_profile))) {
            return fail(error_out,
                        "spatial ROI session stream identity/order/geometry is inconsistent at index " +
                            std::to_string(index));
        }

        const json source_geometry = {
            {"native_raster", raster_json(geometry.native_raster)},
            {"content_rect", rect_json(geometry.content_rect)},
            {"coordinate_space", geometry.source_coordinate_space},
        };
        const json encoded_geometry = {
            {"raster", raster_json(geometry.encoded_raster)},
            {"content_rect", rect_json(geometry.encoded_content_rect)},
            {"coordinate_space", geometry.video_coordinate_space},
        };
        rois.push_back({
            {"stream_id", stream.stream_id},
            {"logical_stream_id", stream.logical_stream_id},
            {"roi_id", stream.roi_id},
            {"region_id", stream.region_id},
            {"arena_group_id", stream.arena_group_id},
            {"arena_id", stream.has_arena_id ? json(stream.arena_id)
                                             : json(nullptr)},
            {"geometry", geometry_json(geometry)},
            {"source_geometry", source_geometry},
            {"encoded_geometry", encoded_geometry},
            {"encode_profile", encode_profile_json(stream.encode_profile)},
            {"encode_fps", stream.encode_fps},
            {"codec", stream.codec},
            {"tuning", stream.tuning},
            {"analytics_gpu_id", stream.analytics_gpu_id},
            {"source_gpu_id", stream.source_gpu_id},
            {"recorder_gpu_id", stream.recorder_gpu_id},
            {"assigned_gpu_id", stream.assigned_gpu_id},
            {"expected_shard_gpu_ids", stream.expected_shard_gpu_ids},
        });
    }

    if (logical_stream_ids.size() != kExpectedStreamCount ||
        roi_ids.size() != kExpectedStreamCount ||
        region_ids.size() != kExpectedStreamCount) {
        return fail(error_out,
                    "spatial ROI session snapshot does not contain four unique streams");
    }

    if (status == "complete" &&
        (finalized_session_receipt == nullptr ||
         !validate_finalized_session_receipt(
             camera_contract, *finalized_session_receipt, error_out))) {
        if (error_out != nullptr && error_out->empty()) {
            *error_out =
                "complete spatial ROI session snapshot requires a valid finalized receipt";
        }
        return false;
    }
    if (status == "complete" &&
        !snapshot_validate_complete_process_status(
            *recorder_process_status,
            &finalized_session_receipt->at("root_authority")
                 .at("artifact_root_identity"))) {
        return fail(error_out,
                    "complete spatial ROI session preflight root identity does not match finalized receipt");
    }
    if (status == "complete" &&
        !snapshot_validate_complete_receipt_budgets(
            *finalized_session_receipt, *recorder_process_status)) {
        return fail(error_out,
                    "complete spatial ROI receipt artifacts exceed the authenticated storage budgets");
    }
    json analytics_gpus = json::object();
    for (const auto& entry : camera_contract.analytics_gpu_by_camera_serial) {
        analytics_gpus[entry.first] = entry.second;
    }
    json recorder_gpus = json::object();
    for (const auto& entry :
         camera_contract.recorder_gpu_by_logical_stream_id) {
        recorder_gpus[entry.first] = entry.second;
    }

    *snapshot_out = {
        {"schema_id", kSpatialRoiSessionSnapshotSchemaId},
        {"schema_version", kSpatialRoiSessionSnapshotSchemaVersion},
        {"status", status},
        {"recording_id", camera_contract.recording_id},
        {"session_id", camera_contract.session_id},
        {"recording_identity_token", camera_contract.recording_identity_token},
        {"producer_generation", camera_contract.producer_generation},
        {"spatial_roi_plan_sha256", camera_contract.spatial_roi_plan_sha256},
        {"product_kind", camera_contract.product_kind},
        {"stream_count", kExpectedStreamCount},
        {"stream_order", camera_contract.stream_order},
        {"identity", {{"recording_id", camera_contract.recording_id},
                       {"session_id", camera_contract.session_id},
                       {"recording_identity_token",
                        camera_contract.recording_identity_token},
                       {"producer_generation",
                        camera_contract.producer_generation},
                       {"spatial_roi_plan_sha256",
                        camera_contract.spatial_roi_plan_sha256}}},
        {"camera", {{"camera_id", camera_contract.camera_id},
                     {"camera_serial", camera_contract.camera_serial},
                     {"native_raster",
                      raster_json(camera_contract.native_raster)}}},
        {"camera_id", camera_contract.camera_id},
        {"camera_serial", camera_contract.camera_serial},
        {"native_raster", raster_json(camera_contract.native_raster)},
        {"authorities",
         {{"layout", authority_json(first_stream.geometry.layout)},
          {"materialization",
           authority_json(first_stream.geometry.materialization)},
          {"registration", authority_json(first_stream.geometry.registration)}}},
        {"gpu_mapping", {{"analytics_gpu_by_camera_serial", analytics_gpus},
                          {"recorder_gpu_by_logical_stream_id", recorder_gpus}}},
        {"artifacts", {{"normalized_config",
                         artifact_reference_json(normalized_config)},
                        {"verified_plan", artifact_reference_json(verified_plan)},
                        {"recorder_contract",
                         artifact_reference_json(recorder_contract)}}},
        {"rois", std::move(rois)},
        {"finalized_session_receipt",
         status == "complete" ? *finalized_session_receipt : json(nullptr)},
        {"recorder_process_status",
         recorder_process_status != nullptr ? *recorder_process_status
                                             : json(nullptr)},
        {"producer_status", producer_status != nullptr ? *producer_status
                                                         : json(nullptr)},
    };
    if (status == "complete" &&
        (producer_status == nullptr ||
         !snapshot_validate_complete_producer_status(
             *producer_status, *snapshot_out, *finalized_session_receipt))) {
        *snapshot_out = json::object();
        return fail(error_out,
                    "complete spatial ROI producer status is not a stopped, fully submitted four-lane result");
    }
    return true;
}

bool build_spatial_roi_session_snapshot_json(
    const SpatialRoiRecorderCameraContractView& camera_contract,
    const SpatialRoiSessionArtifactReference& normalized_config,
    const SpatialRoiSessionArtifactReference& verified_plan,
    const SpatialRoiSessionArtifactReference& recorder_contract,
    const std::string& status,
    const nlohmann::json* recorder_process_status,
    const nlohmann::json* producer_status,
    nlohmann::json* snapshot_out,
    std::string* error_out)
{
    return build_spatial_roi_session_snapshot_json(
        camera_contract,
        normalized_config,
        verified_plan,
        recorder_contract,
        status,
        recorder_process_status,
        producer_status,
        nullptr,
        snapshot_out,
        error_out);
}

bool build_spatial_roi_session_snapshot_json(
    const SpatialRoiRecorderCameraContractView& camera_contract,
    const SpatialRoiSessionArtifactReference& normalized_config,
    const SpatialRoiSessionArtifactReference& verified_plan,
    const SpatialRoiSessionArtifactReference& recorder_contract,
    const std::string& status,
    nlohmann::json* snapshot_out,
    std::string* error_out)
{
    return build_spatial_roi_session_snapshot_json(
        camera_contract,
        normalized_config,
        verified_plan,
        recorder_contract,
        status,
        nullptr,
        nullptr,
        snapshot_out,
        error_out);
}

}  // namespace orange::session::spatial_roi
