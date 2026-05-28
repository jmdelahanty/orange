#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>

namespace orange::external_recorder::ipc {

inline constexpr const char* kProtocolName = "orange.external_recorder.ipc";
inline constexpr int kProtocolVersion = 1;
inline constexpr const char* kRecorderHelloKind = "RECORDER_HELLO";
inline constexpr const char* kClientHelloKind = "CLIENT_HELLO";

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
    auto find_value = [&values](const char* key) -> std::string {
        const auto it = values.find(key);
        return it == values.end() ? std::string() : it->second;
    };

    parsed.protocol = find_value("protocol");
    parsed.role = find_value("role");
    parsed.camera_serial = find_value("camera_serial");
    parsed.session_id = find_value("session_id");
    parsed.stream_id = find_value("stream_id");
    parsed.features = find_value("features");

    const std::string version_value = find_value("version");
    if (!version_value.empty()) {
        char* end = nullptr;
        const long parsed_version = std::strtol(version_value.c_str(), &end, 10);
        if (end != version_value.c_str() && *end == '\0') {
            parsed.version = static_cast<int>(parsed_version);
        }
    }

    if (parsed.protocol != kProtocolName) {
        parsed.error = "unexpected protocol name";
    } else if (parsed.version != kProtocolVersion) {
        parsed.error = "unsupported protocol version";
    } else if (parsed.role.empty()) {
        parsed.error = "missing protocol role";
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
        << " features=frame_ack_release,status_json,storage_preflight\n";
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
        << "\n";
    return out.str();
}

}  // namespace orange::external_recorder::ipc
