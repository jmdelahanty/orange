#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>

namespace orange::external_recorder::ipc {

inline constexpr const char* kProtocolName = "orange.external_recorder.ipc";
inline constexpr int kProtocolVersion = 1;
inline constexpr const char* kRecorderHelloKind = "RECORDER_HELLO";
inline constexpr const char* kClientHelloKind = "CLIENT_HELLO";
inline constexpr const char* kRecorderStatusKind = "RECORDER_STATUS";
inline constexpr const char* kClientControlKind = "CLIENT_CONTROL";
inline constexpr const char* kClientControlDrain = "drain";
inline constexpr const char* kClientControlFinalize = "finalize";

struct HelloFields {
    std::string kind;
    std::string protocol;
    int version = 0;
    std::string role;
    std::string camera_serial;
    std::string session_id;
    std::string stream_id;
    std::string features;
    int frame_rate = 0;
    int resolved_gop_length = 0;
    std::string recording_config_fingerprint_scope;
    std::string recording_config_fingerprint;
    std::string error;
};

inline constexpr const char* kRecordingConfigFingerprintScope =
    "frame_rate_and_gop_v1";

inline std::string build_recording_config_fingerprint(int frame_rate,
                                                      int resolved_gop_length)
{
    const std::string canonical =
        std::string("schema=orange.external_recorder.recording_config.v1\n") +
        "frame_rate=" + std::to_string(frame_rate) + "\n" +
        "resolved_gop_length=" + std::to_string(resolved_gop_length) + "\n";
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : canonical) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << "fnv1a64:" << std::hex << std::nouppercase << std::setfill('0')
        << std::setw(16) << hash;
    return out.str();
}

inline bool validate_recording_config_identity(const HelloFields& hello,
                                               int expected_frame_rate,
                                               int expected_gop_length,
                                               std::string* error_out = nullptr)
{
    const std::string expected_fingerprint = build_recording_config_fingerprint(
        expected_frame_rate,
        expected_gop_length);
    std::string error;
    if (hello.frame_rate <= 0) {
        error = "missing frame_rate in protocol hello";
    } else if (hello.resolved_gop_length <= 0) {
        error = "missing resolved_gop_length in protocol hello";
    } else if (hello.recording_config_fingerprint_scope !=
               kRecordingConfigFingerprintScope) {
        error = "unsupported recording_config_fingerprint_scope";
    } else if (hello.recording_config_fingerprint.empty()) {
        error = "missing recording_config_fingerprint in protocol hello";
    } else if (hello.frame_rate != expected_frame_rate) {
        error = "frame_rate mismatch: peer=" + std::to_string(hello.frame_rate) +
            " expected=" + std::to_string(expected_frame_rate);
    } else if (hello.resolved_gop_length != expected_gop_length) {
        error = "resolved_gop_length mismatch: peer=" +
            std::to_string(hello.resolved_gop_length) + " expected=" +
            std::to_string(expected_gop_length);
    } else if (hello.recording_config_fingerprint != expected_fingerprint) {
        error = "recording_config_fingerprint mismatch: peer=" +
            hello.recording_config_fingerprint + " expected=" + expected_fingerprint;
    }
    if (error_out) {
        *error_out = error;
    }
    return error.empty();
}

inline bool validate_frame_grouping(uint64_t recording_frame_id,
                                    uint64_t descriptor_gop_index,
                                    uint32_t descriptor_frame_index_within_gop,
                                    uint32_t expected_gop_length,
                                    std::string* error_out = nullptr)
{
    std::string error;
    if (recording_frame_id == 0) {
        error = "recording_frame_id must be positive";
    } else if (expected_gop_length == 0) {
        error = "expected_gop_length must be positive";
    } else {
        const uint64_t zero_based_frame = recording_frame_id - 1;
        const uint64_t expected_gop_index =
            zero_based_frame / static_cast<uint64_t>(expected_gop_length);
        const uint32_t expected_frame_index = static_cast<uint32_t>(
            zero_based_frame % static_cast<uint64_t>(expected_gop_length));
        if (descriptor_gop_index != expected_gop_index ||
            descriptor_frame_index_within_gop != expected_frame_index) {
            error = "frame grouping mismatch at recording_frame_id=" +
                std::to_string(recording_frame_id) + ": descriptor=(gop=" +
                std::to_string(descriptor_gop_index) + ",index=" +
                std::to_string(descriptor_frame_index_within_gop) +
                ") expected=(gop=" + std::to_string(expected_gop_index) +
                ",index=" + std::to_string(expected_frame_index) +
                ",gop_length=" + std::to_string(expected_gop_length) + ")";
        }
    }
    if (error_out) {
        *error_out = error;
    }
    return error.empty();
}

// Canonical identity assigned when Orange submits a frame to the recorder.
// NVENC returns recording_frame_id through outputTimeStamp; the merger uses
// that value only as a lookup key and never derives a second GOP assignment.
struct SubmittedFrameIdentity {
    uint64_t recording_frame_id = 0;
    uint64_t gop_index = 0;
    uint32_t frame_index_within_gop = 0;
};

class SubmittedFrameIdentityRegistry {
public:
    bool note(const SubmittedFrameIdentity& identity,
              std::string* error_out = nullptr)
    {
        std::string error;
        if (identity.recording_frame_id == 0) {
            error = "submitted recording_frame_id must be positive";
        } else {
            const auto [unused, inserted] = identities_.emplace(
                identity.recording_frame_id,
                identity);
            (void)unused;
            if (!inserted) {
                error = "duplicate submitted recording_frame_id=" +
                    std::to_string(identity.recording_frame_id);
            }
        }
        if (error_out) {
            *error_out = error;
        }
        return error.empty();
    }

    bool consume(uint64_t recording_frame_id,
                 SubmittedFrameIdentity* identity_out,
                 std::string* error_out = nullptr)
    {
        std::string error;
        if (!identity_out) {
            error = "null submitted-frame identity destination";
        } else if (recording_frame_id == 0) {
            error = "NVENC output timestamp must identify a positive recording_frame_id";
        } else {
            const auto it = identities_.find(recording_frame_id);
            if (it == identities_.end()) {
                error = "NVENC output references unknown or already-consumed recording_frame_id=" +
                    std::to_string(recording_frame_id);
            } else {
                *identity_out = it->second;
                identities_.erase(it);
            }
        }
        if (error_out) {
            *error_out = error;
        }
        return error.empty();
    }

    size_t size() const { return identities_.size(); }
    bool empty() const { return identities_.empty(); }

private:
    std::unordered_map<uint64_t, SubmittedFrameIdentity> identities_;
};

inline bool validate_pending_gop_budget(uint64_t pending_gops,
                                        uint64_t pending_bytes,
                                        uint64_t max_pending_gops,
                                        uint64_t max_pending_bytes,
                                        std::string* error_out = nullptr)
{
    std::string error;
    if (max_pending_gops > 0 && pending_gops > max_pending_gops) {
        error = "pending_gops=" + std::to_string(pending_gops) +
            " exceeds limit=" + std::to_string(max_pending_gops);
    } else if (max_pending_bytes > 0 && pending_bytes > max_pending_bytes) {
        error = "pending_bytes=" + std::to_string(pending_bytes) +
            " exceeds limit=" + std::to_string(max_pending_bytes);
    }
    if (error_out) {
        *error_out = error;
    }
    return error.empty();
}

inline bool validate_pending_frontier_age(uint64_t pending_gops,
                                          uint64_t frontier_age_ms,
                                          uint64_t max_frontier_age_ms,
                                          std::string* error_out = nullptr)
{
    std::string error;
    if (pending_gops > 0 && max_frontier_age_ms > 0 &&
        frontier_age_ms > max_frontier_age_ms) {
        error = "pending GOP release frontier age_ms=" +
            std::to_string(frontier_age_ms) + " exceeds limit_ms=" +
            std::to_string(max_frontier_age_ms);
    }
    if (error_out) {
        *error_out = error;
    }
    return error.empty();
}

struct RecorderStatusFields {
    std::string kind;
    std::string protocol;
    int version = 0;
    std::string role;
    std::string session_id;
    std::string stream_id;
    std::string status;
    uint64_t heartbeat_sequence = 0;
    uint64_t frames_received = 0;
    uint64_t acks_sent = 0;
    uint64_t frames_encoded = 0;
    uint64_t encode_dropped = 0;
    bool worker_failed = false;
    std::string error;
};

struct ClientControlFields {
    std::string kind;
    std::string protocol;
    int version = 0;
    std::string role;
    std::string camera_serial;
    std::string session_id;
    std::string stream_id;
    std::string command;
    std::string reason;
    std::string error;
};

inline std::string token_value(std::string value)
{
    if (value.empty()) {
        return "-";
    }
    std::replace_if(
        value.begin(),
        value.end(),
        [](unsigned char c) {
            return std::isspace(c) != 0 || c == '=';
        },
        '_');
    return value;
}

inline bool starts_with_kind(const std::string& line, const char* kind)
{
    if (!kind) {
        return false;
    }
    const std::string expected(kind);
    if (line.size() < expected.size()) {
        return false;
    }
    if (line.compare(0, expected.size(), expected) != 0) {
        return false;
    }
    return line.size() == expected.size() ||
           std::isspace(static_cast<unsigned char>(line[expected.size()])) != 0;
}

inline std::unordered_map<std::string, std::string> parse_key_values(std::istringstream* in)
{
    std::unordered_map<std::string, std::string> values;
    if (!in) {
        return values;
    }
    std::string token;
    while (*in >> token) {
        const size_t equals = token.find('=');
        if (equals == std::string::npos || equals == 0) {
            continue;
        }
        values[token.substr(0, equals)] = token.substr(equals + 1);
    }
    return values;
}

inline std::string find_value(const std::unordered_map<std::string, std::string>& values,
                              const char* key)
{
    const auto it = values.find(key);
    return it == values.end() ? std::string() : it->second;
}

inline bool parse_int_value(const std::string& value, int* out)
{
    if (!out || value.empty()) {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

inline bool parse_u64_value(const std::string& value, uint64_t* out)
{
    if (!out || value.empty()) {
        return false;
    }
    if (value[0] == '-') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        return false;
    }
    *out = static_cast<uint64_t>(parsed);
    return true;
}

inline bool parse_optional_u64_field(
    const std::unordered_map<std::string, std::string>& values,
    const char* key,
    uint64_t* out,
    std::string* error)
{
    const std::string value = find_value(values, key);
    if (value.empty()) {
        return true;
    }
    if (parse_u64_value(value, out)) {
        return true;
    }
    if (error) {
        *error = std::string("invalid numeric field: ") + key;
    }
    return false;
}

inline bool parse_optional_bool_field(
    const std::unordered_map<std::string, std::string>& values,
    const char* key,
    bool* out,
    std::string* error)
{
    const std::string value = find_value(values, key);
    if (value.empty()) {
        return true;
    }
    if (value == "true" || value == "1") {
        if (out) {
            *out = true;
        }
        return true;
    }
    if (value == "false" || value == "0") {
        if (out) {
            *out = false;
        }
        return true;
    }
    if (error) {
        *error = std::string("invalid bool field: ") + key;
    }
    return false;
}

inline bool parse_protocol_hello_line(const std::string& line,
                                      const char* expected_kind,
                                      HelloFields* hello)
{
    HelloFields parsed;
    std::istringstream in(line);
    in >> parsed.kind;
    if (!in || parsed.kind.empty()) {
        parsed.error = "missing hello kind";
        if (hello) {
            *hello = parsed;
        }
        return false;
    }
    if (expected_kind && parsed.kind != expected_kind) {
        parsed.error = "unexpected hello kind";
        if (hello) {
            *hello = parsed;
        }
        return false;
    }

    const std::unordered_map<std::string, std::string> values = parse_key_values(&in);
    parsed.protocol = find_value(values, "protocol");
    parsed.role = find_value(values, "role");
    parsed.camera_serial = find_value(values, "camera_serial");
    parsed.session_id = find_value(values, "session_id");
    parsed.stream_id = find_value(values, "stream_id");
    parsed.features = find_value(values, "features");
    parsed.recording_config_fingerprint_scope =
        find_value(values, "recording_config_fingerprint_scope");
    parsed.recording_config_fingerprint =
        find_value(values, "recording_config_fingerprint");

    const std::string version_value = find_value(values, "version");
    if (!version_value.empty() &&
        !parse_int_value(version_value, &parsed.version)) {
        parsed.error = "invalid protocol version";
    }
    const std::string frame_rate_value = find_value(values, "frame_rate");
    if (parsed.error.empty() && !frame_rate_value.empty() &&
        !parse_int_value(frame_rate_value, &parsed.frame_rate)) {
        parsed.error = "invalid frame_rate";
    }
    const std::string gop_value = find_value(values, "resolved_gop_length");
    if (parsed.error.empty() && !gop_value.empty() &&
        !parse_int_value(gop_value, &parsed.resolved_gop_length)) {
        parsed.error = "invalid resolved_gop_length";
    }

    if (parsed.error.empty()) {
        if (parsed.protocol != kProtocolName) {
            parsed.error = "unexpected protocol name";
        } else if (parsed.version != kProtocolVersion) {
            parsed.error = "unsupported protocol version";
        } else if (parsed.role.empty()) {
            parsed.error = "missing protocol role";
        }
    }

    if (hello) {
        *hello = parsed;
    }
    return parsed.error.empty();
}

inline bool parse_recorder_hello_line(const std::string& line, HelloFields* hello)
{
    return parse_protocol_hello_line(line, kRecorderHelloKind, hello);
}

inline bool parse_client_hello_line(const std::string& line, HelloFields* hello)
{
    return parse_protocol_hello_line(line, kClientHelloKind, hello);
}

inline bool parse_recorder_status_line(const std::string& line,
                                       RecorderStatusFields* status)
{
    RecorderStatusFields parsed;
    std::istringstream in(line);
    in >> parsed.kind;
    if (!in || parsed.kind.empty()) {
        parsed.error = "missing status kind";
        if (status) {
            *status = parsed;
        }
        return false;
    }
    if (parsed.kind != kRecorderStatusKind) {
        parsed.error = "unexpected status kind";
        if (status) {
            *status = parsed;
        }
        return false;
    }

    const std::unordered_map<std::string, std::string> values = parse_key_values(&in);
    parsed.protocol = find_value(values, "protocol");
    parsed.role = find_value(values, "role");
    parsed.session_id = find_value(values, "session_id");
    parsed.stream_id = find_value(values, "stream_id");
    parsed.status = find_value(values, "status");

    const std::string version_value = find_value(values, "version");
    if (!version_value.empty() &&
        !parse_int_value(version_value, &parsed.version)) {
        parsed.error = "invalid protocol version";
    } else if (parsed.protocol != kProtocolName) {
        parsed.error = "unexpected protocol name";
    } else if (parsed.version != kProtocolVersion) {
        parsed.error = "unsupported protocol version";
    } else if (parsed.role.empty()) {
        parsed.error = "missing protocol role";
    } else if (parsed.status.empty()) {
        parsed.error = "missing recorder status";
    }

    if (parsed.error.empty()) {
        parse_optional_u64_field(
            values, "heartbeat_sequence", &parsed.heartbeat_sequence, &parsed.error);
    }
    if (parsed.error.empty()) {
        parse_optional_u64_field(
            values, "frames_received", &parsed.frames_received, &parsed.error);
    }
    if (parsed.error.empty()) {
        parse_optional_u64_field(values, "acks_sent", &parsed.acks_sent, &parsed.error);
    }
    if (parsed.error.empty()) {
        parse_optional_u64_field(
            values, "frames_encoded", &parsed.frames_encoded, &parsed.error);
    }
    if (parsed.error.empty()) {
        parse_optional_u64_field(
            values, "encode_dropped", &parsed.encode_dropped, &parsed.error);
    }
    if (parsed.error.empty()) {
        parse_optional_bool_field(
            values, "worker_failed", &parsed.worker_failed, &parsed.error);
    }

    if (status) {
        *status = parsed;
    }
    return parsed.error.empty();
}

inline bool parse_client_control_line(const std::string& line,
                                      ClientControlFields* control)
{
    ClientControlFields parsed;
    std::istringstream in(line);
    in >> parsed.kind;
    if (!in || parsed.kind.empty()) {
        parsed.error = "missing control kind";
        if (control) {
            *control = parsed;
        }
        return false;
    }
    if (parsed.kind != kClientControlKind) {
        parsed.error = "unexpected control kind";
        if (control) {
            *control = parsed;
        }
        return false;
    }

    const std::unordered_map<std::string, std::string> values = parse_key_values(&in);
    parsed.protocol = find_value(values, "protocol");
    parsed.role = find_value(values, "role");
    parsed.camera_serial = find_value(values, "camera_serial");
    parsed.session_id = find_value(values, "session_id");
    parsed.stream_id = find_value(values, "stream_id");
    parsed.command = find_value(values, "command");
    parsed.reason = find_value(values, "reason");

    const std::string version_value = find_value(values, "version");
    if (!version_value.empty() &&
        !parse_int_value(version_value, &parsed.version)) {
        parsed.error = "invalid protocol version";
    } else if (parsed.protocol != kProtocolName) {
        parsed.error = "unexpected protocol name";
    } else if (parsed.version != kProtocolVersion) {
        parsed.error = "unsupported protocol version";
    } else if (parsed.role.empty()) {
        parsed.error = "missing protocol role";
    } else if (parsed.command.empty()) {
        parsed.error = "missing control command";
    }

    if (control) {
        *control = parsed;
    }
    return parsed.error.empty();
}

inline std::string build_recorder_hello_line(const std::string& session_id,
                                             const std::string& stream_id,
                                             int frame_rate,
                                             int resolved_gop_length)
{
    std::ostringstream out;
    out << kRecorderHelloKind
        << " protocol=" << kProtocolName
        << " version=" << kProtocolVersion
        << " role=recorder"
        << " session_id=" << token_value(session_id)
        << " stream_id=" << token_value(stream_id)
        << " frame_rate=" << frame_rate
        << " resolved_gop_length=" << resolved_gop_length
        << " recording_config_fingerprint_scope="
        << kRecordingConfigFingerprintScope
        << " recording_config_fingerprint="
        << build_recording_config_fingerprint(frame_rate, resolved_gop_length)
        << " features=frame_ack_release,recorder_status,client_control_drain,client_control_finalize,status_json,storage_preflight\n";
    return out.str();
}

inline std::string build_client_hello_line(const std::string& camera_serial,
                                           const std::string& session_id,
                                           const std::string& stream_id,
                                           const std::string& role,
                                           int frame_rate,
                                           int resolved_gop_length)
{
    std::ostringstream out;
    out << kClientHelloKind
        << " protocol=" << kProtocolName
        << " version=" << kProtocolVersion
        << " role=" << token_value(role)
        << " camera_serial=" << token_value(camera_serial)
        << " session_id=" << token_value(session_id)
        << " stream_id=" << token_value(stream_id)
        << " frame_rate=" << frame_rate
        << " resolved_gop_length=" << resolved_gop_length
        << " recording_config_fingerprint_scope="
        << kRecordingConfigFingerprintScope
        << " recording_config_fingerprint="
        << build_recording_config_fingerprint(frame_rate, resolved_gop_length)
        << " features=client_control_drain,client_control_finalize"
        << "\n";
    return out.str();
}

inline std::string build_recorder_status_line(const std::string& session_id,
                                              const std::string& stream_id,
                                              const std::string& status,
                                              uint64_t heartbeat_sequence,
                                              uint64_t frames_received,
                                              uint64_t acks_sent,
                                              uint64_t frames_encoded,
                                              uint64_t encode_dropped,
                                              bool worker_failed)
{
    std::ostringstream out;
    out << kRecorderStatusKind
        << " protocol=" << kProtocolName
        << " version=" << kProtocolVersion
        << " role=recorder"
        << " session_id=" << token_value(session_id)
        << " stream_id=" << token_value(stream_id)
        << " status=" << token_value(status)
        << " heartbeat_sequence=" << heartbeat_sequence
        << " frames_received=" << frames_received
        << " acks_sent=" << acks_sent
        << " frames_encoded=" << frames_encoded
        << " encode_dropped=" << encode_dropped
        << " worker_failed=" << (worker_failed ? "true" : "false")
        << "\n";
    return out.str();
}

inline std::string build_client_control_line(const std::string& camera_serial,
                                             const std::string& session_id,
                                             const std::string& stream_id,
                                             const std::string& role,
                                             const std::string& command,
                                             const std::string& reason)
{
    std::ostringstream out;
    out << kClientControlKind
        << " protocol=" << kProtocolName
        << " version=" << kProtocolVersion
        << " role=" << token_value(role)
        << " camera_serial=" << token_value(camera_serial)
        << " session_id=" << token_value(session_id)
        << " stream_id=" << token_value(stream_id)
        << " command=" << token_value(command)
        << " reason=" << token_value(reason)
        << "\n";
    return out.str();
}

}  // namespace orange::external_recorder::ipc
