#include "json.hpp"
#include "session/spatial_roi_recorder_camera_contract.h"
#include "session/spatial_roi_recorder_contract.h"
#include "session/spatial_roi_recording_config.h"
#include "spatial_roi_camera_recorder.h"
#include "spatial_roi_camera_recorder_stream_core.h"
#include "spatial_roi_recorder_artifact_root.h"
#include "spatial_roi_recorder_evidence.h"
#include "spatial_roi_recorder_scheduling.h"
#include "spatial_roi_recorder_storage_preflight.h"
#include "spatial_roi_socket_runtime_directory.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;
namespace contract = orange::session::spatial_roi;
namespace recording = orange::spatial_roi::recording;
namespace ipc = orange::spatial_roi::ipc;

// The contract parser has the same structural bounds. Keep the executable's
// file boundary at or below those limits so a plan cannot be accepted by this
// process and then rejected by the authoritative parser after side effects.
constexpr std::size_t kMaxJsonBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaxJsonDepth = 64U;
constexpr std::size_t kMaxJsonEvents = 200000U;
constexpr std::size_t kMaxJsonStringBytes = 64U * 1024U;
constexpr std::size_t kMaxJsonContainerItems = 32768U;
constexpr std::size_t kMaxReasonBytes = 1024U;

constexpr auto kDefaultReadinessTimeout = std::chrono::seconds(60);
constexpr auto kMaximumReadinessTimeout = std::chrono::minutes(5);
// The parent knows the authorized recording duration and must supply the
// corresponding EOF deadline explicitly.  A short executable default would
// silently truncate ordinary experiments (including the current 750-second
// protocols), while an unbounded default would defeat supervisor recovery.
constexpr auto kMaximumEofTimeout = std::chrono::hours(24 * 7);
constexpr auto kDefaultPollInterval = std::chrono::milliseconds(20);
constexpr auto kMaximumPollInterval = std::chrono::seconds(5);
constexpr auto kDefaultHeartbeatInterval = std::chrono::seconds(5);
constexpr auto kMaximumHeartbeatInterval = std::chrono::minutes(1);
constexpr auto kDefaultAcceptTimeout = std::chrono::seconds(1);
constexpr auto kMaximumAcceptTimeout = std::chrono::minutes(5);
// This is an idle in-band protocol watchdog, not the per-frame CUDA bound.
// One second is too short for process scheduling and recording-arm gaps; the
// outer EOF deadline and supervisor still bound the complete child lifetime.
constexpr auto kDefaultIpcTimeout = std::chrono::seconds(30);
constexpr auto kMaximumIpcTimeout = std::chrono::minutes(5);
constexpr auto kDefaultVideoProbeTimeout = std::chrono::minutes(1);
constexpr auto kMaximumVideoProbeTimeout = std::chrono::hours(1);

bool fail(std::string* error_out, std::string message)
{
    if (error_out != nullptr) {
        if (message.size() > kMaxReasonBytes) {
            message.resize(kMaxReasonBytes);
        }
        *error_out = std::move(message);
    }
    return false;
}

std::string bounded_reason(std::string value,
                           const char* fallback = "operation failed")
{
    if (value.empty()) {
        value = fallback != nullptr ? fallback : "operation failed";
    }
    if (value.size() > kMaxReasonBytes) {
        value.resize(kMaxReasonBytes);
    }
    for (char& byte : value) {
        const unsigned char unsigned_byte = static_cast<unsigned char>(byte);
        if (unsigned_byte < 0x20U || unsigned_byte == 0x7fU) {
            byte = '?';
        }
    }
    return value;
}

class ScopedFd final {
public:
    explicit ScopedFd(const int fd = -1) noexcept : fd_(fd) {}
    ~ScopedFd()
    {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const noexcept { return fd_; }

private:
    int fd_ = -1;
};

// This is intentionally local to the executable. It prevents a large or
// adversarial plan from reaching nlohmann's DOM parser before depth, event,
// container, string, and duplicate-key bounds have been checked.
class BoundedJsonSax final : public nlohmann::json_sax<json> {
public:
    bool null() override { return event(); }
    bool boolean(bool) override { return event(); }
    bool number_integer(number_integer_t) override { return event(); }
    bool number_unsigned(number_unsigned_t) override { return event(); }
    bool number_float(number_float_t, const string_t& text) override
    {
        return text.size() <= kMaxJsonStringBytes && event();
    }
    bool string(string_t& value) override
    {
        return value.size() <= kMaxJsonStringBytes && event();
    }
    bool binary(binary_t&) override { return false; }

    bool start_object(const std::size_t elements) override
    {
        if (!start_container(elements)) {
            return false;
        }
        stack_.push_back({true, {}});
        return true;
    }

    bool key(string_t& value) override
    {
        if (value.size() > kMaxJsonStringBytes || stack_.empty() ||
            !stack_.back().object || !event() ||
            stack_.back().keys.size() >= kMaxJsonContainerItems) {
            return false;
        }
        return stack_.back().keys.insert(value).second;
    }

    bool end_object() override
    {
        if (stack_.empty() || !stack_.back().object || !event()) {
            return false;
        }
        stack_.pop_back();
        return true;
    }

    bool start_array(const std::size_t elements) override
    {
        if (!start_container(elements)) {
            return false;
        }
        stack_.push_back({false, {}});
        return true;
    }

    bool end_array() override
    {
        if (stack_.empty() || stack_.back().object || !event()) {
            return false;
        }
        stack_.pop_back();
        return true;
    }

    bool parse_error(std::size_t,
                     const std::string&,
                     const nlohmann::detail::exception&) override
    {
        return false;
    }

private:
    struct Container {
        bool object = false;
        std::set<std::string> keys;
    };

    bool event() noexcept
    {
        if (events_ >= kMaxJsonEvents) {
            return false;
        }
        ++events_;
        return true;
    }

    bool start_container(const std::size_t elements) noexcept
    {
        return stack_.size() < kMaxJsonDepth &&
               (elements == static_cast<std::size_t>(-1) ||
                elements <= kMaxJsonContainerItems) &&
               event();
    }

    std::size_t events_ = 0;
    std::vector<Container> stack_;
};

bool read_bounded_json_file(const std::string& path,
                            json* value_out,
                            std::string* error_out)
{
    if (value_out == nullptr) {
        return fail(error_out, "JSON destination is null");
    }
    *value_out = json();
    if (path.empty()) {
        return fail(error_out, "JSON path is empty");
    }
    if (path.size() > 1024U || path.find('\0') != std::string::npos) {
        return fail(error_out, "JSON path exceeds the bounded path limit");
    }

    const int raw_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (raw_fd < 0) {
        return fail(error_out,
                    "failed to open JSON path as a non-symlink regular file");
    }
    ScopedFd fd(raw_fd);

    struct stat status {};
    if (::fstat(fd.get(), &status) != 0 || !S_ISREG(status.st_mode)) {
        return fail(error_out, "JSON path is not a regular file");
    }
    if (status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) > kMaxJsonBytes) {
        return fail(error_out, "JSON file exceeds the bounded size limit");
    }

    std::string bytes;
    bytes.reserve(static_cast<std::size_t>(status.st_size));
    std::array<char, 64U * 1024U> chunk{};
    while (true) {
        const std::size_t remaining = (kMaxJsonBytes + 1U) - bytes.size();
        const std::size_t requested = std::min(remaining, chunk.size());
        const ssize_t count = ::read(fd.get(), chunk.data(), requested);
        if (count > 0) {
            bytes.append(chunk.data(), static_cast<std::size_t>(count));
            if (bytes.size() > kMaxJsonBytes) {
                return fail(error_out, "JSON file grew beyond the bounded size limit");
            }
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        return fail(error_out, "failed while reading bounded JSON file");
    }

    BoundedJsonSax validator;
    if (!json::sax_parse(bytes, &validator)) {
        return fail(error_out,
                    "JSON exceeds structural bounds, contains duplicate keys, or is invalid");
    }
    const json parsed = json::parse(bytes, nullptr, false);
    if (parsed.is_discarded()) {
        return fail(error_out, "JSON file is invalid");
    }
    *value_out = parsed;
    return true;
}

bool is_safe_identifier(const std::string& value)
{
    if (value.empty() || value.size() > 64U) {
        return false;
    }
    const auto is_alnum = [](const unsigned char byte) {
        return (byte >= static_cast<unsigned char>('a') &&
                byte <= static_cast<unsigned char>('z')) ||
               (byte >= static_cast<unsigned char>('A') &&
                byte <= static_cast<unsigned char>('Z')) ||
               (byte >= static_cast<unsigned char>('0') &&
                byte <= static_cast<unsigned char>('9'));
    };
    if (!is_alnum(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [&](const char byte) {
        return is_alnum(static_cast<unsigned char>(byte)) || byte == '_' ||
               byte == '-' || byte == '.';
    });
}

bool parse_nonnegative_u64(const std::string& text,
                           std::uint64_t* value_out,
                           std::string* error_out,
                           const char* name)
{
    if (text.empty() || text.front() == '+') {
        return fail(error_out, std::string(name) + " must be a decimal integer");
    }
    std::uint64_t value = 0;
    for (const unsigned char byte : text) {
        if (byte < static_cast<unsigned char>('0') ||
            byte > static_cast<unsigned char>('9')) {
            return fail(error_out, std::string(name) + " must be a decimal integer");
        }
        const std::uint64_t digit = byte - static_cast<unsigned char>('0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return fail(error_out, std::string(name) + " is too large");
        }
        value = value * 10U + digit;
    }
    if (value_out != nullptr) {
        *value_out = value;
    }
    return true;
}

bool parse_gpu_assignment(const std::string& text,
                          std::string* key_out,
                          int* gpu_out,
                          std::string* error_out,
                          const char* name)
{
    const std::size_t equals = text.find('=');
    const std::size_t colon = text.find(':');
    std::size_t separator = equals;
    if (separator == std::string::npos ||
        (colon != std::string::npos && colon < separator)) {
        separator = colon;
    }
    if (separator == std::string::npos || separator == 0U ||
        separator + 1U >= text.size() ||
        (equals != std::string::npos && colon != std::string::npos)) {
        return fail(error_out,
                    std::string(name) + " must be KEY=GPU (or KEY:GPU)");
    }
    const std::string key = text.substr(0, separator);
    if (!is_safe_identifier(key)) {
        return fail(error_out, std::string(name) + " has an unsafe mapping key");
    }
    std::uint64_t parsed = 0;
    if (!parse_nonnegative_u64(text.substr(separator + 1U), &parsed, error_out,
                               name) ||
        parsed > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return fail(error_out,
                    std::string(name) + " GPU id must fit a nonnegative int");
    }
    if (key_out != nullptr) {
        *key_out = key;
    }
    if (gpu_out != nullptr) {
        *gpu_out = static_cast<int>(parsed);
    }
    return true;
}

template <typename MaximumDuration>
bool parse_duration_ms(const std::string& text,
                       MaximumDuration maximum,
                       std::chrono::milliseconds* value_out,
                       std::string* error_out,
                       const char* name)
{
    std::uint64_t milliseconds = 0;
    const auto maximum_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(maximum);
    if (!parse_nonnegative_u64(text, &milliseconds, error_out, name) ||
        milliseconds == 0U ||
        milliseconds > static_cast<std::uint64_t>(maximum_ms.count())) {
        return fail(error_out,
                    std::string(name) + " must be between 1 and its bounded maximum (ms)");
    }
    if (value_out != nullptr) {
        *value_out = std::chrono::milliseconds(
            static_cast<std::chrono::milliseconds::rep>(milliseconds));
    }
    return true;
}

struct Options final {
    std::string contract_path;
    std::string plan_path;
    std::string expected_recording_root;
    contract::SpatialRoiRecorderRuntimeGpuMapping gpu_mapping;
    pid_t expected_producer_pid = -1;
    uid_t expected_producer_uid = static_cast<uid_t>(-1);
    std::string cpu_affinity;
    std::string cpu_affinity_source;
    std::chrono::milliseconds readiness_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            kDefaultReadinessTimeout);
    std::chrono::milliseconds eof_timeout{0};
    std::chrono::milliseconds poll_interval = kDefaultPollInterval;
    std::chrono::milliseconds heartbeat_interval =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            kDefaultHeartbeatInterval);
    std::chrono::milliseconds accept_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultAcceptTimeout);
    std::chrono::milliseconds ipc_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultIpcTimeout);
    std::chrono::milliseconds video_probe_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            kDefaultVideoProbeTimeout);
    bool show_help = false;
    bool readiness_timeout_seen = false;
    bool eof_timeout_seen = false;
    bool poll_interval_seen = false;
    bool heartbeat_interval_seen = false;
    bool accept_timeout_seen = false;
    bool ipc_timeout_seen = false;
    bool video_probe_timeout_seen = false;
    bool cpu_affinity_seen = false;
    bool cpu_affinity_source_seen = false;
};

void usage(const char* argv0)
{
    std::cerr
        << "Usage: " << argv0
        << " --contract <contract.json> --plan <verified-plan.json>\n"
        << "       --expected-recording-root <absolute-path>\n"
        << "       --analytics-gpu <camera_serial=gpu> (repeatable)\n"
        << "       --recorder-gpu <logical_stream_id=gpu> (repeatable)\n"
        << "       --expected-producer-pid <pid> --expected-producer-uid <uid>\n"
        << "       --eof-timeout-ms N\n"
        << "       [--cpu-affinity CPU-LIST --cpu-affinity-source IDENTIFIER]\n"
        << "       [--readiness-timeout-ms N] [--poll-interval-ms N]\n"
        << "       [--heartbeat-interval-ms N] [--accept-timeout-ms N]\n"
        << "       [--ipc-timeout-ms N] [--video-probe-timeout-ms N]\n"
        << "\n"
        << "Aliases --verified-plan, --recording-root, --producer-pid, and\n"
        << "--producer-uid are accepted; all authority remains explicit CLI input.\n";
}

bool set_unique_string(std::string* destination,
                       const std::string& value,
                       std::string* error_out,
                       const char* name)
{
    if (destination == nullptr || !destination->empty()) {
        return fail(error_out, std::string(name) + " was supplied more than once");
    }
    *destination = value;
    return true;
}

bool parse_options(int argc, char** argv, Options* options, std::string* error_out)
{
    if (options == nullptr) {
        return fail(error_out, "options destination is null");
    }
    *options = Options{};
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] != nullptr ? argv[index] : "";
        const auto require_value = [&](const char* name,
                                       std::string* value_out) -> bool {
            if (index + 1 >= argc || argv[index + 1] == nullptr ||
                !set_unique_string(value_out, argv[index + 1], error_out, name)) {
                if (index + 1 >= argc || argv[index + 1] == nullptr) {
                    return fail(error_out, std::string(name) + " requires a value");
                }
                return false;
            }
            ++index;
            return true;
        };

        if (argument == "--help" || argument == "-h") {
            usage(argv[0]);
            options->show_help = true;
            return false;
        }
        if (argument == "--contract") {
            if (!require_value("--contract", &options->contract_path)) return false;
        } else if (argument == "--plan" || argument == "--verified-plan") {
            if (!require_value(argument.c_str(), &options->plan_path)) return false;
        } else if (argument == "--expected-recording-root" ||
                   argument == "--recording-root") {
            if (!require_value(argument.c_str(),
                               &options->expected_recording_root)) {
                return false;
            }
        } else if (argument == "--analytics-gpu" ||
                   argument == "--analytics-gpu-by-camera-serial" ||
                   argument == "--recorder-gpu" ||
                   argument == "--recorder-gpu-by-logical-stream-id") {
            if (index + 1 >= argc || argv[index + 1] == nullptr) {
                return fail(error_out, argument + " requires KEY=GPU");
            }
            std::string key;
            int gpu = -1;
            if (!parse_gpu_assignment(argv[index + 1], &key, &gpu, error_out,
                                      argument.c_str())) {
                return false;
            }
            ++index;
            const bool analytics_mapping =
                argument == "--analytics-gpu" ||
                argument == "--analytics-gpu-by-camera-serial";
            auto& destination = analytics_mapping
                                    ? options->gpu_mapping
                                          .analytics_gpu_by_camera_serial
                                    : options->gpu_mapping
                                          .recorder_gpu_by_logical_stream_id;
            if (!destination.emplace(key, gpu).second) {
                return fail(error_out, argument + " repeats mapping key " + key);
            }
        } else if (argument == "--expected-producer-pid" ||
                   argument == "--producer-pid") {
            if (index + 1 >= argc || argv[index + 1] == nullptr) {
                return fail(error_out, argument + " requires a value");
            }
            std::uint64_t value = 0;
            if (!parse_nonnegative_u64(argv[++index], &value, error_out,
                                       argument.c_str()) ||
                value == 0U ||
                value > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()) ||
                options->expected_producer_pid > 0) {
                return fail(error_out,
                            argument + " must be supplied exactly once as a positive PID");
            }
            options->expected_producer_pid = static_cast<pid_t>(value);
        } else if (argument == "--expected-producer-uid" ||
                   argument == "--producer-uid") {
            if (index + 1 >= argc || argv[index + 1] == nullptr) {
                return fail(error_out, argument + " requires a value");
            }
            std::uint64_t value = 0;
            if (!parse_nonnegative_u64(argv[++index], &value, error_out,
                                       argument.c_str()) ||
                value > static_cast<std::uint64_t>(std::numeric_limits<uid_t>::max()) ||
                options->expected_producer_uid != static_cast<uid_t>(-1)) {
                return fail(error_out,
                            argument + " must be supplied exactly once as a UID");
            }
            options->expected_producer_uid = static_cast<uid_t>(value);
        } else if (argument == "--cpu-affinity") {
            if (options->cpu_affinity_seen ||
                !require_value("--cpu-affinity", &options->cpu_affinity)) {
                return fail(error_out,
                            options->cpu_affinity_seen
                                ? "--cpu-affinity was supplied more than once"
                                : (error_out ? *error_out
                                             : "--cpu-affinity requires a value"));
            }
            options->cpu_affinity_seen = true;
        } else if (argument == "--cpu-affinity-source") {
            if (options->cpu_affinity_source_seen ||
                !require_value("--cpu-affinity-source",
                               &options->cpu_affinity_source)) {
                return fail(error_out,
                            options->cpu_affinity_source_seen
                                ? "--cpu-affinity-source was supplied more than once"
                                : (error_out ? *error_out
                                             : "--cpu-affinity-source requires a value"));
            }
            options->cpu_affinity_source_seen = true;
        } else if (argument == "--readiness-timeout-ms") {
            if (options->readiness_timeout_seen) {
                return fail(error_out, "--readiness-timeout-ms was supplied more than once");
            }
            if (index + 1 >= argc) {
                return fail(error_out, "--readiness-timeout-ms requires a value");
            }
            ++index;
            if (!parse_duration_ms(argv[index], kMaximumReadinessTimeout,
                                   &options->readiness_timeout, error_out,
                                   "--readiness-timeout-ms")) {
                return false;
            }
            options->readiness_timeout_seen = true;
        } else if (argument == "--eof-timeout-ms") {
            if (options->eof_timeout_seen) {
                return fail(error_out, "--eof-timeout-ms was supplied more than once");
            }
            if (index + 1 >= argc) {
                return fail(error_out, "--eof-timeout-ms requires a value");
            }
            ++index;
            if (!parse_duration_ms(argv[index], kMaximumEofTimeout,
                                   &options->eof_timeout, error_out,
                                   "--eof-timeout-ms")) {
                return false;
            }
            options->eof_timeout_seen = true;
        } else if (argument == "--poll-interval-ms") {
            if (options->poll_interval_seen) {
                return fail(error_out, "--poll-interval-ms was supplied more than once");
            }
            if (index + 1 >= argc) {
                return fail(error_out, "--poll-interval-ms requires a value");
            }
            ++index;
            if (!parse_duration_ms(argv[index], kMaximumPollInterval,
                                   &options->poll_interval, error_out,
                                   "--poll-interval-ms")) {
                return false;
            }
            options->poll_interval_seen = true;
        } else if (argument == "--heartbeat-interval-ms") {
            if (options->heartbeat_interval_seen) {
                return fail(error_out,
                            "--heartbeat-interval-ms was supplied more than once");
            }
            if (index + 1 >= argc) {
                return fail(error_out,
                            "--heartbeat-interval-ms requires a value");
            }
            ++index;
            if (!parse_duration_ms(argv[index], kMaximumHeartbeatInterval,
                                   &options->heartbeat_interval, error_out,
                                   "--heartbeat-interval-ms")) {
                return false;
            }
            options->heartbeat_interval_seen = true;
        } else if (argument == "--accept-timeout-ms") {
            if (options->accept_timeout_seen) {
                return fail(error_out, "--accept-timeout-ms was supplied more than once");
            }
            if (index + 1 >= argc) {
                return fail(error_out, "--accept-timeout-ms requires a value");
            }
            ++index;
            if (!parse_duration_ms(argv[index], kMaximumAcceptTimeout,
                                   &options->accept_timeout, error_out,
                                   "--accept-timeout-ms")) {
                return false;
            }
            options->accept_timeout_seen = true;
        } else if (argument == "--ipc-timeout-ms") {
            if (options->ipc_timeout_seen) {
                return fail(error_out, "--ipc-timeout-ms was supplied more than once");
            }
            if (index + 1 >= argc) {
                return fail(error_out, "--ipc-timeout-ms requires a value");
            }
            ++index;
            if (!parse_duration_ms(argv[index], kMaximumIpcTimeout,
                                   &options->ipc_timeout, error_out,
                                   "--ipc-timeout-ms")) {
                return false;
            }
            options->ipc_timeout_seen = true;
        } else if (argument == "--video-probe-timeout-ms") {
            if (options->video_probe_timeout_seen) {
                return fail(error_out,
                            "--video-probe-timeout-ms was supplied more than once");
            }
            if (index + 1 >= argc) {
                return fail(error_out, "--video-probe-timeout-ms requires a value");
            }
            ++index;
            if (!parse_duration_ms(argv[index], kMaximumVideoProbeTimeout,
                                   &options->video_probe_timeout, error_out,
                                   "--video-probe-timeout-ms")) {
                return false;
            }
            options->video_probe_timeout_seen = true;
        } else {
            return fail(error_out, "unknown argument: " + argument);
        }
    }

    if (options->contract_path.empty() || options->plan_path.empty() ||
        options->expected_recording_root.empty() ||
        !options->eof_timeout_seen ||
        options->expected_producer_pid <= 0 ||
        options->expected_producer_uid == static_cast<uid_t>(-1) ||
        options->gpu_mapping.analytics_gpu_by_camera_serial.empty() ||
        options->gpu_mapping.recorder_gpu_by_logical_stream_id.empty()) {
        return fail(error_out,
                    "contract, verified plan, expected root, GPU mapping, producer PID/UID, and EOF timeout are required");
    }
    if (options->cpu_affinity_seen != options->cpu_affinity_source_seen) {
        return fail(error_out,
                    "--cpu-affinity and --cpu-affinity-source must be supplied together");
    }
    if (options->cpu_affinity_seen) {
        recording::SpatialRoiRecorderCpuList parsed;
        std::string affinity_error;
        if (!is_safe_identifier(options->cpu_affinity_source)) {
            return fail(error_out,
                        "invalid recorder CPU affinity authority source");
        }
        if (!recording::parse_spatial_roi_recorder_cpu_list(
                options->cpu_affinity, &parsed, &affinity_error)) {
            return fail(error_out,
                        "invalid recorder CPU affinity: " + affinity_error);
        }
        options->cpu_affinity = std::move(parsed.canonical);
    }
    return true;
}

json owner_snapshot_to_json(
    const recording::SpatialRoiCameraRecorderSnapshot& snapshot,
    const std::string& event,
    const json& scheduling,
    const std::string& terminal_reason = {})
{
    json result = recording::spatial_roi_camera_recorder_snapshot_to_json(
        snapshot, event, terminal_reason);
    result["scheduling"] = scheduling;
    return result;
}

json failure_status(const std::string& reason,
                    const json& storage_preflight = json(),
                    const json& scheduling = json())
{
    json result = {
        {"program", "spatial_roi_camera_recorder"},
        {"event", "terminal"},
        {"status", "failed"},
        {"state", "failed"},
        {"completed", false},
        {"error", bounded_reason(reason)},
    };
    if (!storage_preflight.is_null() && !storage_preflight.empty()) {
        result["storage_preflight"] = storage_preflight;
    }
    if (!scheduling.is_null() && !scheduling.empty()) {
        result["scheduling"] = scheduling;
    }
    return result;
}

void print_status(const json& status)
{
    std::cout << status.dump() << '\n' << std::flush;
}

template <typename Duration>
bool poll_until_ready(recording::SpatialRoiCameraRecorder* owner,
                      Duration timeout,
                      std::chrono::milliseconds poll_interval,
                      std::chrono::milliseconds heartbeat_interval,
                      const json& scheduling,
                      std::string* error_out)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto next_heartbeat =
        std::chrono::steady_clock::now() + heartbeat_interval;
    while (true) {
        std::string poll_error;
        if (!owner->PollReadiness(&poll_error)) {
            return fail(error_out,
                        poll_error.empty() ? "camera recorder readiness failed"
                                            : poll_error);
        }
        if (owner->ready()) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_heartbeat) {
            print_status(owner_snapshot_to_json(
                owner->snapshot(), "heartbeat", scheduling));
            next_heartbeat = now + heartbeat_interval;
        }
        if (now >= deadline) {
            return fail(error_out, "camera recorder readiness deadline expired");
        }
        const auto remaining = deadline - now;
        const auto sleep_for = std::min(
            poll_interval,
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
        if (sleep_for.count() > 0) {
            std::this_thread::sleep_for(sleep_for);
        }
    }
}

template <typename Duration>
bool poll_until_eof(recording::SpatialRoiCameraRecorder* owner,
                    Duration timeout,
                    std::chrono::milliseconds poll_interval,
                    std::chrono::milliseconds heartbeat_interval,
                    const json& scheduling,
                    std::string* error_out)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto next_heartbeat =
        std::chrono::steady_clock::now() + heartbeat_interval;
    while (true) {
        std::string poll_error;
        if (!owner->PollEof(&poll_error)) {
            return fail(error_out,
                        poll_error.empty() ? "camera recorder EOF failed"
                                            : poll_error);
        }
        const auto snapshot = owner->snapshot();
        if (snapshot.clean_eof) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_heartbeat) {
            print_status(owner_snapshot_to_json(
                snapshot, "heartbeat", scheduling));
            next_heartbeat = now + heartbeat_interval;
        }
        if (now >= deadline) {
            return fail(error_out, "camera recorder EOF deadline expired");
        }
        const auto remaining = deadline - now;
        const auto sleep_for = std::min(
            poll_interval,
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
        if (sleep_for.count() > 0) {
            std::this_thread::sleep_for(sleep_for);
        }
    }
}

int run_recorder(const Options& options)
{
    recording::SpatialRoiRecorderSchedulingSnapshot scheduling_snapshot;
    std::string scheduling_error;
    if (!recording::initialize_spatial_roi_recorder_scheduling(
            options.cpu_affinity,
            options.cpu_affinity_source,
            &scheduling_snapshot,
            &scheduling_error)) {
        const json scheduling =
            recording::spatial_roi_recorder_scheduling_to_json(
                scheduling_snapshot);
        print_status(failure_status(
            "recorder scheduling initialization failed: " +
                bounded_reason(scheduling_error),
            json(),
            scheduling));
        return 2;
    }
    const json scheduling =
        recording::spatial_roi_recorder_scheduling_to_json(
            scheduling_snapshot);

    json candidate_contract;
    json verified_plan;
    std::string error;
    if (!read_bounded_json_file(options.contract_path, &candidate_contract,
                                &error) ||
        !read_bounded_json_file(options.plan_path, &verified_plan, &error)) {
        print_status(failure_status(error, json(), scheduling));
        return 2;
    }

    // Verify the plan independently before the camera-level parser can use it
    // as authority. The camera parser then rebuilds the complete contract and
    // compares the candidate byte-for-value against this plan/root/mapping.
    contract::SpatialRoiRecordingPlan parsed_plan;
    if (!contract::parse_verified_plan(verified_plan, &parsed_plan, &error)) {
        print_status(failure_status("verified plan authentication failed: " +
                                   bounded_reason(error), json(), scheduling));
        return 2;
    }

    contract::SpatialRoiRecorderCameraContractView camera_contract;
    if (!contract::parse_spatial_roi_recorder_camera_contract(
            candidate_contract, verified_plan, options.expected_recording_root,
            options.gpu_mapping, &camera_contract, &error)) {
        print_status(failure_status("camera contract authentication failed: " +
                                   bounded_reason(error), json(), scheduling));
        return 2;
    }
    if (parsed_plan.plan_sha256 != camera_contract.spatial_roi_plan_sha256 ||
        camera_contract.stream_count != 4U || camera_contract.streams.size() != 4U ||
        camera_contract.stream_order.size() != 4U) {
        print_status(failure_status(
            "verified plan and camera contract do not describe exactly four fixed-region streams",
            json(), scheduling));
        return 2;
    }

    std::vector<recording::SpatialRoiRecorderEvidenceBinding> bindings;
    bindings.reserve(camera_contract.stream_order.size());
    std::vector<std::string> allowed_artifacts;
    std::set<std::string> unique_artifacts;
    for (const std::string& logical_stream_id : camera_contract.stream_order) {
        recording::SpatialRoiRecorderEvidenceBinding binding;
        if (!recording::make_spatial_roi_recorder_evidence_binding(
                candidate_contract, verified_plan, options.expected_recording_root,
                options.gpu_mapping, logical_stream_id, &binding, &error)) {
            print_status(failure_status(
                "evidence binding authentication failed: " + bounded_reason(error),
                json(), scheduling));
            return 2;
        }
        for (const auto& artifact : binding.expected_artifacts) {
            if (unique_artifacts.insert(artifact.second).second) {
                allowed_artifacts.push_back(artifact.second);
            }
        }
        bindings.push_back(std::move(binding));
    }
    if (bindings.size() != 4U || allowed_artifacts.empty()) {
        print_status(failure_status(
            "authenticated evidence binding set is incomplete", json(), scheduling));
        return 2;
    }

    std::unique_ptr<recording::SpatialRoiRecorderArtifactRoot> artifact_root;
    if (!recording::SpatialRoiRecorderArtifactRoot::Open(
            options.expected_recording_root, allowed_artifacts, &artifact_root,
            &error) || !artifact_root) {
        print_status(failure_status("artifact root authentication failed: " +
                                   bounded_reason(error), json(), scheduling));
        return 2;
    }
    std::shared_ptr<recording::SpatialRoiRecorderArtifactRoot> artifact_authority(
        std::move(artifact_root));

    // This is the live readiness gate.  The filesystem observation is bound
    // to the retained artifact-root descriptor; no pathname lookup is used
    // and the authenticated aggregate media/evidence budgets are charged in
    // full alongside the explicit nonzero reserve policy.
    recording::SpatialRoiRecorderStoragePreflightPolicy storage_policy;
    storage_policy.schema_id = camera_contract.storage_preflight_policy.schema_id;
    storage_policy.schema_version =
        camera_contract.storage_preflight_policy.schema_version;
    storage_policy.required = camera_contract.storage_preflight_policy.required;
    storage_policy.reserved_free_bytes =
        camera_contract.storage_preflight_policy.reserved_free_bytes;
    recording::SpatialRoiRecorderStoragePreflightResult storage_preflight;
    if (!recording::run_spatial_roi_recorder_storage_preflight(
            *artifact_authority,
            camera_contract.aggregate_bounds.max_media_bytes_total,
            camera_contract.aggregate_bounds.max_evidence_bytes_total,
            storage_policy,
            &storage_preflight,
            {},
            &error)) {
        print_status(failure_status(
            "storage preflight failed: " + bounded_reason(error),
            recording::spatial_roi_recorder_storage_preflight_to_json(
                storage_preflight),
            scheduling));
        return 2;
    }

    ipc::SpatialRoiSocketRuntimeDirectoryConfig runtime_config;
    runtime_config.recording_identity_token =
        camera_contract.recording_identity_token;
    runtime_config.logical_stream_ids = camera_contract.stream_order;
    std::string runtime_error;
    auto runtime_directory = ipc::SpatialRoiSocketRuntimeDirectory::Create(
        runtime_config, &runtime_error);
    if (!runtime_directory) {
        print_status(failure_status("socket runtime directory creation failed: " +
                                   bounded_reason(runtime_error), json(), scheduling));
        return 2;
    }
    std::shared_ptr<ipc::SpatialRoiSocketRuntimeDirectory> runtime_authority(
        std::move(runtime_directory));

    // This is the sole production factory. Every core receives the same
    // descriptor-backed authorities and the exact plan-order binding; no
    // full-frame recorder or legacy external-recorder path is involved.
    const auto factory =
        [bindings, artifact_authority, runtime_authority,
         expected_pid = options.expected_producer_pid,
         expected_uid = options.expected_producer_uid,
         accept_timeout = options.accept_timeout,
         ipc_timeout = options.ipc_timeout,
         video_probe_timeout = options.video_probe_timeout](
            const contract::SpatialRoiRecorderStreamView& stream,
            const std::size_t plan_index,
            std::string* factory_error)
        -> std::unique_ptr<recording::SpatialRoiCameraRecorderStreamCore> {
        if (plan_index >= bindings.size() ||
            bindings[plan_index].logical_stream_id != stream.logical_stream_id) {
            fail(factory_error, "factory stream does not match authenticated plan order");
            return nullptr;
        }
        recording::SpatialRoiCameraRecorderStreamCoreConfig config;
        config.stream = stream;
        config.evidence_binding = bindings[plan_index];
        config.artifact_root = artifact_authority;
        config.runtime_directory = runtime_authority;
        config.expected_producer_pid = expected_pid;
        config.expected_producer_uid = expected_uid;
        config.accept_timeout = accept_timeout;
        config.ipc_response_timeout = ipc_timeout;
        config.video_probe_timeout = video_probe_timeout;
        return recording::SpatialRoiConcreteCameraRecorderStreamCore::Create(
            std::move(config), factory_error);
    };

    auto owner = recording::SpatialRoiCameraRecorder::Create(
        candidate_contract, verified_plan, options.expected_recording_root,
        options.gpu_mapping, factory, &error);
    if (!owner) {
        print_status(failure_status("camera recorder construction failed: " +
                                   bounded_reason(error),
                                   recording::spatial_roi_recorder_storage_preflight_to_json(
                                       storage_preflight),
                                   scheduling));
        return 2;
    }
    owner->set_storage_preflight_result(std::move(storage_preflight));

    auto emit_owner_failure = [&](const std::string& reason, const int code) {
        auto snapshot = owner->snapshot();
        json status = owner_snapshot_to_json(
            snapshot, "terminal", scheduling, reason);
        if (status.value("first_failure", std::string()).empty()) {
            status["first_failure"] = bounded_reason(reason);
        }
        // Failure reporting must not wait behind a potentially stuck CUDA
        // teardown. The outer supervisor owns the process-exit deadline.
        print_status(status);
        owner.reset();
        return code;
    };

    if (!owner->Start(&error)) {
        return emit_owner_failure(error.empty() ? "camera recorder start failed" : error,
                                  1);
    }
    if (!poll_until_ready(owner.get(), options.readiness_timeout,
                          options.poll_interval, options.heartbeat_interval,
                          scheduling,
                          &error)) {
        return emit_owner_failure(error, 1);
    }
    print_status(owner_snapshot_to_json(
        owner->snapshot(), "ready", scheduling));
    if (!poll_until_eof(owner.get(), options.eof_timeout, options.poll_interval,
                        options.heartbeat_interval, scheduling, &error)) {
        return emit_owner_failure(error, 1);
    }
    if (!owner->DrainAndFinalize(&error)) {
        return emit_owner_failure(
            error.empty() ? "camera recorder drain/finalize failed" : error, 1);
    }

    const auto snapshot = owner->snapshot();
    const json status = owner_snapshot_to_json(
        snapshot, "terminal", scheduling);
    print_status(status);
    owner.reset();
    return snapshot.completed ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        Options options;
        std::string error;
        if (!parse_options(argc, argv, &options, &error)) {
            if (options.show_help) {
                return 0;
            }
            if (!error.empty()) {
                print_status(failure_status(error));
            }
            return 2;
        }
        return run_recorder(options);
    } catch (const std::exception& exception) {
        print_status(failure_status(std::string("fatal recorder exception: ") +
                                    exception.what()));
        return 2;
    } catch (...) {
        print_status(failure_status("fatal recorder exception: unknown exception"));
        return 2;
    }
}
