#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
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
    std::string error;
};

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

    const std::string version_value = find_value(values, "version");
    if (!version_value.empty() &&
        !parse_int_value(version_value, &parsed.version)) {
        parsed.error = "invalid protocol version";
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
                                             const std::string& stream_id)
{
    std::ostringstream out;
    out << kRecorderHelloKind
        << " protocol=" << kProtocolName
        << " version=" << kProtocolVersion
        << " role=recorder"
        << " session_id=" << token_value(session_id)
        << " stream_id=" << token_value(stream_id)
        << " features=frame_ack_release,recorder_status,client_control_drain,client_control_finalize,status_json,storage_preflight\n";
    return out.str();
}

inline std::string build_client_hello_line(const std::string& camera_serial,
                                           const std::string& session_id,
                                           const std::string& stream_id,
                                           const std::string& role)
{
    std::ostringstream out;
    out << kClientHelloKind
        << " protocol=" << kProtocolName
        << " version=" << kProtocolVersion
        << " role=" << token_value(role)
        << " camera_serial=" << token_value(camera_serial)
        << " session_id=" << token_value(session_id)
        << " stream_id=" << token_value(stream_id)
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
