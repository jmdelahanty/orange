#include "spatial_roi_camera_recorder_process.h"

#include "session/spatial_roi_recorder_camera_contract.h"
#include "session/spatial_roi_recording_config.h"
#include "spatial_roi_recorder_scheduling.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <initializer_list>
#include <limits>
#include <poll.h>
#include <sched.h>
#include <set>
#include <signal.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace orange::spatial_roi::recording {
namespace {

using json = nlohmann::json;
namespace contract = session::spatial_roi;

constexpr std::size_t kMaxJsonBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaxJsonDepth = 64U;
constexpr std::size_t kMaxJsonEvents = 200000U;
constexpr std::size_t kMaxJsonStringBytes = 64U * 1024U;
constexpr std::size_t kMaxJsonContainerItems = 32768U;
constexpr std::size_t kMaxPathBytes = 4096U;
constexpr std::size_t kMaxReasonBytes = 1024U;
constexpr auto kMaxChildEofTimeout = std::chrono::hours(24 * 7);
constexpr auto kMaxChildReadinessTimeout = std::chrono::minutes(5);
constexpr auto kMaxChildPollInterval = std::chrono::seconds(5);
constexpr auto kMaxChildHeartbeatInterval = std::chrono::minutes(1);
constexpr auto kMaxChildAcceptTimeout = std::chrono::minutes(5);
constexpr auto kMaxChildIpcTimeout = std::chrono::minutes(5);
constexpr auto kMaxChildVideoProbeTimeout = std::chrono::hours(1);
constexpr auto kMaxSupervisorWait = std::chrono::hours(24 * 7);
constexpr auto kMaxSupervisorPollInterval = std::chrono::seconds(5);
constexpr auto kMaxSupervisorKillWait = std::chrono::minutes(5);

std::string bounded_reason(std::string value,
                           const char* fallback = "operation failed")
{
    if (value.empty()) {
        value = fallback ? fallback : "operation failed";
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

bool fail(std::string* error_out, std::string message)
{
    if (error_out) {
        *error_out = bounded_reason(std::move(message));
    }
    return false;
}

std::string errno_message(const char* operation, const int error_number)
{
    std::string result = operation ? operation : "process operation";
    result += " failed: ";
    const char* text = std::strerror(error_number);
    if (text) result += text;
    return result;
}

bool safe_absolute_path(const std::string& value,
                        const bool allow_root,
                        const char* name,
                        std::string* error_out)
{
    if (value.empty() || value.size() > kMaxPathBytes ||
        value.find('\0') != std::string::npos) {
        return fail(error_out,
                    std::string(name) + " must be a bounded path without NUL");
    }
    try {
        const std::filesystem::path path(value);
        if (!path.is_absolute() || (!allow_root && path == path.root_path())) {
            return fail(error_out,
                        std::string(name) + " must be an absolute non-root path");
        }
        for (const auto& component : path) {
            if (component == std::filesystem::path(".") ||
                component == std::filesystem::path("..")) {
                return fail(error_out,
                            std::string(name) + " contains an unsafe dot component");
            }
        }
    } catch (const std::exception& exception) {
        return fail(error_out,
                    std::string(name) + " path parsing failed: " + exception.what());
    } catch (...) {
        return fail(error_out, std::string(name) + " path parsing failed");
    }
    return true;
}

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
        if (!start_container(elements)) return false;
        stack_.push_back({true, {}});
        return true;
    }

    bool key(string_t& value) override
    {
        if (stack_.empty() || !stack_.back().object ||
            value.size() > kMaxJsonStringBytes ||
            stack_.back().keys.size() >= kMaxJsonContainerItems || !event()) {
            return false;
        }
        return stack_.back().keys.insert(value).second;
    }

    bool end_object() override
    {
        if (stack_.empty() || !stack_.back().object || !event()) return false;
        stack_.pop_back();
        return true;
    }

    bool start_array(const std::size_t elements) override
    {
        if (!start_container(elements)) return false;
        stack_.push_back({false, {}});
        return true;
    }

    bool end_array() override
    {
        if (stack_.empty() || stack_.back().object || !event()) return false;
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
        if (events_ >= kMaxJsonEvents) return false;
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
    if (!value_out) return fail(error_out, "JSON destination is null");
    *value_out = json();
    if (!safe_absolute_path(path, false, "JSON path", error_out)) return false;

    const int raw_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (raw_fd < 0) {
        return fail(error_out,
                    "JSON path could not be opened as a non-symlink file");
    }
    struct ScopedFd final {
        int fd;
        ~ScopedFd() { if (fd >= 0) (void)::close(fd); }
    } fd{raw_fd};

    struct stat status {};
    if (::fstat(fd.fd, &status) != 0 || !S_ISREG(status.st_mode)) {
        return fail(error_out, "JSON path is not a regular file");
    }
    if (status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) > kMaxJsonBytes) {
        return fail(error_out, "JSON file exceeds the bounded size limit");
    }

    std::string bytes;
    try {
        bytes.reserve(static_cast<std::size_t>(status.st_size));
    } catch (const std::exception& exception) {
        return fail(error_out,
                    std::string("JSON buffer allocation failed: ") + exception.what());
    }
    std::array<char, 64U * 1024U> chunk{};
    while (true) {
        const std::size_t remaining = (kMaxJsonBytes + 1U) - bytes.size();
        const std::size_t requested = std::min(remaining, chunk.size());
        const ssize_t count = ::read(fd.fd, chunk.data(), requested);
        if (count > 0) {
            try {
                bytes.append(chunk.data(), static_cast<std::size_t>(count));
            } catch (const std::exception& exception) {
                return fail(error_out,
                            std::string("JSON buffer allocation failed: ") +
                                exception.what());
            }
            if (bytes.size() > kMaxJsonBytes) {
                return fail(error_out, "JSON file grew beyond the bounded size limit");
            }
            continue;
        }
        if (count == 0) break;
        if (errno == EINTR) continue;
        return fail(error_out, errno_message("read JSON file", errno));
    }

    BoundedJsonSax validator;
    if (!json::sax_parse(bytes, &validator)) {
        return fail(error_out,
                    "JSON exceeds structural bounds, contains duplicate keys, or is invalid");
    }
    const json parsed = json::parse(bytes, nullptr, false);
    if (parsed.is_discarded()) return fail(error_out, "JSON file is invalid");
    *value_out = parsed;
    return true;
}

bool valid_duration(const std::chrono::milliseconds value,
                    const std::chrono::milliseconds maximum)
{
    return value.count() > 0 && value <= maximum;
}

bool safe_identifier(const std::string& value)
{
    if (value.empty() || value.size() > 64U) return false;
    const auto alnum = [](const unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') ||
               (byte >= 'A' && byte <= 'Z') ||
               (byte >= '0' && byte <= '9');
    };
    if (!alnum(static_cast<unsigned char>(value.front()))) return false;
    return std::all_of(value.begin() + 1, value.end(), [&](const char byte) {
        return alnum(static_cast<unsigned char>(byte)) || byte == '_' ||
               byte == '-' || byte == '.';
    });
}

std::string decimal(const std::uint64_t value)
{
    return std::to_string(value);
}

std::vector<char*> mutable_argv(const std::vector<std::string>& argv)
{
    std::vector<char*> result;
    result.reserve(argv.size() + 1U);
    for (const std::string& item : argv) {
        result.push_back(const_cast<char*>(item.c_str()));
    }
    result.push_back(nullptr);
    return result;
}

std::chrono::steady_clock::time_point deadline_after(
    const std::chrono::milliseconds timeout)
{
    return std::chrono::steady_clock::now() + timeout;
}

int poll_timeout_ms(const std::chrono::steady_clock::time_point deadline) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    const auto remaining = deadline - now;
    auto value = std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
                     .count();
    if (remaining > std::chrono::milliseconds(value)) ++value;
    if (value <= 0) return 1;
    if (value > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(value);
}

bool stat_is_owned_directory(const struct stat& status)
{
    return S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() &&
           (status.st_mode & 0777) == 0700;
}

bool stat_is_owned_socket(const struct stat& status)
{
    return S_ISSOCK(status.st_mode) && status.st_uid == ::geteuid() &&
           (status.st_mode & 0777) == 0600;
}

bool checked_add_u64(const std::uint64_t left,
                     const std::uint64_t right,
                     std::uint64_t* result)
{
    if (!result || right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    *result = left + right;
    return true;
}

bool checked_multiply_u64(const std::uint64_t left,
                          const std::uint64_t right,
                          std::uint64_t* result)
{
    if (!result || (left != 0 &&
                    right > std::numeric_limits<std::uint64_t>::max() / left)) {
        return false;
    }
    *result = left * right;
    return true;
}

bool exact_object_keys(const json& value,
                       const std::initializer_list<const char*> keys,
                       const char* path,
                       std::string* error_out)
{
    if (!value.is_object()) {
        return fail(error_out,
                    std::string(path ? path : "object") + " must be an object");
    }
    std::set<std::string> expected;
    for (const char* key : keys) {
        if (key) expected.emplace(key);
    }
    if (value.size() != expected.size()) {
        return fail(error_out,
                    std::string(path ? path : "object") +
                        " does not have the exact closed key set");
    }
    for (const auto& item : value.items()) {
        if (expected.find(item.key()) == expected.end()) {
            return fail(error_out,
                        std::string(path ? path : "object") +
                            " contains an unexpected key: " + item.key());
        }
    }
    return true;
}

bool read_unsigned_u64(const json& value,
                       const char* key,
                       const char* path,
                       std::uint64_t* output,
                       std::string* error_out)
{
    if (!output || !value.is_object() || !value.contains(key) ||
        !value.at(key).is_number_unsigned()) {
        return fail(error_out,
                    std::string(path ? path : "object") + "." + key +
                        " must be an unsigned integer");
    }
    try {
        *output = value.at(key).get<std::uint64_t>();
    } catch (...) {
        return fail(error_out,
                    std::string(path ? path : "object") + "." + key +
                        " is outside the uint64 range");
    }
    return true;
}

bool exact_unsigned_u64(const json& value,
                        const char* key,
                        const std::uint64_t expected,
                        const char* path,
                        std::string* error_out)
{
    std::uint64_t actual = 0;
    return read_unsigned_u64(value, key, path, &actual, error_out) &&
           (actual == expected ||
            fail(error_out,
                 std::string(path ? path : "object") + "." + key +
                     " does not match the authenticated value"));
}

bool exact_boolean(const json& value,
                   const char* key,
                   const bool expected,
                   const char* path,
                   std::string* error_out)
{
    if (!value.is_object() || !value.contains(key) ||
        !value.at(key).is_boolean()) {
        return fail(error_out,
                    std::string(path ? path : "object") + "." + key +
                        " must be a boolean");
    }
    return value.at(key).get<bool>() == expected ||
           fail(error_out,
                std::string(path ? path : "object") + "." + key +
                    " does not match the authenticated value");
}

bool exact_string(const json& value,
                  const char* key,
                  const std::string& expected,
                  const char* path,
                  std::string* error_out)
{
    if (!value.is_object() || !value.contains(key) ||
        !value.at(key).is_string()) {
        return fail(error_out,
                    std::string(path ? path : "object") + "." + key +
                        " must be a string");
    }
    return value.at(key).get<std::string>() == expected ||
           fail(error_out,
                std::string(path ? path : "object") + "." + key +
                    " does not match the authenticated value");
}

bool exact_schema_version(const json& value,
                          const char* key,
                          const int expected,
                          const char* path,
                          std::string* error_out)
{
    if (!value.is_object() || !value.contains(key) ||
        (!value.at(key).is_number_integer() &&
         !value.at(key).is_number_unsigned())) {
        return fail(error_out,
                    std::string(path ? path : "object") + "." + key +
                        " must be an integer");
    }
    try {
        const std::int64_t actual = value.at(key).get<std::int64_t>();
        return actual == expected ||
               fail(error_out,
                    std::string(path ? path : "object") + "." + key +
                        " does not match the authenticated schema version");
    } catch (...) {
        return fail(error_out,
                    std::string(path ? path : "object") + "." + key +
                        " is outside the signed integer range");
    }
}

}  // namespace

const char* spatial_roi_camera_recorder_process_state_name(
    const SpatialRoiCameraRecorderProcessState state) noexcept
{
    switch (state) {
    case SpatialRoiCameraRecorderProcessState::kConstructed:
        return "constructed";
    case SpatialRoiCameraRecorderProcessState::kStarting:
        return "starting";
    case SpatialRoiCameraRecorderProcessState::kSocketsBound:
        return "sockets_bound";
    case SpatialRoiCameraRecorderProcessState::kReady:
        return "ready";
    case SpatialRoiCameraRecorderProcessState::kExited:
        return "exited";
    case SpatialRoiCameraRecorderProcessState::kFailed:
        return "failed";
    case SpatialRoiCameraRecorderProcessState::kStopped:
        return "stopped";
    }
    return "unknown";
}

SpatialRoiCameraRecorderProcess::SpatialRoiCameraRecorderProcess(
    SpatialRoiCameraRecorderProcessConfig config)
    : config_(std::move(config))
{
}

SpatialRoiCameraRecorderProcess::~SpatialRoiCameraRecorderProcess()
{
    try {
        (void)Stop(nullptr);
    } catch (...) {
        // Destruction is best effort.  The documented outer supervisor must
        // still bound the parent if a child is stuck in an uninterruptible
        // CUDA/destructor path.
    }
    if (stdout_fd_ >= 0) (void)::close(std::exchange(stdout_fd_, -1));
    if (runtime_directory_fd_ >= 0) {
        (void)::close(std::exchange(runtime_directory_fd_, -1));
    }
    if (executable_fd_ >= 0) {
        (void)::close(std::exchange(executable_fd_, -1));
    }
}

std::unique_ptr<SpatialRoiCameraRecorderProcess>
SpatialRoiCameraRecorderProcess::Create(
    SpatialRoiCameraRecorderProcessConfig config,
    std::string* error_out)
{
    if (error_out) error_out->clear();
    try {
        return std::unique_ptr<SpatialRoiCameraRecorderProcess>(
            new SpatialRoiCameraRecorderProcess(std::move(config)));
    } catch (const std::exception& exception) {
        fail(error_out,
             std::string("camera recorder process construction failed: ") +
                 exception.what());
    } catch (...) {
        fail(error_out, "camera recorder process construction failed");
    }
    return nullptr;
}

bool SpatialRoiCameraRecorderProcess::set_error(
    std::string* error_out,
    const std::string& message) const
{
    if (error_out) *error_out = bounded_reason(message);
    return false;
}

bool SpatialRoiCameraRecorderProcess::authenticate(std::string* error_out)
{
    if (!safe_absolute_path(config_.contract_path,
                            false,
                            "contract path",
                            error_out) ||
        !safe_absolute_path(config_.verified_plan_path,
                            false,
                            "verified plan path",
                            error_out) ||
        !safe_absolute_path(config_.expected_recording_root,
                            false,
                            "recording root",
                            error_out) ||
        !safe_absolute_path(config_.recorder_executable,
                            false,
                            "recorder executable",
                            error_out)) {
        return false;
    }
    if (config_.expected_producer_pid <= 0 ||
        config_.expected_producer_uid == static_cast<uid_t>(-1)) {
        return fail(error_out,
                    "expected producer PID and UID must be explicit and positive");
    }
    if (config_.expected_artifact_root_identity_available &&
        config_.expected_artifact_root_identity.inode == 0) {
        return fail(error_out,
                    "expected artifact-root identity must contain a nonzero inode");
    }
    if (config_.cpu_affinity.empty() != config_.cpu_affinity_source.empty()) {
        return fail(error_out,
                    "recorder CPU affinity and its authority source must be supplied together");
    }
    if (!config_.cpu_affinity.empty()) {
        SpatialRoiRecorderCpuList parsed_affinity;
        std::string affinity_error;
        if (!safe_identifier(config_.cpu_affinity_source)) {
            return fail(error_out,
                        "recorder CPU affinity authority source is invalid");
        }
        if (!parse_spatial_roi_recorder_cpu_list(
                config_.cpu_affinity, &parsed_affinity, &affinity_error)) {
            return fail(error_out,
                        "recorder CPU affinity is invalid: " + affinity_error);
        }
        config_.cpu_affinity = std::move(parsed_affinity.canonical);
    }
    if (!valid_duration(config_.eof_timeout,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kMaxChildEofTimeout)) ||
        !valid_duration(config_.readiness_timeout,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kMaxChildReadinessTimeout)) ||
        !valid_duration(config_.poll_interval,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kMaxChildPollInterval)) ||
        !valid_duration(config_.heartbeat_interval,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kMaxChildHeartbeatInterval)) ||
        !valid_duration(config_.accept_timeout,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kMaxChildAcceptTimeout)) ||
        !valid_duration(config_.ipc_timeout,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kMaxChildIpcTimeout)) ||
        !valid_duration(config_.video_probe_timeout,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kMaxChildVideoProbeTimeout)) ||
        !valid_duration(config_.socket_wait_timeout,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kMaxSupervisorWait)) ||
        !valid_duration(config_.ready_wait_timeout,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kMaxSupervisorWait)) ||
        !valid_duration(config_.clean_exit_timeout,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kMaxSupervisorWait)) ||
        !valid_duration(config_.term_grace_timeout,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kMaxSupervisorKillWait)) ||
        !valid_duration(config_.kill_reap_timeout,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kMaxSupervisorKillWait)) ||
        !valid_duration(config_.supervisor_poll_interval,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kMaxSupervisorPollInterval)) ||
        config_.max_stdout_line_bytes == 0U ||
        config_.max_stdout_line_bytes > kMaxJsonBytes) {
        return fail(error_out, "camera recorder process bounds are invalid");
    }

    json candidate;
    json verified_plan;
    if (!read_bounded_json_file(config_.contract_path, &candidate, error_out) ||
        !read_bounded_json_file(config_.verified_plan_path,
                                &verified_plan,
                                error_out)) {
        return false;
    }

    contract::SpatialRoiRecordingPlan parsed_plan;
    std::string authority_error;
    if (!contract::parse_verified_plan(verified_plan,
                                       &parsed_plan,
                                       &authority_error)) {
        return fail(error_out,
                    "verified plan authentication failed: " + authority_error);
    }

    contract::SpatialRoiRecorderCameraContractView camera_contract;
    if (!contract::parse_spatial_roi_recorder_camera_contract(
            candidate,
            verified_plan,
            config_.expected_recording_root,
            config_.gpu_mapping,
            &camera_contract,
            &authority_error)) {
        return fail(error_out,
                    "camera recorder contract authentication failed: " +
                        authority_error);
    }
    if (camera_contract.stream_count != 4U ||
        camera_contract.stream_order.size() != 4U ||
        camera_contract.streams.size() != 4U ||
        parsed_plan.cameras.size() != 1U) {
        return fail(error_out,
                    "camera recorder process requires one camera and four streams");
    }
    expected_storage_preflight_policy_ =
        camera_contract.storage_preflight_policy;
    expected_aggregate_bounds_ = camera_contract.aggregate_bounds;

    try {
        runtime_directory_path_ =
            contract::expected_socket_runtime_directory(
                camera_contract.recording_identity_token);
        if (!safe_absolute_path(runtime_directory_path_,
                                false,
                                "derived socket runtime directory",
                                error_out) ||
            runtime_directory_path_.rfind("/tmp/orange_spatial_roi_", 0) != 0) {
            return fail(error_out,
                        "authenticated socket runtime directory is unsafe");
        }
        std::set<std::string> unique_paths;
        socket_paths_.clear();
        socket_paths_.reserve(4U);
        for (std::size_t index = 0; index < 4U; ++index) {
            const auto& stream = camera_contract.streams[index];
            if (!safe_identifier(stream.logical_stream_id) ||
                !safe_identifier(stream.camera_serial)) {
                return fail(error_out,
                            "authenticated stream identity is unsafe for child argv");
            }
            const std::string expected_path = contract::expected_socket_path(
                camera_contract.recording_identity_token,
                stream.logical_stream_id);
            if (stream.socket_path != expected_path ||
                expected_path.rfind(runtime_directory_path_ + "/", 0) != 0 ||
                expected_path.size() >= sizeof(sockaddr_un{}.sun_path) ||
                !unique_paths.insert(expected_path).second) {
                return fail(error_out,
                            "authenticated stream socket path is not exact and unique");
            }
            socket_paths_.push_back(expected_path);
        }
    } catch (const std::exception& exception) {
        return fail(error_out,
                    std::string("socket authority derivation failed: ") +
                        exception.what());
    } catch (...) {
        return fail(error_out, "socket authority derivation failed");
    }
    return build_child_argv(error_out);
}

bool SpatialRoiCameraRecorderProcess::build_child_argv(std::string* error_out)
{
    try {
        child_argv_.clear();
        child_argv_.reserve(36U);
        child_argv_.push_back(config_.recorder_executable);
        const auto add = [&](const char* option, const std::string& value) {
            child_argv_.push_back(option);
            child_argv_.push_back(value);
        };
        add("--contract", config_.contract_path);
        add("--plan", config_.verified_plan_path);
        add("--expected-recording-root", config_.expected_recording_root);
        add("--expected-producer-pid",
            decimal(static_cast<std::uint64_t>(config_.expected_producer_pid)));
        add("--expected-producer-uid",
            decimal(static_cast<std::uint64_t>(config_.expected_producer_uid)));
        add("--eof-timeout-ms",
            decimal(static_cast<std::uint64_t>(config_.eof_timeout.count())));
        if (!config_.cpu_affinity.empty()) {
            add("--cpu-affinity", config_.cpu_affinity);
            add("--cpu-affinity-source", config_.cpu_affinity_source);
        }
        const auto& streams = socket_paths_;
        (void)streams;

        // The camera parser authenticated exactly one analytics mapping and
        // four plan-ordered recorder mappings. std::map iteration is stable,
        // so the child receives one deterministic, complete argument set.
        for (const auto& [camera_serial, gpu] :
             config_.gpu_mapping.analytics_gpu_by_camera_serial) {
            if (!safe_identifier(camera_serial) || gpu < 0) {
                return fail(error_out, "analytics GPU mapping is unsafe");
            }
            add("--analytics-gpu",
                camera_serial + "=" + decimal(static_cast<std::uint64_t>(gpu)));
        }
        for (const auto& [logical_stream_id, gpu] :
             config_.gpu_mapping.recorder_gpu_by_logical_stream_id) {
            if (!safe_identifier(logical_stream_id) || gpu < 0) {
                return fail(error_out, "recorder GPU mapping is unsafe");
            }
            add("--recorder-gpu",
                logical_stream_id + "=" +
                    decimal(static_cast<std::uint64_t>(gpu)));
        }
        add("--readiness-timeout-ms",
            decimal(static_cast<std::uint64_t>(config_.readiness_timeout.count())));
        add("--poll-interval-ms",
            decimal(static_cast<std::uint64_t>(config_.poll_interval.count())));
        add("--heartbeat-interval-ms",
            decimal(static_cast<std::uint64_t>(config_.heartbeat_interval.count())));
        add("--accept-timeout-ms",
            decimal(static_cast<std::uint64_t>(config_.accept_timeout.count())));
        add("--ipc-timeout-ms",
            decimal(static_cast<std::uint64_t>(config_.ipc_timeout.count())));
        add("--video-probe-timeout-ms",
            decimal(static_cast<std::uint64_t>(config_.video_probe_timeout.count())));
    } catch (const std::exception& exception) {
        return fail(error_out,
                    std::string("child argv construction failed: ") +
                        exception.what());
    } catch (...) {
        return fail(error_out, "child argv construction failed");
    }
    return true;
}

bool SpatialRoiCameraRecorderProcess::launch(std::string* error_out)
{
    if (!config_.spawn_override) {
        const int raw_executable_fd =
            ::open(config_.recorder_executable.c_str(),
                   O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (raw_executable_fd < 0) {
            return fail(error_out,
                        "recorder executable could not be opened as a non-symlink file");
        }
        executable_fd_ = raw_executable_fd;
        struct stat executable_status {};
        if (::fstat(executable_fd_, &executable_status) != 0 ||
            !S_ISREG(executable_status.st_mode) ||
            (executable_status.st_mode & 0111) == 0) {
            (void)::close(std::exchange(executable_fd_, -1));
            return fail(error_out,
                        "recorder executable is not a regular executable file");
        }
    }

    int pipe_fds[2] = {-1, -1};
    if (::pipe2(pipe_fds, O_CLOEXEC) != 0) {
        const int saved_errno = errno;
        (void)::close(std::exchange(executable_fd_, -1));
        return fail(error_out, errno_message("pipe2 recorder stdout", saved_errno));
    }
    const int read_fd = pipe_fds[0];
    const int write_fd = pipe_fds[1];
    const int flags = ::fcntl(read_fd, F_GETFL);
    if (flags < 0 || ::fcntl(read_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        const int saved_errno = errno;
        (void)::close(read_fd);
        (void)::close(write_fd);
        (void)::close(std::exchange(executable_fd_, -1));
        return fail(error_out, errno_message("fcntl recorder stdout", saved_errno));
    }

    pid_t child = -1;
    try {
        if (config_.spawn_override) {
            child = config_.spawn_override(child_argv_, write_fd, error_out);
        } else {
            child = ::fork();
            if (child == 0) {
                (void)::close(read_fd);
                if (::dup2(write_fd, STDOUT_FILENO) < 0) _exit(126);
                if (write_fd != STDOUT_FILENO) (void)::close(write_fd);
                std::vector<char*> argv = mutable_argv(child_argv_);
                ::fexecve(executable_fd_, argv.data(), environ);
                _exit(127);
            }
            if (child < 0) {
                const int saved_errno = errno;
                (void)::close(read_fd);
                (void)::close(write_fd);
                (void)::close(std::exchange(executable_fd_, -1));
                return fail(error_out, errno_message("fork recorder", saved_errno));
            }
        }
    } catch (const std::exception& exception) {
        (void)::close(read_fd);
        (void)::close(write_fd);
        (void)::close(std::exchange(executable_fd_, -1));
        return fail(error_out,
                    std::string("recorder launch callback threw: ") +
                        exception.what());
    } catch (...) {
        (void)::close(read_fd);
        (void)::close(write_fd);
        (void)::close(std::exchange(executable_fd_, -1));
        return fail(error_out, "recorder launch callback threw");
    }

    (void)::close(write_fd);
    (void)::close(std::exchange(executable_fd_, -1));
    if (child <= 0) {
        (void)::close(read_fd);
        return fail(error_out,
                    error_out && !error_out->empty()
                        ? *error_out
                        : "recorder launch callback did not return a child PID");
    }
    stdout_fd_ = read_fd;
    status_.pid = child;
    status_.started = true;
    status_.state = SpatialRoiCameraRecorderProcessState::kStarting;
    status_.starting.event = "starting";
    status_.starting.status = "starting";
    status_.starting.state = "starting";
    status_.starting.payload = json{{"event", "starting"},
                                    {"status", "starting"},
                                    {"state", "starting"},
                                    {"pid", child}};
    status_.last = status_.starting;
    return true;
}

bool SpatialRoiCameraRecorderProcess::Start(std::string* error_out)
{
    if (error_out) error_out->clear();
    if (status_.state == SpatialRoiCameraRecorderProcessState::kFailed ||
        status_.state == SpatialRoiCameraRecorderProcessState::kStopped) {
        return fail(error_out,
                    status_.error.empty() ? "recorder process cannot restart"
                                          : status_.error);
    }
    if (status_.started) {
        return status_.state != SpatialRoiCameraRecorderProcessState::kFailed &&
               status_.state != SpatialRoiCameraRecorderProcessState::kStopped;
    }
    if (stop_attempted_) return fail(error_out, "recorder process cannot restart");
    if (!authenticate(error_out)) {
        status_.state = SpatialRoiCameraRecorderProcessState::kFailed;
        status_.error = error_out && !error_out->empty()
                            ? *error_out
                            : "recorder process authentication failed";
        return false;
    }
    if (!launch(error_out)) {
        status_.state = SpatialRoiCameraRecorderProcessState::kFailed;
        status_.error = error_out && !error_out->empty()
                            ? *error_out
                            : "recorder process launch failed";
        return false;
    }
    return true;
}

bool SpatialRoiCameraRecorderProcess::open_runtime_directory(
    bool* pending,
    std::string* error_out)
{
    if (pending) *pending = false;

    if (runtime_directory_fd_ >= 0) {
        struct stat descriptor_status {};
        struct stat entry_status {};
        if (::fstat(runtime_directory_fd_, &descriptor_status) != 0) {
            return fail(error_out, errno_message("fstat socket runtime directory", errno));
        }
        if (::lstat(runtime_directory_path_.c_str(), &entry_status) != 0) {
            if (errno == ENOENT) {
                (void)::close(std::exchange(runtime_directory_fd_, -1));
                if (pending) *pending = true;
                return true;
            }
            return fail(error_out,
                        errno_message("lstat socket runtime directory", errno));
        }
        if (!stat_is_owned_directory(descriptor_status) ||
            !stat_is_owned_directory(entry_status) ||
            descriptor_status.st_dev != entry_status.st_dev ||
            descriptor_status.st_ino != entry_status.st_ino) {
            return fail(error_out,
                        "socket runtime directory was replaced or is unsafe");
        }
        return true;
    }

    const int raw_fd = ::open(runtime_directory_path_.c_str(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (raw_fd < 0) {
        if (errno == ENOENT) {
            if (pending) *pending = true;
            return true;
        }
        if (errno == ELOOP) {
            return fail(error_out, "socket runtime directory is a symlink");
        }
        return fail(error_out,
                    errno_message("open socket runtime directory", errno));
    }
    struct stat descriptor_status {};
    struct stat entry_status {};
    const bool valid =
        ::fstat(raw_fd, &descriptor_status) == 0 &&
        ::lstat(runtime_directory_path_.c_str(), &entry_status) == 0 &&
        stat_is_owned_directory(descriptor_status) &&
        stat_is_owned_directory(entry_status) &&
        descriptor_status.st_dev == entry_status.st_dev &&
        descriptor_status.st_ino == entry_status.st_ino;
    if (!valid) {
        const int saved_errno = errno;
        (void)::close(raw_fd);
        return fail(error_out,
                    saved_errno == ENOENT
                        ? "socket runtime directory disappeared during validation"
                        : "socket runtime directory is unsafe or was replaced");
    }
    runtime_directory_fd_ = raw_fd;
    return true;
}

bool SpatialRoiCameraRecorderProcess::sockets_ready(
    bool* pending,
    std::string* error_out)
{
    if (pending) *pending = false;
    bool directory_pending = false;
    if (!open_runtime_directory(&directory_pending, error_out)) return false;
    if (directory_pending) {
        if (pending) *pending = true;
        return true;
    }
    const std::string prefix = runtime_directory_path_ + "/";
    for (const std::string& path : socket_paths_) {
        if (path.rfind(prefix, 0) != 0 || path.find('/', prefix.size()) !=
                                                  std::string::npos) {
            return fail(error_out, "authenticated socket path escaped runtime directory");
        }
        const std::string leaf = path.substr(prefix.size());
        struct stat status {};
        if (::fstatat(runtime_directory_fd_,
                      leaf.c_str(),
                      &status,
                      AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                if (pending) *pending = true;
                continue;
            }
            return fail(error_out, errno_message("fstatat recorder socket", errno));
        }
        if (!stat_is_owned_socket(status)) {
            return fail(error_out,
                        "recorder socket path is not an euid-owned mode-0600 socket");
        }
    }
    return true;
}

bool SpatialRoiCameraRecorderProcess::validate_storage_preflight_payload(
    const json& value,
    std::string* error_out) const
{
    if (error_out) error_out->clear();
    if (!exact_object_keys(value,
                           {"schema_id", "schema_version", "checked", "passed",
                            "status", "error", "policy", "artifact_root",
                            "filesystem", "budgets"},
                           "storage_preflight",
                           error_out) ||
        !exact_string(value,
                      "schema_id",
                      contract::kSpatialRoiRecorderStoragePreflightSchemaId,
                      "storage_preflight",
                      error_out) ||
        !exact_schema_version(
            value,
            "schema_version",
            contract::kSpatialRoiRecorderStoragePreflightSchemaVersion,
            "storage_preflight",
            error_out) ||
        !exact_boolean(value, "checked", true, "storage_preflight", error_out) ||
        !exact_boolean(value, "passed", true, "storage_preflight", error_out) ||
        !exact_string(value, "status", "passed", "storage_preflight", error_out) ||
        !exact_string(value, "error", "", "storage_preflight", error_out)) {
        return false;
    }

    const json& policy = value.at("policy");
    if (!exact_object_keys(policy,
                           {"schema_id", "schema_version", "required",
                            "reserved_free_bytes"},
                           "storage_preflight.policy",
                           error_out) ||
        !exact_string(
            policy,
            "schema_id",
            expected_storage_preflight_policy_.schema_id,
            "storage_preflight.policy",
            error_out) ||
        !exact_schema_version(
            policy,
            "schema_version",
            expected_storage_preflight_policy_.schema_version,
            "storage_preflight.policy",
            error_out) ||
        !exact_boolean(policy,
                       "required",
                       true,
                       "storage_preflight.policy",
                       error_out) ||
        !exact_unsigned_u64(policy,
                            "reserved_free_bytes",
                            expected_storage_preflight_policy_.reserved_free_bytes,
                            "storage_preflight.policy",
                            error_out) ||
        expected_storage_preflight_policy_.schema_id !=
            contract::kSpatialRoiRecorderStoragePreflightPolicySchemaId ||
        expected_storage_preflight_policy_.schema_version !=
            contract::kSpatialRoiRecorderStoragePreflightPolicySchemaVersion ||
        !expected_storage_preflight_policy_.required ||
        expected_storage_preflight_policy_.reserved_free_bytes == 0) {
        if (error_out && error_out->empty()) {
            *error_out = "storage_preflight.policy is not the authenticated nonzero policy";
        }
        return false;
    }

    const json& artifact_root = value.at("artifact_root");
    if (!exact_object_keys(artifact_root,
                           {"device", "inode"},
                           "storage_preflight.artifact_root",
                           error_out)) {
        return false;
    }
    std::uint64_t artifact_device = 0;
    std::uint64_t artifact_inode = 0;
    if (!read_unsigned_u64(artifact_root,
                           "device",
                           "storage_preflight.artifact_root",
                           &artifact_device,
                           error_out) ||
        !read_unsigned_u64(artifact_root,
                           "inode",
                           "storage_preflight.artifact_root",
                           &artifact_inode,
                           error_out) ||
        artifact_inode == 0) {
        if (error_out && error_out->empty()) {
            *error_out = "storage_preflight.artifact_root inode is zero";
        }
        return false;
    }
    if (config_.expected_artifact_root_identity_available &&
        (artifact_device != config_.expected_artifact_root_identity.device ||
         artifact_inode != config_.expected_artifact_root_identity.inode)) {
        return fail(error_out,
                    "storage_preflight artifact-root identity disagrees with the authenticated expectation");
    }

    const json& filesystem = value.at("filesystem");
    if (!exact_object_keys(filesystem,
                           {"block_size_bytes", "total_blocks", "available_blocks",
                            "capacity_bytes", "available_bytes"},
                           "storage_preflight.filesystem",
                           error_out)) {
        return false;
    }
    std::uint64_t block_size = 0;
    std::uint64_t total_blocks = 0;
    std::uint64_t available_blocks = 0;
    std::uint64_t capacity_bytes = 0;
    std::uint64_t available_bytes = 0;
    if (!read_unsigned_u64(filesystem,
                           "block_size_bytes",
                           "storage_preflight.filesystem",
                           &block_size,
                           error_out) ||
        !read_unsigned_u64(filesystem,
                           "total_blocks",
                           "storage_preflight.filesystem",
                           &total_blocks,
                           error_out) ||
        !read_unsigned_u64(filesystem,
                           "available_blocks",
                           "storage_preflight.filesystem",
                           &available_blocks,
                           error_out) ||
        !read_unsigned_u64(filesystem,
                           "capacity_bytes",
                           "storage_preflight.filesystem",
                           &capacity_bytes,
                           error_out) ||
        !read_unsigned_u64(filesystem,
                           "available_bytes",
                           "storage_preflight.filesystem",
                           &available_bytes,
                           error_out) ||
        block_size == 0 || available_blocks > total_blocks) {
        if (error_out && error_out->empty()) {
            *error_out = "storage_preflight filesystem block observation is invalid";
        }
        return false;
    }
    std::uint64_t expected_capacity_bytes = 0;
    std::uint64_t expected_available_bytes = 0;
    if (!checked_multiply_u64(total_blocks,
                              block_size,
                              &expected_capacity_bytes) ||
        !checked_multiply_u64(available_blocks,
                              block_size,
                              &expected_available_bytes) ||
        capacity_bytes != expected_capacity_bytes ||
        available_bytes != expected_available_bytes ||
        available_bytes > capacity_bytes) {
        return fail(error_out,
                    "storage_preflight filesystem byte arithmetic is invalid or overflowed");
    }

    const json& budgets = value.at("budgets");
    if (!exact_object_keys(budgets,
                           {"max_media_bytes_total", "max_evidence_bytes_total",
                            "reserved_free_bytes", "required_bytes"},
                           "storage_preflight.budgets",
                           error_out)) {
        return false;
    }
    std::uint64_t media_bytes = 0;
    std::uint64_t evidence_bytes = 0;
    std::uint64_t reserved_free_bytes = 0;
    std::uint64_t required_bytes = 0;
    if (!read_unsigned_u64(budgets,
                           "max_media_bytes_total",
                           "storage_preflight.budgets",
                           &media_bytes,
                           error_out) ||
        !read_unsigned_u64(budgets,
                           "max_evidence_bytes_total",
                           "storage_preflight.budgets",
                           &evidence_bytes,
                           error_out) ||
        !read_unsigned_u64(budgets,
                           "reserved_free_bytes",
                           "storage_preflight.budgets",
                           &reserved_free_bytes,
                           error_out) ||
        !read_unsigned_u64(budgets,
                           "required_bytes",
                           "storage_preflight.budgets",
                           &required_bytes,
                           error_out)) {
        return false;
    }
    if (media_bytes != expected_aggregate_bounds_.max_media_bytes_total ||
        evidence_bytes != expected_aggregate_bounds_.max_evidence_bytes_total ||
        reserved_free_bytes != expected_storage_preflight_policy_.reserved_free_bytes ||
        reserved_free_bytes == 0) {
        return fail(error_out,
                    "storage_preflight budgets disagree with the authenticated contract");
    }
    std::uint64_t expected_required_bytes = 0;
    if (!checked_add_u64(media_bytes, evidence_bytes, &expected_required_bytes) ||
        !checked_add_u64(expected_required_bytes,
                         reserved_free_bytes,
                         &expected_required_bytes) ||
        required_bytes != expected_required_bytes ||
        available_bytes < required_bytes) {
        return fail(error_out,
                    "storage_preflight required byte total is invalid or unavailable");
    }
    return true;
}

bool SpatialRoiCameraRecorderProcess::validate_scheduling_payload(
    const json& value,
    std::string* error_out) const
{
    if (error_out) error_out->clear();
    constexpr const char* kPath = "scheduling";
    if (!exact_object_keys(
            value,
            {"schema_id", "schema_version", "scope", "configuration_mode",
             "configuration_source", "requested_cpu_list",
             "canonical_requested_cpu_list", "affinity_syscall_succeeded",
             "affinity_applied", "effective_mask_verified",
             "effective_cpu_list", "effective_cpus",
             "kernel_isolated_cpu_list", "kernel_isolated_cpus",
             "kernel_isolation_observed",
             "kernel_isolation_observation_error", "scheduler",
             "application_phase", "thread_inheritance", "error"},
            kPath,
            error_out) ||
        !exact_string(value,
                      "schema_id",
                      "orange.spatial_roi_recorder.scheduling",
                      kPath,
                      error_out) ||
        !exact_schema_version(value, "schema_version", 1, kPath, error_out) ||
        !exact_string(value,
                      "scope",
                      "camera_recorder_inherited_thread_mask",
                      kPath,
                      error_out) ||
        !exact_string(value,
                      "application_phase",
                      "before_recorder_authority_and_worker_initialization",
                      kPath,
                      error_out) ||
        !exact_string(
            value,
            "thread_inheritance",
            "recorder_threads_created_after_this_snapshot_inherit_the_effective_mask",
            kPath,
            error_out)) {
        return false;
    }

    const bool configured = !config_.cpu_affinity.empty();
    if (!exact_string(value,
                      "configuration_mode",
                      configured ? "explicit" : "inherited",
                      kPath,
                      error_out) ||
        !exact_string(value,
                      "configuration_source",
                      configured ? config_.cpu_affinity_source
                                 : "inherited_process_affinity",
                      kPath,
                      error_out) ||
        !exact_boolean(value,
                       "affinity_syscall_succeeded",
                       configured,
                       kPath,
                       error_out) ||
        !exact_boolean(value,
                       "affinity_applied",
                       configured,
                       kPath,
                       error_out) ||
        !exact_boolean(value,
                       "effective_mask_verified",
                       configured,
                       kPath,
                       error_out)) {
        return false;
    }
    if (configured) {
        if (!exact_string(value,
                          "requested_cpu_list",
                          config_.cpu_affinity,
                          kPath,
                          error_out) ||
            !exact_string(value,
                          "canonical_requested_cpu_list",
                          config_.cpu_affinity,
                          kPath,
                          error_out)) {
            return false;
        }
    } else if (!value.at("requested_cpu_list").is_null() ||
               !value.at("canonical_requested_cpu_list").is_null()) {
        return fail(error_out,
                    "inherited scheduling must use null requested CPU lists");
    }

    const auto parse_cpu_array = [&](const json& array,
                                     const char* path,
                                     std::vector<int>* cpus_out) {
        if (!cpus_out || !array.is_array()) {
            return fail(error_out, std::string(path) + " must be an array");
        }
        cpus_out->clear();
        cpus_out->reserve(array.size());
        for (const json& item : array) {
            if ((!item.is_number_integer() && !item.is_number_unsigned())) {
                return fail(error_out,
                            std::string(path) + " must contain integers");
            }
            std::int64_t cpu = -1;
            try {
                cpu = item.get<std::int64_t>();
            } catch (...) {
                return fail(error_out,
                            std::string(path) + " contains an out-of-range CPU");
            }
            if (cpu < 0 || cpu >= CPU_SETSIZE) {
                return fail(error_out,
                            std::string(path) + " contains an invalid CPU");
            }
            cpus_out->push_back(static_cast<int>(cpu));
        }
        return true;
    };
    if (!value.at("effective_cpu_list").is_string()) {
        return fail(error_out, "scheduling.effective_cpu_list must be a string");
    }
    SpatialRoiRecorderCpuList effective;
    std::string cpu_error;
    const std::string effective_text =
        value.at("effective_cpu_list").get<std::string>();
    if (!parse_spatial_roi_recorder_cpu_list(
            effective_text, &effective, &cpu_error)) {
        return fail(error_out,
                    "scheduling effective CPU list is invalid: " + cpu_error);
    }
    std::vector<int> effective_array;
    if (!parse_cpu_array(value.at("effective_cpus"),
                         "scheduling.effective_cpus",
                         &effective_array) ||
        effective.canonical != effective_text ||
        effective.cpus != effective_array ||
        (configured && effective.canonical != config_.cpu_affinity)) {
        if (error_out && error_out->empty()) {
            *error_out =
                "scheduling effective CPU representations do not match exactly";
        }
        return false;
    }

    if (!value.at("kernel_isolation_observed").is_boolean()) {
        return fail(error_out,
                    "scheduling.kernel_isolation_observed must be boolean");
    }
    const bool isolation_observed =
        value.at("kernel_isolation_observed").get<bool>();
    std::vector<int> isolated_array;
    if (!parse_cpu_array(value.at("kernel_isolated_cpus"),
                         "scheduling.kernel_isolated_cpus",
                         &isolated_array)) {
        return false;
    }
    if (isolation_observed) {
        if (!value.at("kernel_isolation_observation_error").is_null()) {
            return fail(error_out,
                        "observed kernel isolation must not carry an error");
        }
        if (isolated_array.empty()) {
            if (!value.at("kernel_isolated_cpu_list").is_null()) {
                return fail(error_out,
                            "empty observed kernel isolation must use a null CPU list");
            }
        } else {
            if (!value.at("kernel_isolated_cpu_list").is_string()) {
                return fail(error_out,
                            "observed kernel isolation CPU list must be a string");
            }
            SpatialRoiRecorderCpuList isolated;
            if (!parse_spatial_roi_recorder_cpu_list(
                    value.at("kernel_isolated_cpu_list").get<std::string>(),
                    &isolated,
                    &cpu_error) ||
                isolated.cpus != isolated_array ||
                isolated.canonical !=
                    value.at("kernel_isolated_cpu_list").get<std::string>()) {
                return fail(error_out,
                            "kernel isolation CPU representations do not match exactly");
            }
        }
    } else if (!isolated_array.empty() ||
               !value.at("kernel_isolated_cpu_list").is_null() ||
               !value.at("kernel_isolation_observation_error").is_string() ||
               value.at("kernel_isolation_observation_error")
                   .get<std::string>()
                   .empty()) {
        return fail(error_out,
                    "unobserved kernel isolation must carry only a nonempty error");
    }

    const json& scheduler = value.at("scheduler");
    if (!exact_object_keys(scheduler,
                           {"policy", "priority"},
                           "scheduling.scheduler",
                           error_out) ||
        !scheduler.at("policy").is_string() ||
        scheduler.at("policy").get<std::string>().empty() ||
        (!scheduler.at("priority").is_number_integer() &&
         !scheduler.at("priority").is_number_unsigned()) ||
        !value.at("error").is_null()) {
        return fail(error_out,
                    "scheduling scheduler or success error field is invalid");
    }
    return true;
}

bool SpatialRoiCameraRecorderProcess::poll_child(std::string* error_out)
{
    if (!status_.started || status_.reaped) return true;
    int wait_status = 0;
    const pid_t result = ::waitpid(status_.pid, &wait_status, WNOHANG);
    if (result == 0) return true;
    if (result < 0) {
        if (errno == EINTR) return true;
        return set_error(error_out, errno_message("waitpid recorder", errno));
    }
    status_.reaped = true;
    status_.exited = true;
    if (WIFEXITED(wait_status)) {
        status_.exit_code = WEXITSTATUS(wait_status);
    } else if (WIFSIGNALED(wait_status)) {
        status_.term_signal = WTERMSIG(wait_status);
    }
    if (status_.state != SpatialRoiCameraRecorderProcessState::kFailed &&
        status_.state != SpatialRoiCameraRecorderProcessState::kStopped) {
        status_.state = SpatialRoiCameraRecorderProcessState::kExited;
    }
    return true;
}

bool SpatialRoiCameraRecorderProcess::parse_line(
    const std::string& line,
    std::string* error_out)
{
    if (line.empty() || line.size() > config_.max_stdout_line_bytes) {
        return fail(error_out, "recorder stdout JSONL line exceeds its bound");
    }
    // Validate the bounded line with the same SAX guard used for authority
    // files before constructing a DOM.  In particular, nlohmann's default
    // object parser accepts the last duplicate key and has no lifecycle-line
    // depth bound of its own.
    BoundedJsonSax validator;
    if (!json::sax_parse(line, &validator)) {
        return fail(error_out,
                    "recorder stdout JSONL object exceeds structural bounds, "
                    "contains duplicate keys, or is invalid");
    }
    const json payload = json::parse(line, nullptr, false);
    if (payload.is_discarded() || !payload.is_object() ||
        !payload.contains("event") || !payload.at("event").is_string()) {
        return fail(error_out, "recorder stdout contained a malformed JSONL object");
    }
    const std::string event = payload.at("event").get<std::string>();
    if (event != "ready" && event != "heartbeat" && event != "terminal") {
        return fail(error_out, "recorder stdout contained an unknown lifecycle event");
    }
    SpatialRoiCameraRecorderProcessSnapshot snapshot;
    snapshot.event = event;
    snapshot.payload = payload;
    if (payload.contains("status") && payload.at("status").is_string()) {
        snapshot.status = payload.at("status").get<std::string>();
    }
    if (payload.contains("state") && payload.at("state").is_string()) {
        snapshot.state = payload.at("state").get<std::string>();
    }
    const auto get_bool = [&](const char* key, bool* value) -> bool {
        if (!payload.contains(key)) return true;
        if (!payload.at(key).is_boolean()) return false;
        *value = payload.at(key).get<bool>();
        return true;
    };
    if (!get_bool("ready", &snapshot.ready) ||
        !get_bool("clean_eof", &snapshot.clean_eof) ||
        !get_bool("completed", &snapshot.completed) ||
        !get_bool("failed", &snapshot.failed)) {
        return fail(error_out, "recorder stdout lifecycle flags were not boolean");
    }
    const auto get_text = [&](const char* key, std::string* value) -> bool {
        if (!payload.contains(key)) return true;
        if (!payload.at(key).is_string()) return false;
        *value = bounded_reason(payload.at(key).get<std::string>(), "");
        return true;
    };
    if (!get_text("first_failure_stream_id", &snapshot.first_failure_stream_id) ||
        !get_text("first_failure", &snapshot.first_failure) ||
        !get_text("error", &snapshot.error)) {
        return fail(error_out, "recorder stdout lifecycle text fields were invalid");
    }
    if (event == "ready" && !snapshot.ready) {
        return fail(error_out, "recorder ready event did not assert ready=true");
    }
    if (event == "ready") {
        if (status_.ready) {
            return fail(error_out, "recorder emitted a duplicate ready event");
        }
        if (status_.terminal_seen) {
            return fail(error_out,
                        "recorder emitted ready after its terminal event");
        }
        if (snapshot.status != "ready" || snapshot.state != "ready" ||
            !snapshot.ready || snapshot.clean_eof || snapshot.completed ||
            snapshot.failed || !payload.contains("clean_eof") ||
            !payload.contains("completed") || !payload.contains("failed") ||
            !payload.contains("first_failure_stream_id") ||
            !payload.contains("first_failure") || !payload.contains("error") ||
            !payload.at("first_failure_stream_id").is_string() ||
            !payload.at("first_failure").is_string() ||
            !payload.at("error").is_string() ||
            !payload.at("first_failure_stream_id").get<std::string>().empty() ||
            !payload.at("first_failure").get<std::string>().empty() ||
            !payload.at("error").get<std::string>().empty()) {
            return fail(error_out,
                        "recorder ready event did not have exact clean readiness semantics");
        }
    }
    if (event == "terminal") {
        snapshot.failed = snapshot.failed || snapshot.status == "failed";
        if (snapshot.failed && snapshot.error.empty()) {
            snapshot.error = snapshot.first_failure;
        }
    }
    const bool terminal_complete =
        event == "terminal" && snapshot.completed && !snapshot.failed &&
        snapshot.status == "complete";
    if (snapshot.ready || event == "heartbeat" || terminal_complete) {
        const auto scheduling = payload.find("scheduling");
        if (scheduling == payload.end() ||
            !validate_scheduling_payload(scheduling.value(), error_out)) {
            if (error_out && !error_out->empty()) {
                return fail(error_out,
                            "recorder lifecycle scheduling is invalid: " +
                                *error_out);
            }
            return fail(error_out,
                        "recorder lifecycle scheduling is missing or invalid");
        }
        if (status_.ready && event != "ready" &&
            (!status_.ready_snapshot.payload.contains("scheduling") ||
             status_.ready_snapshot.payload.at("scheduling") !=
                 scheduling.value())) {
            return fail(error_out,
                        "recorder lifecycle scheduling does not match ready evidence");
        }
    }
    if (snapshot.ready || terminal_complete) {
        const auto preflight = payload.find("storage_preflight");
        if (preflight == payload.end() ||
            !validate_storage_preflight_payload(preflight.value(), error_out)) {
            if (error_out && !error_out->empty()) {
                return fail(error_out,
                            "recorder lifecycle storage preflight is invalid: " +
                                *error_out);
            }
            return fail(error_out,
                        "recorder lifecycle storage preflight is missing or invalid");
        }
    }
    if (terminal_complete) {
        if (!status_.ready ||
            !status_.ready_snapshot.payload.contains("storage_preflight") ||
            status_.ready_snapshot.payload.at("storage_preflight") !=
                payload.at("storage_preflight")) {
            return fail(error_out,
                        "recorder complete terminal storage preflight does not match ready evidence");
        }
    }
    status_.last = snapshot;
    if (event == "ready") {
        status_.ready_snapshot = snapshot;
        status_.ready = true;
    } else if (event == "heartbeat") {
        status_.heartbeat = snapshot;
    } else {
        status_.terminal = snapshot;
        status_.terminal_seen = true;
    }
    return true;
}

bool SpatialRoiCameraRecorderProcess::pump_output(std::string* error_out)
{
    if (stdout_fd_ < 0 || stdout_closed_) return true;
    std::array<char, 16384> bytes{};
    while (true) {
        const ssize_t count = ::read(stdout_fd_, bytes.data(), bytes.size());
        if (count > 0) {
            const std::uint64_t received = static_cast<std::uint64_t>(count);
            const std::uint64_t maximum =
                std::numeric_limits<std::uint64_t>::max();
            if (received > maximum - status_.stdout_bytes_read) {
                status_.stdout_bytes_read = maximum;
            } else {
                status_.stdout_bytes_read += received;
            }
            try {
                stdout_line_buffer_.append(bytes.data(),
                                           static_cast<std::size_t>(count));
            } catch (const std::exception& exception) {
                return fail(error_out,
                            std::string("recorder stdout buffer allocation failed: ") +
                                exception.what());
            }
            while (true) {
                const std::size_t newline = stdout_line_buffer_.find('\n');
                if (newline == std::string::npos) break;
                std::string line = stdout_line_buffer_.substr(0, newline);
                stdout_line_buffer_.erase(0, newline + 1U);
                if (!parse_line(line, error_out)) return false;
            }
            // Check only the unterminated remainder. Complete lines may have
            // arrived in one read and are no longer part of the bounded line
            // buffer after the loop above.
            if (stdout_line_buffer_.size() > config_.max_stdout_line_bytes) {
                return fail(error_out, "recorder stdout JSONL line exceeds its bound");
            }
            continue;
        }
        if (count == 0) {
            stdout_closed_ = true;
            (void)::close(std::exchange(stdout_fd_, -1));
            if (!stdout_line_buffer_.empty()) {
                return fail(error_out,
                            "recorder stdout ended with an unterminated JSONL line");
            }
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return fail(error_out, errno_message("read recorder stdout", errno));
    }
}

bool SpatialRoiCameraRecorderProcess::wait_until(
    const std::chrono::steady_clock::time_point deadline,
    const char* phase,
    const std::function<bool()>& predicate,
    std::string* error_out)
{
    while (true) {
        try {
            std::string pump_error;
            if (!pump_output(&pump_error)) return fail(error_out, pump_error);
            std::string child_error;
            if (!poll_child(&child_error)) return fail(error_out, child_error);
            if (predicate()) return true;
            if (status_.reaped) {
                std::string message =
                    std::string("recorder child exited before ") + phase;
                if (status_.exit_code >= 0) {
                    message += " (exit_code=" +
                               std::to_string(status_.exit_code) + ")";
                } else if (status_.term_signal > 0) {
                    message += " (signal=" +
                               std::to_string(status_.term_signal) + ")";
                }
                return fail(error_out, std::move(message));
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return fail(error_out,
                            std::string("recorder ") + phase +
                                " deadline expired");
            }
            const auto next = std::min(deadline,
                                       now + config_.supervisor_poll_interval);
            if (stdout_fd_ >= 0) {
                pollfd descriptor{};
                descriptor.fd = stdout_fd_;
                descriptor.events = POLLIN;
                const int result = ::poll(&descriptor, 1, poll_timeout_ms(next));
                if (result < 0 && errno != EINTR) {
                    return fail(error_out,
                                errno_message("poll recorder stdout", errno));
                }
            } else {
                std::this_thread::sleep_for(next - now);
            }
        } catch (const std::exception& exception) {
            return fail(error_out,
                        std::string("recorder supervisor wait failed: ") +
                            exception.what());
        } catch (...) {
            return fail(error_out, "recorder supervisor wait failed");
        }
    }
}

bool SpatialRoiCameraRecorderProcess::WaitForFourSockets(
    std::string* error_out)
{
    if (error_out) error_out->clear();
    if (!status_.started || stop_attempted_) {
        return fail(error_out, "recorder process is not running");
    }
    if (status_.state != SpatialRoiCameraRecorderProcessState::kStarting) {
        return status_.sockets_bound;
    }
    std::string socket_error;
    bool pending = true;
    const bool completed = wait_until(
        deadline_after(config_.socket_wait_timeout),
        "socket binding",
        [&] {
            if (!sockets_ready(&pending, &socket_error)) return true;
            return !pending;
        },
        error_out);
    if (!completed) return fail_and_stop(error_out && !error_out->empty()
                                             ? *error_out
                                             : "recorder socket binding failed",
                                         error_out);
    if (!socket_error.empty()) return fail_and_stop(socket_error, error_out);
    status_.sockets_bound = true;
    status_.state = SpatialRoiCameraRecorderProcessState::kSocketsBound;
    return true;
}

bool SpatialRoiCameraRecorderProcess::WaitUntilReady(std::string* error_out)
{
    if (error_out) error_out->clear();
    if (!status_.started || stop_attempted_) {
        return fail(error_out, "recorder process is not running");
    }
    if (status_.state == SpatialRoiCameraRecorderProcessState::kReady) return true;
    if (!status_.sockets_bound) {
        return fail(error_out,
                    "recorder readiness requires the four-socket bound phase");
    }
    std::string ready_error;
    const bool completed = wait_until(
        deadline_after(config_.ready_wait_timeout),
        "recorder readiness",
        [&] {
            if (status_.terminal_seen && status_.terminal.failed) {
                ready_error = status_.terminal.error.empty()
                                  ? "recorder reported terminal failure before ready"
                                  : status_.terminal.error;
                return true;
            }
            return status_.ready;
        },
        error_out);
    if (!completed) return fail_and_stop(error_out && !error_out->empty()
                                             ? *error_out
                                             : "recorder readiness failed",
                                         error_out);
    if (!ready_error.empty()) return fail_and_stop(ready_error, error_out);
    status_.state = SpatialRoiCameraRecorderProcessState::kReady;
    return true;
}

bool SpatialRoiCameraRecorderProcess::WaitForCleanExit(
    std::string* error_out)
{
    if (error_out) error_out->clear();
    if (!status_.started || stop_attempted_) {
        return fail(error_out, "recorder process is not running");
    }
    if (status_.state != SpatialRoiCameraRecorderProcessState::kReady &&
        status_.state != SpatialRoiCameraRecorderProcessState::kExited) {
        return fail(error_out, "clean exit requires recorder readiness");
    }
    std::string clean_error;
    const bool completed = wait_until(
        deadline_after(config_.clean_exit_timeout),
        "clean recorder exit",
        [&] {
            if (status_.terminal_seen && status_.terminal.failed) {
                clean_error = status_.terminal.error.empty()
                                  ? "recorder reported terminal failure"
                                  : status_.terminal.error;
                return true;
            }
            if (!status_.terminal_seen || !status_.reaped || !status_.exited ||
                !stdout_closed_) {
                return false;
            }
            if (status_.exit_code != 0 || !status_.terminal.completed ||
                !status_.terminal.clean_eof) {
                clean_error = "recorder did not produce a clean completed terminal state";
                return true;
            }
            return true;
        },
        error_out);
    if (!completed) return fail_and_stop(error_out && !error_out->empty()
                                             ? *error_out
                                             : "recorder clean exit failed",
                                         error_out);
    if (!clean_error.empty()) return fail_and_stop(clean_error, error_out);
    status_.state = SpatialRoiCameraRecorderProcessState::kExited;
    return true;
}

bool SpatialRoiCameraRecorderProcess::terminate_bounded(std::string* error_out)
{
    if (!status_.started || status_.reaped) return true;
    bool signal_error = false;
    if (::kill(status_.pid, SIGTERM) != 0 && errno != ESRCH) {
        signal_error = true;
        if (error_out) *error_out = errno_message("SIGTERM recorder", errno);
    }

    const auto wait_for_reap = [&](const std::chrono::milliseconds timeout) {
        const auto deadline = deadline_after(timeout);
        while (!status_.reaped && std::chrono::steady_clock::now() < deadline) {
            (void)pump_output(nullptr);
            std::string wait_error;
            if (!poll_child(&wait_error)) {
                if (error_out && error_out->empty()) *error_out = wait_error;
                return false;
            }
            if (status_.reaped) return true;
            const auto now = std::chrono::steady_clock::now();
            const auto next = std::min(deadline, now + config_.supervisor_poll_interval);
            if (stdout_fd_ >= 0) {
                pollfd descriptor{};
                descriptor.fd = stdout_fd_;
                descriptor.events = POLLIN;
                const int result = ::poll(&descriptor, 1, poll_timeout_ms(next));
                if (result < 0 && errno != EINTR) {
                    if (error_out) *error_out = errno_message("poll recorder stdout", errno);
                    return false;
                }
            } else {
                std::this_thread::sleep_for(next - now);
            }
        }
        return status_.reaped;
    };

    if (!wait_for_reap(config_.term_grace_timeout)) {
        if (::kill(status_.pid, SIGKILL) != 0 && errno != ESRCH && error_out &&
            error_out->empty()) {
            *error_out = errno_message("SIGKILL recorder", errno);
        }
        if (!wait_for_reap(config_.kill_reap_timeout)) {
            if (error_out && error_out->empty()) {
                *error_out =
                    "recorder child was not reaped after bounded SIGKILL wait; "
                    "outer supervisor termination remains required";
            }
            return false;
        }
    }
    (void)signal_error;
    return true;
}

bool SpatialRoiCameraRecorderProcess::Stop(std::string* error_out)
{
    if (error_out) error_out->clear();
    if (stop_attempted_) return status_.reaped || !status_.started;
    stop_attempted_ = true;
    if (!status_.started) {
        status_.state = SpatialRoiCameraRecorderProcessState::kStopped;
        return true;
    }
    std::string stop_error;
    const bool terminated = terminate_bounded(&stop_error);
    (void)pump_output(nullptr);
    if (stdout_fd_ >= 0) (void)::close(std::exchange(stdout_fd_, -1));
    if (!terminated) {
        status_.state = SpatialRoiCameraRecorderProcessState::kFailed;
        status_.error = bounded_reason(stop_error,
                                       "recorder process teardown failed");
        return fail(error_out, status_.error);
    }
    if (status_.state != SpatialRoiCameraRecorderProcessState::kFailed) {
        status_.state = SpatialRoiCameraRecorderProcessState::kStopped;
    }
    if (!stop_error.empty()) {
        status_.error = bounded_reason(stop_error);
        return fail(error_out, status_.error);
    }
    return true;
}

bool SpatialRoiCameraRecorderProcess::fail_and_stop(
    const std::string& reason,
    std::string* error_out)
{
    status_.state = SpatialRoiCameraRecorderProcessState::kFailed;
    status_.error = bounded_reason(reason);
    std::string stop_error;
    if (status_.started && !status_.reaped && !stop_attempted_ &&
        !terminate_bounded(&stop_error)) {
        if (!stop_error.empty()) {
            status_.error += "; ";
            status_.error += bounded_reason(stop_error);
            status_.error = bounded_reason(status_.error);
        }
    }
    if (stdout_fd_ >= 0 && (status_.reaped || stop_attempted_)) {
        (void)::close(std::exchange(stdout_fd_, -1));
    }
    return fail(error_out, status_.error);
}

}  // namespace orange::spatial_roi::recording
