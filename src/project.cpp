// src/project.cpp
#include "project.h"
#include "camera_config_schema.h"
#include "fsuid_guard.h"
#include "spatial_calibration_snapshot.h"
#include <unistd.h>      // For gethostname in client_send_bringup_message
#include <pwd.h>
#include <sys/stat.h>    // For mkdir
#include <sys/wait.h>
#include <iostream>
#include <fstream>       // For std::ifstream
#include <filesystem>    // For std::filesystem
#include <algorithm>     // For std::sort, std::find_if
#include <numeric>       // For std::iota (if used, though it's in camera.cpp sort_indexes)
#include <iomanip>       // For std::put_time, std::setfill, std::setw
#include <sstream>       // For std::ostringstream
#include <ctime>         // For std::gmtime
#include <cctype>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <mutex>
#include <set>
#include <limits>
#include <cuda_runtime.h>
#include "json.hpp"      // For nlohmann::json
#include "fetch_generated.h" // For FetchGame:: enums and builders
#include "flatbuffers/flatbuffers.h" // For flatbuffers::FlatBufferBuilder

// --- Definitions of all functions previously in project.h ---

namespace {

std::string read_file_to_string(const std::string& path, std::string* error);

constexpr const char* kAppConfigSchemaId = "orange.app.config";
constexpr int kAppConfigSchemaVersion = 1;

std::string default_recording_root_for_orange_root(const std::string& orange_root_dir_str)
{
    return (std::filesystem::path(orange_root_dir_str) / "exp" / "unsorted").string();
}

std::string default_canonical_pointer_root_for_orange_root(const std::string& orange_root_dir_str)
{
    return (std::filesystem::path(orange_root_dir_str) / ".orange").string();
}

std::string default_run_pointer_path()
{
    return "/run/orange/latest_recording.json";
}

bool read_recording_snapshot_locked(const std::filesystem::path& snapshot_path,
                                    nlohmann::json* snapshot_out)
{
    if (!snapshot_out) {
        return false;
    }

    std::string error;
    std::string contents = read_file_to_string(snapshot_path.string(), &error);
    if (contents.empty()) {
        std::cerr << "Failed to read recording snapshot: " << snapshot_path.string()
                  << " (" << (error.empty() ? "empty file" : error) << ")" << std::endl;
        return false;
    }

    try {
        *snapshot_out = nlohmann::json::parse(contents);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to parse recording snapshot: " << snapshot_path.string()
                  << " (" << ex.what() << ")" << std::endl;
        return false;
    }

    if (!snapshot_out->is_object()) {
        *snapshot_out = nlohmann::json::object();
    }
    return true;
}

std::string trim_ascii_copy(std::string value)
{
    value.erase(
        value.begin(),
        std::find_if(value.begin(), value.end(), [](unsigned char c) {
            return !std::isspace(c);
        }));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
            return !std::isspace(c);
        }).base(),
        value.end());
    return value;
}

std::string normalize_snapshot_recording_sink_mode(std::string value)
{
    value = trim_ascii_copy(value);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value.empty() ? "real" : value;
}

std::string sanitize_env_suffix(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(std::toupper(ch)));
        } else {
            out.push_back('_');
        }
    }
    return out;
}

bool parse_nonnegative_int_token(const std::string& text, int* value_out)
{
    if (!value_out || text.empty()) {
        return false;
    }
    for (unsigned char ch : text) {
        if (!std::isdigit(ch)) {
            return false;
        }
    }

    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        value < 0 || value > std::numeric_limits<int>::max()) {
        return false;
    }
    *value_out = static_cast<int>(value);
    return true;
}

nlohmann::json parse_cpu_list_for_snapshot(const std::string& raw)
{
    nlohmann::json result = {
        {"parse_ok", true},
        {"cpus", nlohmann::json::array()},
        {"error", nullptr}
    };

    const std::string value = trim_ascii_copy(raw);
    if (value.empty()) {
        return result;
    }

    std::set<int> cpus;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::size_t end = comma == std::string::npos ? value.size() : comma;
        const std::string token = trim_ascii_copy(value.substr(start, end - start));
        if (token.empty()) {
            result["parse_ok"] = false;
            result["error"] = "empty token";
            return result;
        }

        const std::size_t dash = token.find('-');
        if (dash == std::string::npos) {
            int cpu = -1;
            if (!parse_nonnegative_int_token(token, &cpu)) {
                result["parse_ok"] = false;
                result["error"] = "invalid cpu token: " + token;
                return result;
            }
            cpus.insert(cpu);
        } else {
            const std::string first_text = trim_ascii_copy(token.substr(0, dash));
            const std::string last_text = trim_ascii_copy(token.substr(dash + 1));
            int first = -1;
            int last = -1;
            if (token.find('-', dash + 1) != std::string::npos ||
                !parse_nonnegative_int_token(first_text, &first) ||
                !parse_nonnegative_int_token(last_text, &last) ||
                first > last) {
                result["parse_ok"] = false;
                result["error"] = "invalid cpu range: " + token;
                return result;
            }
            for (int cpu = first; cpu <= last; ++cpu) {
                cpus.insert(cpu);
            }
        }

        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }

    for (const int cpu : cpus) {
        result["cpus"].push_back(cpu);
    }
    return result;
}

nlohmann::json parse_kernel_cmdline_options_for_snapshot(const std::string& raw)
{
    const std::set<std::string> keys = {"isolcpus", "nohz_full", "rcu_nocbs"};
    nlohmann::json options = nlohmann::json::object();
    std::istringstream stream(raw);
    std::string token;
    while (stream >> token) {
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = token.substr(0, equals);
        if (keys.find(key) != keys.end()) {
            options[key] = token.substr(equals + 1);
        }
    }
    return options;
}

nlohmann::json build_system_cpu_runtime_snapshot()
{
    std::string isolated_error;
    const std::string isolated_raw =
        trim_ascii_copy(read_file_to_string("/sys/devices/system/cpu/isolated", &isolated_error));
    nlohmann::json isolated = parse_cpu_list_for_snapshot(isolated_raw);
    isolated["source"] = "/sys/devices/system/cpu/isolated";
    isolated["available"] = isolated_error.empty();
    isolated["raw"] = isolated_raw;
    if (!isolated_error.empty()) {
        isolated["error"] = isolated_error;
    }

    std::string cmdline_error;
    const std::string cmdline_raw =
        trim_ascii_copy(read_file_to_string("/proc/cmdline", &cmdline_error));
    nlohmann::json cmdline = {
        {"source", "/proc/cmdline"},
        {"available", cmdline_error.empty()},
        {"raw", cmdline_raw},
        {"options", parse_kernel_cmdline_options_for_snapshot(cmdline_raw)}
    };
    if (!cmdline_error.empty()) {
        cmdline["error"] = cmdline_error;
    }

    return {
        {"schema_version", 1},
        {"isolated_cpus", isolated},
        {"kernel_cmdline", cmdline}
    };
}

nlohmann::json build_yolo_worker_runtime_snapshot(const CameraParams* cameras_params,
                                                  int num_cameras)
{
    nlohmann::json affinity_by_camera = nlohmann::json::object();
    const char* global_affinity = std::getenv("ORANGE_YOLO_AFFINITY");
    for (int i = 0; i < num_cameras; ++i) {
        const CameraParams& params = cameras_params[i];
        std::string camera_key = params.camera_serial.empty()
            ? std::to_string(params.camera_id)
            : params.camera_serial;
        const std::string env_key =
            "ORANGE_YOLO_AFFINITY_CAM_" + sanitize_env_suffix(camera_key);
        const char* per_camera_affinity = std::getenv(env_key.c_str());

        nlohmann::json affinity = {
            {"configured", false},
            {"source", "none"},
            {"env_key", nullptr},
            {"requested_cpus", nullptr}
        };
        if (per_camera_affinity && *per_camera_affinity) {
            affinity["configured"] = true;
            affinity["source"] = "per_camera_environment";
            affinity["env_key"] = env_key;
            affinity["requested_cpus"] = per_camera_affinity;
        } else if (global_affinity && *global_affinity) {
            affinity["configured"] = true;
            affinity["source"] = "global_environment";
            affinity["env_key"] = "ORANGE_YOLO_AFFINITY";
            affinity["requested_cpus"] = global_affinity;
        }
        affinity_by_camera[camera_key] = affinity;
    }

    return {
        {"schema_version", 1},
        {"affinity", {
            {"source", "environment"},
            {"per_camera", affinity_by_camera}
        }}
    };
}

std::string shell_single_quote(const std::string& value)
{
    std::string out = "'";
    for (const char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

bool parse_uid_gid_from_env(uid_t* uid_out, gid_t* gid_out)
{
    if (!uid_out || !gid_out) {
        return false;
    }
    const char* uid_env = std::getenv("SUDO_UID");
    const char* gid_env = std::getenv("SUDO_GID");
    if (!uid_env || !gid_env) {
        return false;
    }

    char* end = nullptr;
    errno = 0;
    const unsigned long uid_ul = std::strtoul(uid_env, &end, 10);
    if (errno != 0 || end == uid_env || *end != '\0') {
        return false;
    }
    errno = 0;
    const unsigned long gid_ul = std::strtoul(gid_env, &end, 10);
    if (errno != 0 || end == gid_env || *end != '\0') {
        return false;
    }
    *uid_out = static_cast<uid_t>(uid_ul);
    *gid_out = static_cast<gid_t>(gid_ul);
    return true;
}

std::string run_shell_command_as_user(const std::string& command,
                                      const uid_t uid,
                                      const gid_t gid,
                                      int* exit_status_out)
{
    constexpr std::size_t kMaxCapturedOutputBytes = 8192;
    if (exit_status_out) {
        *exit_status_out = -1;
    }
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        return {};
    }

    const pid_t child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return {};
    }

    if (child == 0) {
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(pipe_fds[1]);
        if (setgid(gid) != 0 || setuid(uid) != 0) {
            _exit(127);
        }
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close(pipe_fds[1]);
    std::array<char, 256> buffer{};
    std::string output;
    while (true) {
        const ssize_t bytes =
            read(pipe_fds[0], buffer.data(), buffer.size());
        if (bytes > 0) {
            if (output.size() < kMaxCapturedOutputBytes) {
                const std::size_t remaining =
                    kMaxCapturedOutputBytes - output.size();
                output.append(
                    buffer.data(),
                    std::min<std::size_t>(
                        remaining,
                        static_cast<std::size_t>(bytes)));
            }
            continue;
        }
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(pipe_fds[0]);

    int status = -1;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (exit_status_out) {
        *exit_status_out = status;
    }
    return trim_ascii_copy(output);
}

std::string run_command_capture_stdout(const std::string& command,
                                       int* exit_status_out = nullptr)
{
    constexpr std::size_t kMaxCapturedOutputBytes = 8192;
    if (exit_status_out) {
        *exit_status_out = -1;
    }

    uid_t sudo_uid = 0;
    gid_t sudo_gid = 0;
    if (geteuid() == 0 &&
        parse_uid_gid_from_env(&sudo_uid, &sudo_gid) &&
        (sudo_uid != 0 || sudo_gid != 0)) {
        return run_shell_command_as_user(command, sudo_uid, sudo_gid, exit_status_out);
    }

    std::array<char, 256> buffer{};
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return {};
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        if (output.size() < kMaxCapturedOutputBytes) {
            const std::size_t remaining =
                kMaxCapturedOutputBytes - output.size();
            output.append(buffer.data(), std::min<std::size_t>(
                remaining,
                std::strlen(buffer.data())));
        }
    }
    const int status = pclose(pipe);
    if (exit_status_out) {
        *exit_status_out = status;
    }
    return trim_ascii_copy(output);
}

std::filesystem::path find_git_worktree_from(const std::filesystem::path& start)
{
    if (start.empty()) {
        return {};
    }
    std::error_code ec;
    std::filesystem::path current = std::filesystem::absolute(start, ec);
    if (ec) {
        current = start;
    }
    if (!std::filesystem::is_directory(current, ec)) {
        current = current.parent_path();
    }
    while (!current.empty()) {
        if (std::filesystem::exists(current / ".git", ec) && !ec) {
            return current;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return {};
}

std::filesystem::path resolve_source_worktree()
{
    if (const char* env = std::getenv("ORANGE_SOURCE_WORKTREE"); env && *env) {
        const std::filesystem::path env_path = find_git_worktree_from(env);
        if (!env_path.empty()) {
            return env_path;
        }
    }

    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec) {
        const std::filesystem::path cwd_worktree = find_git_worktree_from(cwd);
        if (!cwd_worktree.empty()) {
            return cwd_worktree;
        }
    }

    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        const std::filesystem::path exe_worktree = find_git_worktree_from(exe);
        if (!exe_worktree.empty()) {
            return exe_worktree;
        }
    }
    return {};
}

std::filesystem::path resolve_git_dir(const std::filesystem::path& worktree)
{
    if (worktree.empty()) {
        return {};
    }
    const std::filesystem::path dot_git = worktree / ".git";
    std::error_code ec;
    if (std::filesystem::is_directory(dot_git, ec) && !ec) {
        return dot_git;
    }
    if (std::filesystem::is_regular_file(dot_git, ec) && !ec) {
        std::string error;
        const std::string contents = read_file_to_string(dot_git.string(), &error);
        const std::string prefix = "gitdir:";
        if (contents.rfind(prefix, 0) == 0) {
            std::string value = trim_ascii_copy(contents.substr(prefix.size()));
            if (!value.empty()) {
                std::filesystem::path gitdir(value);
                if (gitdir.is_relative()) {
                    gitdir = worktree / gitdir;
                }
                return gitdir;
            }
        }
    }
    return {};
}

std::string read_git_ref_commit(const std::filesystem::path& git_dir,
                                const std::string& ref)
{
    if (git_dir.empty() || ref.empty()) {
        return {};
    }
    std::string error;
    const std::string loose_ref =
        trim_ascii_copy(read_file_to_string((git_dir / ref).string(), &error));
    if (!loose_ref.empty()) {
        return loose_ref;
    }

    error.clear();
    const std::string packed_refs =
        read_file_to_string((git_dir / "packed-refs").string(), &error);
    std::istringstream input(packed_refs);
    std::string line;
    while (std::getline(input, line)) {
        line = trim_ascii_copy(line);
        if (line.empty() || line[0] == '#' || line[0] == '^') {
            continue;
        }
        std::istringstream fields(line);
        std::string commit;
        std::string packed_ref;
        fields >> commit >> packed_ref;
        if (packed_ref == ref) {
            return commit;
        }
    }
    return {};
}

nlohmann::json read_git_head_direct(const std::filesystem::path& worktree)
{
    nlohmann::json out = nlohmann::json::object();
    const std::filesystem::path git_dir = resolve_git_dir(worktree);
    if (git_dir.empty()) {
        return out;
    }

    std::string error;
    const std::string head =
        trim_ascii_copy(read_file_to_string((git_dir / "HEAD").string(), &error));
    if (head.empty()) {
        return out;
    }

    if (head.rfind("ref:", 0) == 0) {
        const std::string ref = trim_ascii_copy(head.substr(4));
        const std::string heads_prefix = "refs/heads/";
        out["ref"] = ref;
        if (ref.rfind(heads_prefix, 0) == 0) {
            out["branch"] = ref.substr(heads_prefix.size());
        }
        const std::string commit = read_git_ref_commit(git_dir, ref);
        if (!commit.empty()) {
            out["commit"] = commit;
            out["commit_short"] = commit.substr(0, std::min<std::size_t>(12, commit.size()));
        }
    } else {
        out["commit"] = head;
        out["commit_short"] = head.substr(0, std::min<std::size_t>(12, head.size()));
        out["detached_head"] = true;
    }
    return out;
}

nlohmann::json build_source_version_snapshot()
{
    nlohmann::json source = {
        {"schema_version", 1},
        {"captured_at_utc", get_current_utc_timestamp()},
        {"vcs", "git"},
        {"available", false}
    };

    const std::filesystem::path worktree = resolve_source_worktree();
    if (worktree.empty()) {
        source["error"] = "git worktree not found from cwd or executable path";
        return source;
    }

    const std::string worktree_string = worktree.string();
    const nlohmann::json direct_head = read_git_head_direct(worktree);
    const std::string git_prefix = "git -C " + shell_single_quote(worktree_string) + " ";
    int top_level_status = -1;
    int commit_status = -1;
    int commit_short_status = -1;
    int branch_status = -1;
    int describe_status = -1;
    int tracked_status_status = -1;
    const std::string top_level =
        run_command_capture_stdout(git_prefix + "rev-parse --show-toplevel 2>/dev/null",
                                   &top_level_status);
    std::string commit =
        run_command_capture_stdout(git_prefix + "rev-parse HEAD 2>/dev/null",
                                   &commit_status);
    std::string commit_short =
        run_command_capture_stdout(git_prefix + "rev-parse --short HEAD 2>/dev/null",
                                   &commit_short_status);
    std::string branch =
        run_command_capture_stdout(git_prefix + "rev-parse --abbrev-ref HEAD 2>/dev/null",
                                   &branch_status);
    const std::string describe =
        run_command_capture_stdout(git_prefix + "describe --always --dirty --tags 2>/dev/null",
                                   &describe_status);
    const std::string tracked_status =
        run_command_capture_stdout(git_prefix + "status --porcelain --untracked-files=no 2>/dev/null",
                                   &tracked_status_status);

    if (commit.empty() && direct_head.contains("commit") && direct_head["commit"].is_string()) {
        commit = direct_head["commit"].get<std::string>();
    }
    if (commit_short.empty() &&
        direct_head.contains("commit_short") &&
        direct_head["commit_short"].is_string()) {
        commit_short = direct_head["commit_short"].get<std::string>();
    }
    if ((branch.empty() || branch == "HEAD") &&
        direct_head.contains("branch") &&
        direct_head["branch"].is_string()) {
        branch = direct_head["branch"].get<std::string>();
    }

    source["available"] = !commit.empty();
    source["git_command_available"] =
        top_level_status == 0 || commit_status == 0 || commit_short_status == 0 ||
        branch_status == 0 || describe_status == 0;
    if (geteuid() == 0) {
        uid_t sudo_uid = 0;
        gid_t sudo_gid = 0;
        if (parse_uid_gid_from_env(&sudo_uid, &sudo_gid) &&
            (sudo_uid != 0 || sudo_gid != 0)) {
            source["git_command_user"] = {
                {"mode", "sudo_invoking_user"},
                {"uid", static_cast<uint64_t>(sudo_uid)},
                {"gid", static_cast<uint64_t>(sudo_gid)}
            };
        } else {
            source["git_command_user"] = {
                {"mode", "process_euid"},
                {"uid", static_cast<uint64_t>(geteuid())}
            };
        }
    } else {
        source["git_command_user"] = {
            {"mode", "process_euid"},
            {"uid", static_cast<uint64_t>(geteuid())}
        };
    }
    source["worktree"] = top_level.empty() ? worktree_string : top_level;
    if (!branch.empty()) {
        source["branch"] = branch;
    }
    if (!commit.empty()) {
        source["commit"] = commit;
    }
    if (!commit_short.empty()) {
        source["commit_short"] = commit_short;
    }
    if (!describe.empty()) {
        source["describe"] = describe;
    }
    source["dirty_tracked_available"] = tracked_status_status == 0;
    if (tracked_status_status == 0) {
        source["dirty_tracked"] = !tracked_status.empty();
        source["status_porcelain_tracked"] = tracked_status;
    }
    return source;
}

bool snapshot_sink_writes_full_output(const std::string& sink_mode)
{
    return sink_mode == "real" || sink_mode == "external_ipc";
}

bool read_nonnegative_int_field(const nlohmann::json& object,
                                const char* key,
                                int* value_out,
                                std::string* error_out,
                                const std::string& context)
{
    if (!object.contains(key)) {
        return true;
    }
    if (!object[key].is_number_integer()) {
        if (error_out) {
            *error_out = context + "." + key + " must be an integer";
        }
        return false;
    }
    const int64_t value = object[key].get<int64_t>();
    if (value < 0 || value > std::numeric_limits<int>::max()) {
        if (error_out) {
            *error_out = context + "." + key + " must be a non-negative int";
        }
        return false;
    }
    if (value_out) {
        *value_out = static_cast<int>(value);
    }
    return true;
}

std::string snapshot_recording_output_backend(const std::string& sink_mode)
{
    if (sink_mode == "external_ipc") {
        return "external_ipc";
    }
    if (sink_mode == "real") {
        return "in_process";
    }
    return sink_mode.empty() ? "unknown" : sink_mode;
}

std::string snapshot_camera_serial(const CameraParams& params)
{
    if (!params.camera_serial.empty()) {
        return params.camera_serial;
    }
    return std::to_string(params.camera_id);
}

nlohmann::json build_initial_recording_outputs_snapshot(
    const CameraParams* cameras_params,
    const int num_cameras,
    const std::string& recording_sink_mode)
{
    nlohmann::json outputs = nlohmann::json::object();
    const std::string sink_mode = normalize_snapshot_recording_sink_mode(recording_sink_mode);
    const bool writes_full_output = snapshot_sink_writes_full_output(sink_mode);
    const std::string backend = snapshot_recording_output_backend(sink_mode);

    for (int i = 0; i < num_cameras; ++i) {
        const CameraParams& params = cameras_params[i];
        const std::string camera_serial = snapshot_camera_serial(params);
        if (camera_serial.empty()) {
            continue;
        }

        nlohmann::json full_output = {
            {"schema_version", 1},
            {"camera_serial", camera_serial},
            {"output_kind", "full"},
            {"role", "ingest_authoritative"},
            {"backend", backend},
            {"status", writes_full_output ? "pending" : "disabled"},
            {"width", params.width},
            {"height", params.height},
            {"frame_rate", params.frame_rate},
            {"container", writes_full_output ? "mp4" : "none"},
            {"coordinate_space", "full_frame_pixels"}
        };
        if (sink_mode == "real") {
            const std::string prefix = "Cam" + camera_serial;
            full_output["video"] = prefix + ".mp4";
            full_output["metadata"] = prefix + "_meta.csv";
            full_output["keyframes"] = prefix + "_keyframe.json";
        } else if (sink_mode == "external_ipc") {
            full_output["details"] = {
                {"path_resolution", "finalized_recording_session_manifest"},
                {"note", "external recorder paths are finalized after recorder shutdown"}
            };
        }

        outputs[camera_serial] = {{"full", full_output}};
    }
    return outputs;
}

nlohmann::json build_crop_recording_output_from_snapshot(
    const std::string& camera_serial,
    const nlohmann::json& crop_output_info)
{
    if (camera_serial.empty() ||
        !crop_output_info.is_object() ||
        !crop_output_info.value("enabled", false)) {
        return nullptr;
    }

    const nlohmann::json runtime =
        crop_output_info.value("runtime", nlohmann::json::object());
    const nlohmann::json files =
        runtime.value("files", nlohmann::json::object());

    nlohmann::json output = {
        {"schema_version", 1},
        {"camera_serial", camera_serial},
        {"output_kind", "crop"},
        {"role", "sidecar"},
        {"backend", "in_process"},
        {"status", "pending"},
        {"width", runtime.value("width", 0)},
        {"height", runtime.value("height", 0)},
        {"frame_rate", runtime.value("frame_rate", 0)},
        {"codec", runtime.value("codec", std::string("hevc"))},
        {"container", runtime.value("container", std::string("mp4"))},
        {"tuning", runtime.value("tuning", std::string("lossless"))},
        {"pixel_source_format", "mono8"},
        {"encoded_format", "nv12"},
        {"coordinate_space", runtime.value("coordinate_space", std::string("full_frame_pixels"))},
        {"details",
         {
             {"mode", crop_output_info.value("mode", std::string("yolo_centered_square"))},
             {"selection_policy",
              runtime.value("selection_policy", std::string("largest_detection_by_confidence"))},
             {"blank_frame_policy",
              runtime.value("blank_frame_policy", std::string("encode_black_frame_when_no_detection"))}
         }}
    };
    if (files.is_object()) {
        const std::map<std::string, const char*> file_keys = {
            {"video", "video"},
            {"metadata", "metadata"},
            {"keyframes", "keyframes"},
            {"perf", "perf"},
            {"sidecar_perf", "sidecar_perf"}
        };
        for (const auto& item : file_keys) {
            if (files.contains(item.first) && files[item.first].is_string()) {
                output[item.second] = files[item.first].get<std::string>();
            }
        }
    }
    return output;
}

bool app_config_schema_version_supported(int schema_version)
{
    return schema_version == kAppConfigSchemaVersion;
}

} // namespace

std::string build_default_orange_root_dir(std::string* warning_out)
{
    if (warning_out) {
        warning_out->clear();
    }

    auto build_for_home = [](const std::string& home_dir) -> std::string {
        if (home_dir.empty()) {
            return std::string();
        }
        return (std::filesystem::path(home_dir) / "orange_data").string();
    };

    const char* sudo_user = std::getenv("SUDO_USER");
    if (sudo_user && sudo_user[0] != '\0') {
        if (passwd* pw = getpwnam(sudo_user)) {
            if (pw->pw_dir && pw->pw_dir[0] != '\0') {
                return build_for_home(pw->pw_dir);
            }
        } else if (warning_out) {
            *warning_out = std::string("Failed to resolve SUDO_USER home for `") + sudo_user + "`";
        }
    }

    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return build_for_home(home);
    }

    if (passwd* pw = getpwuid(getuid())) {
        if (pw->pw_dir && pw->pw_dir[0] != '\0') {
            return build_for_home(pw->pw_dir);
        }
    }

    if (warning_out) {
        *warning_out = "Failed to resolve Orange data root from runtime environment";
    }
    return std::string();
}

std::string build_default_app_config_path(const std::string& orange_root_dir_str)
{
    const char* app_config_override = std::getenv("ORANGE_APP_CONFIG_PATH");
    if (app_config_override && app_config_override[0] != '\0') {
        return trim_ascii_copy(app_config_override);
    }
    const char* gui_app_config_override = std::getenv("ORANGE_GUI_APP_CONFIG_PATH");
    if (gui_app_config_override && gui_app_config_override[0] != '\0') {
        return trim_ascii_copy(gui_app_config_override);
    }
    return (std::filesystem::path(orange_root_dir_str) / "config" / "app" / "default.json").string();
}

static bool read_optional_bounded_int_field(const nlohmann::json& object,
                                            const char* field_name,
                                            int* value_out,
                                            const int min_value,
                                            const int max_value,
                                            std::string* error_out,
                                            const std::string& context)
{
    if (!value_out || !object.contains(field_name) || object[field_name].is_null()) {
        return true;
    }
    if (!object[field_name].is_number_integer()) {
        if (error_out) {
            *error_out = context + "." + field_name + " must be an integer";
        }
        return false;
    }
    const int value = object[field_name].get<int>();
    if (value < min_value || value > max_value) {
        if (error_out) {
            *error_out = context + "." + field_name + " must be in [" +
                         std::to_string(min_value) + "," + std::to_string(max_value) + "]";
        }
        return false;
    }
    *value_out = value;
    return true;
}

static bool read_optional_nonnegative_int_map_field(const nlohmann::json& object,
                                                    const char* field_name,
                                                    std::map<std::string, int>* values_out,
                                                    const int max_value,
                                                    std::string* error_out,
                                                    const std::string& context)
{
    if (!values_out || !object.contains(field_name) || object[field_name].is_null()) {
        return true;
    }
    if (!object[field_name].is_object()) {
        if (error_out) {
            *error_out = context + "." + field_name + " must be an object";
        }
        return false;
    }

    const nlohmann::json& values = object[field_name];
    for (auto it = values.begin(); it != values.end(); ++it) {
        const std::string key = trim_ascii_copy(it.key());
        if (key.empty()) {
            if (error_out) {
                *error_out = context + "." + field_name +
                             " must not contain an empty serial key";
            }
            return false;
        }
        if (!it.value().is_number_integer()) {
            if (error_out) {
                *error_out = context + "." + field_name + "." + key +
                             " must be an integer";
            }
            return false;
        }
        const int value = it.value().get<int>();
        if (value < 0 || value > max_value) {
            if (error_out) {
                *error_out = context + "." + field_name + "." + key +
                             " must be in [0," + std::to_string(max_value) + "]";
            }
            return false;
        }
        (*values_out)[key] = value;
    }
    return true;
}

static bool read_optional_bool_field(const nlohmann::json& object,
                                     const char* field_name,
                                     bool* value_out,
                                     std::string* error_out,
                                     const std::string& context)
{
    if (!value_out || !object.contains(field_name) || object[field_name].is_null()) {
        return true;
    }
    if (!object[field_name].is_boolean()) {
        if (error_out) {
            *error_out = context + "." + field_name + " must be a boolean";
        }
        return false;
    }
    *value_out = object[field_name].get<bool>();
    return true;
}

static bool read_optional_gui_stream_downsample_field(const nlohmann::json& object,
                                                      const char* field_name,
                                                      int* value_out,
                                                      std::string* error_out,
                                                      const std::string& context)
{
    if (!value_out || !object.contains(field_name) || object[field_name].is_null()) {
        return true;
    }
    if (!object[field_name].is_number_integer()) {
        if (error_out) {
            *error_out = context + "." + field_name + " must be an integer";
        }
        return false;
    }
    const int value = object[field_name].get<int>();
    static constexpr std::array<int, 5> kAllowed = {1, 2, 4, 8, 16};
    if (std::find(kAllowed.begin(), kAllowed.end(), value) == kAllowed.end()) {
        if (error_out) {
            *error_out =
                context + "." + field_name + " must be one of 1, 2, 4, 8, or 16";
        }
        return false;
    }
    *value_out = value;
    return true;
}

static std::string normalize_app_crop_recording_sink_mode(std::string value)
{
    value = trim_ascii_copy(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (value.empty() || value == "real" || value == "in_process" || value == "inprocess") {
        return "in_process";
    }
    if (value == "external_ipc") {
        return value;
    }
    return std::string();
}

static std::string normalize_camera_preferred_recording_sink_mode(std::string value)
{
    value = trim_ascii_copy(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (value.empty() || value == "default" || value == "auto" || value == "app") {
        return {};
    }
    if (value == "real" || value == "external_ipc") {
        return value;
    }
    return {};
}

static bool apply_app_config_display_profile(AppStorageConfig* config,
                                             const std::string& profile,
                                             std::string* error_out,
                                             const std::string& config_path)
{
    if (!config) {
        return false;
    }
    std::string normalized = trim_ascii_copy(profile);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    config->gui_display_profile = normalized;
    if (normalized.empty() || normalized == "default") {
        return true;
    }
    if (normalized == "fast") {
        config->gui_display_preview_max_fps = 15;
        config->gui_swap_interval = 0;
        config->gui_frame_max_fps = 60;
        return true;
    }
    if (normalized == "citrus_safe" || normalized == "citrus-safe") {
        config->gui_display_profile = "citrus_safe";
        config->gui_display_preview_max_fps = 10;
        config->gui_swap_interval = 1;
        config->gui_frame_max_fps = 30;
        return true;
    }
    if (error_out) {
        *error_out =
            "gui.display.profile must be default, fast, or citrus_safe in " + config_path;
    }
    return false;
}

bool load_app_storage_config(const std::string& orange_root_dir_str,
                             AppStorageConfig* config_out,
                             std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!config_out) {
        if (error_out) {
            *error_out = "Internal error: null app storage config destination";
        }
        return false;
    }

    AppStorageConfig config;
    config.schema_id = kAppConfigSchemaId;
    config.schema_version = kAppConfigSchemaVersion;
    config.default_detect_engine.clear();
    config.default_recording_root = default_recording_root_for_orange_root(orange_root_dir_str);
    config.gui_recording_sink_mode = "real";
    config.gui_recording_sink_mode_configured = false;
    config.gui_recording_record_for_seconds = 0;
    config.gui_recording_clip_seconds = 0;
    config.gui_crop_recording_sink_mode = "in_process";
    config.gui_crop_external_encode_queue_depth = -1;
    config.gui_crop_external_recorder_gpu_id = -1;
    config.gui_crop_external_recorder_gpu_ids_by_serial.clear();
    config.gui_crop_frame_pool_size = -1;
    config.gui_external_recorder_contract_path.clear();
    config.gui_external_recorder_contract = nlohmann::json::object();
    config.gui_ptp_register_read_decimate = 1;
    config.gui_stream_downsample = -1;
    config.gui_display_profile.clear();
    config.gui_display_preview_max_fps = -1;
    config.gui_swap_interval = -1;
    config.gui_frame_max_fps = -1;
    config.gui_show_speed_graphs = false;
    config.gui_local_control_recording_start_enabled = false;
    config.gui_local_control_recording_stop_enabled = false;
    config.gui_local_control_citrus_completion_stop_enabled = false;
    config.gui_local_control_exit_after_finalize = false;
    config.gui_local_control_drain_timeout_seconds = -1;
    config.write_local_pointer = true;
    config.canonical_pointer_root = default_canonical_pointer_root_for_orange_root(orange_root_dir_str);
    config.write_run_pointer = true;
    config.run_pointer_path = default_run_pointer_path();

    const std::filesystem::path config_path(build_default_app_config_path(orange_root_dir_str));
    if (!std::filesystem::exists(config_path)) {
        *config_out = std::move(config);
        return true;
    }

    std::ifstream input(config_path);
    if (!input.is_open()) {
        if (error_out) {
            *error_out = "Failed to open app config: " + config_path.string();
        }
        return false;
    }

    nlohmann::json root;
    try {
        input >> root;
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = "Failed to parse app config " + config_path.string() + ": " + ex.what();
        }
        return false;
    }

    if (!root.is_object()) {
        if (error_out) {
            *error_out = "App config root must be a JSON object: " + config_path.string();
        }
        return false;
    }

    const std::string schema_id = root.value("schema_id", std::string());
    const int schema_version = root.value("schema_version", 0);
    if (!schema_id.empty() && schema_id != kAppConfigSchemaId) {
        if (error_out) {
            *error_out = "App config schema_id mismatch for " + config_path.string() +
                         ": " + schema_id + " (expected " + kAppConfigSchemaId + ")";
        }
        return false;
    }
    if (schema_version > 0 && !app_config_schema_version_supported(schema_version)) {
        if (error_out) {
            *error_out = "App config schema_version mismatch for " + config_path.string() +
                         ": " + std::to_string(schema_version) +
                         " (expected " + std::to_string(kAppConfigSchemaVersion) + ")";
        }
        return false;
    }

    config.schema_id = schema_id.empty() ? kAppConfigSchemaId : schema_id;
    config.schema_version = (schema_version <= 0) ? kAppConfigSchemaVersion : schema_version;

    if (root.contains("models")) {
        if (!root["models"].is_object()) {
            if (error_out) {
                *error_out = "models must be an object in " + config_path.string();
            }
            return false;
        }

        const nlohmann::json& models = root["models"];
        if (models.contains("default_detect_engine")) {
            if (!models["default_detect_engine"].is_string()) {
                if (error_out) {
                    *error_out = "models.default_detect_engine must be a string in " +
                                 config_path.string();
                }
                return false;
            }
            config.default_detect_engine =
                trim_ascii_copy(models["default_detect_engine"].get<std::string>());
        }
    }

    if (root.contains("recording")) {
        if (!root["recording"].is_object()) {
            if (error_out) {
                *error_out = "recording must be an object in " + config_path.string();
            }
            return false;
        }

        const nlohmann::json& recording = root["recording"];
        if (recording.contains("sink_mode")) {
            if (!recording["sink_mode"].is_string()) {
                if (error_out) {
                    *error_out = "recording.sink_mode must be a string in " +
                                 config_path.string();
                }
                return false;
            }
            const std::string sink_mode =
                trim_ascii_copy(recording["sink_mode"].get<std::string>());
            if (!sink_mode.empty()) {
                config.gui_recording_sink_mode = sink_mode;
                config.gui_recording_sink_mode_configured = true;
            }
        }
        if (recording.contains("recording_control")) {
            if (!recording["recording_control"].is_object()) {
                if (error_out) {
                    *error_out = "recording.recording_control must be an object in " +
                                 config_path.string();
                }
                return false;
            }
            const nlohmann::json& control = recording["recording_control"];
            if (!read_nonnegative_int_field(
                    control,
                    "record_for_seconds",
                    &config.gui_recording_record_for_seconds,
                    error_out,
                    "recording.recording_control")) {
                if (error_out && error_out->find(config_path.string()) == std::string::npos) {
                    *error_out += " in " + config_path.string();
                }
                return false;
            }
            if (!read_nonnegative_int_field(
                    control,
                    "clip_seconds",
                    &config.gui_recording_clip_seconds,
                    error_out,
                    "recording.recording_control")) {
                if (error_out && error_out->find(config_path.string()) == std::string::npos) {
                    *error_out += " in " + config_path.string();
                }
                return false;
            }
            if (config.gui_recording_clip_seconds > 0 &&
                config.gui_recording_record_for_seconds <= 0) {
                if (error_out) {
                    *error_out =
                        "recording.recording_control.clip_seconds requires "
                        "record_for_seconds > 0 in " +
                        config_path.string();
                }
                return false;
            }
        }
        if (recording.contains("crop")) {
            if (!recording["crop"].is_object()) {
                if (error_out) {
                    *error_out = "recording.crop must be an object in " + config_path.string();
                }
                return false;
            }
            const nlohmann::json& crop = recording["crop"];
            if (crop.contains("sink_mode")) {
                if (!crop["sink_mode"].is_string()) {
                    if (error_out) {
                        *error_out = "recording.crop.sink_mode must be a string in " +
                                     config_path.string();
                    }
                    return false;
                }
                const std::string normalized =
                    normalize_app_crop_recording_sink_mode(
                        crop["sink_mode"].get<std::string>());
                if (normalized.empty()) {
                    if (error_out) {
                        *error_out =
                            "recording.crop.sink_mode must be real, in_process, or "
                            "external_ipc in " +
                            config_path.string();
                    }
                    return false;
                }
                config.gui_crop_recording_sink_mode = normalized;
            }
            if (!read_optional_bounded_int_field(
                    crop,
                    "frame_pool_size",
                    &config.gui_crop_frame_pool_size,
                    1,
                    512,
                    error_out,
                    "recording.crop")) {
                if (error_out && error_out->find(config_path.string()) == std::string::npos) {
                    *error_out += " in " + config_path.string();
                }
                return false;
            }
            if (crop.contains("external_ipc")) {
                if (!crop["external_ipc"].is_object()) {
                    if (error_out) {
                        *error_out =
                            "recording.crop.external_ipc must be an object in " +
                            config_path.string();
                    }
                    return false;
                }
                const nlohmann::json& external_ipc = crop["external_ipc"];
                if (!read_optional_bounded_int_field(
                        external_ipc,
                        "encode_queue_depth",
                        &config.gui_crop_external_encode_queue_depth,
                        1,
                        4096,
                        error_out,
                        "recording.crop.external_ipc")) {
                    if (error_out &&
                        error_out->find(config_path.string()) == std::string::npos) {
                        *error_out += " in " + config_path.string();
                    }
                    return false;
                }
                if (!read_optional_bounded_int_field(
                        external_ipc,
                        "recorder_gpu_id",
                        &config.gui_crop_external_recorder_gpu_id,
                        0,
                        255,
                        error_out,
                        "recording.crop.external_ipc")) {
                    if (error_out &&
                        error_out->find(config_path.string()) == std::string::npos) {
                        *error_out += " in " + config_path.string();
                    }
                    return false;
                }
                if (!read_optional_nonnegative_int_map_field(
                        external_ipc,
                        "recorder_gpu_ids_by_serial",
                        &config.gui_crop_external_recorder_gpu_ids_by_serial,
                        255,
                        error_out,
                        "recording.crop.external_ipc")) {
                    if (error_out &&
                        error_out->find(config_path.string()) == std::string::npos) {
                        *error_out += " in " + config_path.string();
                    }
                    return false;
                }
            }
        }
        if (recording.contains("external_recorder_contract")) {
            const nlohmann::json& contract = recording["external_recorder_contract"];
            if (contract.is_string()) {
                config.gui_external_recorder_contract_path =
                    trim_ascii_copy(contract.get<std::string>());
            } else if (contract.is_object()) {
                config.gui_external_recorder_contract = contract;
            } else {
                if (error_out) {
                    *error_out =
                        "recording.external_recorder_contract must be an object or string path in " +
                        config_path.string();
                }
                return false;
            }
        }
        if (recording.contains("external_recorder_contract_path")) {
            if (!recording["external_recorder_contract_path"].is_string()) {
                if (error_out) {
                    *error_out =
                        "recording.external_recorder_contract_path must be a string in " +
                        config_path.string();
                }
                return false;
            }
            config.gui_external_recorder_contract_path =
                trim_ascii_copy(recording["external_recorder_contract_path"].get<std::string>());
        }
        if (recording.contains("ptp_register_read_decimate")) {
            if (!recording["ptp_register_read_decimate"].is_number_integer()) {
                if (error_out) {
                    *error_out =
                        "recording.ptp_register_read_decimate must be an integer in " +
                        config_path.string();
                }
                return false;
            }
            const int decimate = recording["ptp_register_read_decimate"].get<int>();
            if (decimate < 1) {
                if (error_out) {
                    *error_out =
                        "recording.ptp_register_read_decimate must be >= 1 in " +
                        config_path.string();
                }
                return false;
            }
            config.gui_ptp_register_read_decimate = decimate;
        }
    }

    if (root.contains("gui")) {
        if (!root["gui"].is_object()) {
            if (error_out) {
                *error_out = "gui must be an object in " + config_path.string();
            }
            return false;
        }
        const nlohmann::json& gui = root["gui"];
        if (gui.contains("stream")) {
            if (!gui["stream"].is_object()) {
                if (error_out) {
                    *error_out = "gui.stream must be an object in " + config_path.string();
                }
                return false;
            }
            const nlohmann::json& stream = gui["stream"];
            if (!read_optional_gui_stream_downsample_field(
                    stream,
                    "downsample",
                    &config.gui_stream_downsample,
                    error_out,
                    "gui.stream")) {
                if (error_out && error_out->find(config_path.string()) == std::string::npos) {
                    *error_out += " in " + config_path.string();
                }
                return false;
            }
        }
        if (gui.contains("display")) {
            if (!gui["display"].is_object()) {
                if (error_out) {
                    *error_out = "gui.display must be an object in " + config_path.string();
                }
                return false;
            }
            const nlohmann::json& display = gui["display"];
            if (display.contains("profile") && !display["profile"].is_null()) {
                if (!display["profile"].is_string()) {
                    if (error_out) {
                        *error_out =
                            "gui.display.profile must be a string in " + config_path.string();
                    }
                    return false;
                }
                if (!apply_app_config_display_profile(
                        &config,
                        display["profile"].get<std::string>(),
                        error_out,
                        config_path.string())) {
                    return false;
                }
            }
            if (!read_optional_bounded_int_field(
                    display,
                    "display_preview_max_fps",
                    &config.gui_display_preview_max_fps,
                    0,
                    10000,
                    error_out,
                    "gui.display")) {
                if (error_out && error_out->find(config_path.string()) == std::string::npos) {
                    *error_out += " in " + config_path.string();
                }
                return false;
            }
            if (!read_optional_bounded_int_field(
                    display,
                    "swap_interval",
                    &config.gui_swap_interval,
                    0,
                    4,
                    error_out,
                    "gui.display")) {
                if (error_out && error_out->find(config_path.string()) == std::string::npos) {
                    *error_out += " in " + config_path.string();
                }
                return false;
            }
            if (!read_optional_bounded_int_field(
                    display,
                    "frame_max_fps",
                    &config.gui_frame_max_fps,
                    0,
                    1000,
                    error_out,
                    "gui.display")) {
                if (error_out && error_out->find(config_path.string()) == std::string::npos) {
                    *error_out += " in " + config_path.string();
                }
                return false;
            }
        }
        if (gui.contains("telemetry")) {
            if (!gui["telemetry"].is_object()) {
                if (error_out) {
                    *error_out = "gui.telemetry must be an object in " + config_path.string();
                }
                return false;
            }
            const nlohmann::json& telemetry = gui["telemetry"];
            if (!read_optional_bool_field(
                    telemetry,
                    "show_speed_graphs",
                    &config.gui_show_speed_graphs,
                    error_out,
                    "gui.telemetry")) {
                if (error_out && error_out->find(config_path.string()) == std::string::npos) {
                    *error_out += " in " + config_path.string();
                }
                return false;
            }
        }
        if (gui.contains("local_control")) {
            if (!gui["local_control"].is_object()) {
                if (error_out) {
                    *error_out = "gui.local_control must be an object in " +
                                 config_path.string();
                }
                return false;
            }
            const nlohmann::json& local_control = gui["local_control"];
            if (!read_optional_bool_field(
                    local_control,
                    "recording_start_enabled",
                    &config.gui_local_control_recording_start_enabled,
                    error_out,
                    "gui.local_control") ||
                !read_optional_bool_field(
                    local_control,
                    "recording_stop_enabled",
                    &config.gui_local_control_recording_stop_enabled,
                    error_out,
                    "gui.local_control") ||
                !read_optional_bool_field(
                    local_control,
                    "citrus_completion_stop_enabled",
                    &config.gui_local_control_citrus_completion_stop_enabled,
                    error_out,
                    "gui.local_control") ||
                !read_optional_bool_field(
                    local_control,
                    "exit_after_finalize",
                    &config.gui_local_control_exit_after_finalize,
                    error_out,
                    "gui.local_control") ||
                !read_optional_bounded_int_field(
                    local_control,
                    "drain_timeout_seconds",
                    &config.gui_local_control_drain_timeout_seconds,
                    0,
                    86400,
                    error_out,
                    "gui.local_control")) {
                if (error_out && error_out->find(config_path.string()) == std::string::npos) {
                    *error_out += " in " + config_path.string();
                }
                return false;
            }
        }
    }

    if (root.contains("storage") && root["storage"].is_object()) {
        const nlohmann::json& storage = root["storage"];
        if (storage.contains("default_recording_root")) {
            if (!storage["default_recording_root"].is_string()) {
                if (error_out) {
                    *error_out = "storage.default_recording_root must be a string in " + config_path.string();
                }
                return false;
            }
            std::string configured_root = trim_ascii_copy(storage["default_recording_root"].get<std::string>());
            if (!configured_root.empty()) {
                config.default_recording_root = configured_root;
            }
        }
        if (storage.contains("latest_recording")) {
            if (!storage["latest_recording"].is_object()) {
                if (error_out) {
                    *error_out = "storage.latest_recording must be an object in " + config_path.string();
                }
                return false;
            }

            const nlohmann::json& latest = storage["latest_recording"];
            if (latest.contains("write_local_pointer")) {
                if (!latest["write_local_pointer"].is_boolean()) {
                    if (error_out) {
                        *error_out =
                            "storage.latest_recording.write_local_pointer must be a boolean in " +
                            config_path.string();
                    }
                    return false;
                }
                config.write_local_pointer = latest["write_local_pointer"].get<bool>();
            }
            if (latest.contains("canonical_pointer_root")) {
                if (!latest["canonical_pointer_root"].is_string()) {
                    if (error_out) {
                        *error_out =
                            "storage.latest_recording.canonical_pointer_root must be a string in " +
                            config_path.string();
                    }
                    return false;
                }
                config.canonical_pointer_root =
                    trim_ascii_copy(latest["canonical_pointer_root"].get<std::string>());
            }
            if (latest.contains("write_run_pointer")) {
                if (!latest["write_run_pointer"].is_boolean()) {
                    if (error_out) {
                        *error_out =
                            "storage.latest_recording.write_run_pointer must be a boolean in " +
                            config_path.string();
                    }
                    return false;
                }
                config.write_run_pointer = latest["write_run_pointer"].get<bool>();
            }
            if (latest.contains("run_pointer_path")) {
                if (!latest["run_pointer_path"].is_string()) {
                    if (error_out) {
                        *error_out =
                            "storage.latest_recording.run_pointer_path must be a string in " +
                            config_path.string();
                    }
                    return false;
                }
                config.run_pointer_path = trim_ascii_copy(latest["run_pointer_path"].get<std::string>());
            }
        }
    }

    *config_out = std::move(config);
    return true;
}

std::string resolve_default_detect_engine(const std::string& orange_root_dir_str,
                                          std::string* warning_out)
{
    if (warning_out) {
        warning_out->clear();
    }

    const char* env_engine = std::getenv("ORANGE_DEFAULT_DETECT_ENGINE");
    if (env_engine && env_engine[0] != '\0') {
        const std::string override_engine = trim_ascii_copy(env_engine);
        if (!override_engine.empty()) {
            return override_engine;
        }
    }

    AppStorageConfig config;
    std::string error;
    if (!load_app_storage_config(orange_root_dir_str, &config, &error)) {
        if (warning_out) {
            *warning_out = error;
        }
        return std::string();
    }

    return config.default_detect_engine;
}

std::string resolve_default_recording_root(const std::string& orange_root_dir_str,
                                           std::string* warning_out)
{
    if (warning_out) {
        warning_out->clear();
    }

    AppStorageConfig config;
    std::string error;
    if (!load_app_storage_config(orange_root_dir_str, &config, &error)) {
        if (warning_out) {
            *warning_out = error;
        }
        return default_recording_root_for_orange_root(orange_root_dir_str);
    }

    return config.default_recording_root;
}

void prepare_application_folders(std::string orange_root_dir_str)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    // ... (implementation from project.h)
    std::string recordings_str = orange_root_dir_str + "/exp/unsorted";
    std::filesystem::path recordings_path(recordings_str);
    if (!std::filesystem::exists(recordings_path)) {
        if(std::filesystem::create_directories(recordings_path)) {
            std::cout << "Create recording folder..." << std::endl;
        }
    }

    std::string detect_str = orange_root_dir_str + "/detect";
    std::filesystem::path detect_path(detect_str);
    if (!std::filesystem::exists(detect_path)) {
        if(std::filesystem::create_directory(detect_path)) {
            std::cout << "Create detecting folder..." << std::endl;
        }
    }

    std::string config_local = orange_root_dir_str + "/config/local";
    std::filesystem::path config_local_path(config_local);
    if (!std::filesystem::exists(config_local_path)) {
        if(std::filesystem::create_directories(config_local_path)) {
            std::cout << "Create config/local folder..." << std::endl;
        }
    }

    std::string config_network = orange_root_dir_str + "/config/network";
    std::filesystem::path config_network_path(config_network);
    if (!std::filesystem::exists(config_network_path)) {
        if(std::filesystem::create_directory(config_network_path)) {
            std::cout << "Create config/network folder..." << std::endl;
        }
    }

    std::string config_app = orange_root_dir_str + "/config/app";
    std::filesystem::path config_app_path(config_app);
    if (!std::filesystem::exists(config_app_path)) {
        if(std::filesystem::create_directories(config_app_path)) {
            std::cout << "Create config/app folder..." << std::endl;
        }
    }

    std::string picture_str = orange_root_dir_str + "/pictures";
    std::filesystem::path picture_path(picture_str);
    if (!std::filesystem::exists(picture_path)) {
        if(std::filesystem::create_directory(picture_path)) {
            std::cout << "Create picture folder..." << std::endl;
        }
    }

    std::string calibration_str = orange_root_dir_str + "/exp/calibration";
    std::filesystem::path calibration_path(calibration_str);
    if (!std::filesystem::exists(calibration_path)) {
        if(std::filesystem::create_directory(calibration_path)) {
            std::cout << "Create calibration folder..." << std::endl;
        }
    }

    std::string calibration_artifacts_str = orange_root_dir_str + "/calibrations/artifacts";
    std::filesystem::path calibration_artifacts_path(calibration_artifacts_str);
    if (!std::filesystem::exists(calibration_artifacts_path)) {
        if (std::filesystem::create_directories(calibration_artifacts_path)) {
            std::cout << "Create calibration artifacts folder..." << std::endl;
        }
    }
}

void intialize_servers(ConnectedServer* my_servers)
{
    // ... (implementation from project.h)
    my_servers[0].server_state = FetchGame::ManagerState_IDLE;
    my_servers[0].num_cameras = 0;
    my_servers[0].peer = nullptr;
    my_servers[0].ip_add[0] = 192;
    my_servers[0].ip_add[1] = 168;
    my_servers[0].ip_add[2] = 20;
    my_servers[0].ip_add[3] = 60;
    my_servers[0].port = 3333;
    my_servers[0].connected = false;
    strcpy(my_servers[0].name, "waffle-0");


    my_servers[1].server_state = FetchGame::ManagerState_IDLE;
    my_servers[1].num_cameras = 0;
    my_servers[1].peer = nullptr;
    my_servers[1].ip_add[0] = 192;
    my_servers[1].ip_add[1] = 168;
    my_servers[1].ip_add[2] = 20;
    my_servers[1].ip_add[3] = 61;
    my_servers[1].port = 3333;
    my_servers[1].connected = false;
    strcpy(my_servers[1].name, "waffle-1");
}

std::vector<std::string> string_split(std::string s, std::string delimiter) {
    // ... (implementation from project.h)
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    std::string token;
    std::vector<std::string> res;

    while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
        token = s.substr (pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back (token);
    }

    res.push_back (s.substr (pos_start));
    return res;
}

nlohmann::json build_gpu_runtime_info(int gpu_id) {
    nlohmann::json info = nlohmann::json::object();
    info["id"] = gpu_id;

    if (gpu_id < 0) {
        info["lookup_error"] = "invalid gpu id";
        return info;
    }

    cudaDeviceProp props{};
    const cudaError_t props_status = cudaGetDeviceProperties(&props, gpu_id);
    if (props_status != cudaSuccess) {
        info["lookup_error"] = cudaGetErrorString(props_status);
        return info;
    }

    info["name"] = std::string(props.name);
    info["compute_capability"] = {
        {"major", props.major},
        {"minor", props.minor},
    };
    info["total_global_mem_bytes"] = static_cast<uint64_t>(props.totalGlobalMem);

    char pci_bus_id[32] = {0};
    const cudaError_t pci_status = cudaDeviceGetPCIBusId(pci_bus_id, sizeof(pci_bus_id), gpu_id);
    if (pci_status == cudaSuccess && pci_bus_id[0] != '\0') {
        info["pci_bus_id"] = std::string(pci_bus_id);
    } else if (pci_status != cudaSuccess) {
        info["pci_bus_id_lookup_error"] = cudaGetErrorString(pci_status);
    }

    return info;
}

namespace {

std::vector<std::string> split_whitespace_tokens(const std::string& line)
{
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

std::string strip_ansi_escape_sequences(const std::string& input)
{
    std::string output;
    output.reserve(input.size());

    for (std::size_t i = 0; i < input.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(input[i]);
        if (ch == 0x1b && i + 1 < input.size() && input[i + 1] == '[') {
            i += 2;
            while (i < input.size()) {
                const unsigned char c = static_cast<unsigned char>(input[i]);
                if (c >= '@' && c <= '~') {
                    break;
                }
                ++i;
            }
            continue;
        }
        output.push_back(static_cast<char>(ch));
    }

    return output;
}

struct NvidiaSmiTopologyCache {
    bool success = false;
    std::string error;
    std::vector<std::string> gpu_headers;
    std::map<std::string, std::vector<std::string>> rows;
};

NvidiaSmiTopologyCache load_nvidia_smi_topology_cache()
{
    NvidiaSmiTopologyCache cache;
    FILE* pipe = popen("nvidia-smi topo -m 2>/dev/null", "r");
    if (!pipe) {
        cache.error = "failed to execute `nvidia-smi topo -m`";
        return cache;
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        const std::string clean_line = strip_ansi_escape_sequences(buffer);
        const std::vector<std::string> tokens = split_whitespace_tokens(clean_line);
        if (tokens.empty()) {
            continue;
        }

        if (cache.gpu_headers.empty() &&
            tokens.size() > 1 &&
            tokens.front().rfind("GPU", 0) == 0 &&
            tokens[1].rfind("GPU", 0) == 0) {
            for (const std::string& token : tokens) {
                if (token.rfind("GPU", 0) != 0) {
                    break;
                }
                cache.gpu_headers.push_back(token);
            }
            continue;
        }

        if (tokens.front().rfind("GPU", 0) != 0 || cache.gpu_headers.empty()) {
            continue;
        }

        if (tokens.size() < cache.gpu_headers.size() + 1) {
            continue;
        }

        std::vector<std::string> connections;
        connections.reserve(cache.gpu_headers.size());
        for (std::size_t i = 0; i < cache.gpu_headers.size(); ++i) {
            connections.push_back(tokens[i + 1]);
        }
        cache.rows[tokens.front()] = std::move(connections);
    }

    const int close_status = pclose(pipe);
    if (close_status != 0) {
        cache.error = "`nvidia-smi topo -m` exited with status " + std::to_string(close_status);
        return cache;
    }
    if (cache.gpu_headers.empty() || cache.rows.empty()) {
        cache.error = "`nvidia-smi topo -m` did not return a parseable GPU topology table";
        return cache;
    }

    cache.success = true;
    return cache;
}

const NvidiaSmiTopologyCache& get_nvidia_smi_topology_cache()
{
    static std::mutex cache_mutex;
    static bool initialized = false;
    static NvidiaSmiTopologyCache cache;

    std::lock_guard<std::mutex> lock(cache_mutex);
    if (!initialized) {
        cache = load_nvidia_smi_topology_cache();
        initialized = true;
    }
    return cache;
}

} // namespace

std::string lookup_nvidia_smi_topology_class(int source_gpu_id,
                                             int target_gpu_id,
                                             std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (source_gpu_id < 0 || target_gpu_id < 0) {
        if (error_out) {
            *error_out = "invalid GPU ids";
        }
        return "";
    }

    const NvidiaSmiTopologyCache& cache = get_nvidia_smi_topology_cache();
    if (!cache.success) {
        if (error_out) {
            *error_out = cache.error.empty() ? "topology cache unavailable" : cache.error;
        }
        return "";
    }

    const std::string row_key = "GPU" + std::to_string(source_gpu_id);
    const std::string col_key = "GPU" + std::to_string(target_gpu_id);
    const auto row_it = cache.rows.find(row_key);
    if (row_it == cache.rows.end()) {
        if (error_out) {
            *error_out = "row not found in `nvidia-smi topo -m`: " + row_key;
        }
        return "";
    }

    const auto header_it = std::find(cache.gpu_headers.begin(), cache.gpu_headers.end(), col_key);
    if (header_it == cache.gpu_headers.end()) {
        if (error_out) {
            *error_out = "column not found in `nvidia-smi topo -m`: " + col_key;
        }
        return "";
    }

    const std::size_t column_index = static_cast<std::size_t>(
        std::distance(cache.gpu_headers.begin(), header_it));
    if (column_index >= row_it->second.size()) {
        if (error_out) {
            *error_out = "topology matrix index out of range";
        }
        return "";
    }

    return row_it->second[column_index];
}

nlohmann::json build_gpu_copy_path_static_topology_info(int source_gpu_id, int target_gpu_id)
{
    nlohmann::json info = {
        {"source_gpu_id", source_gpu_id},
        {"target_gpu_id", target_gpu_id},
        {"source_gpu", build_gpu_runtime_info(source_gpu_id)},
        {"target_gpu", build_gpu_runtime_info(target_gpu_id)},
        {"same_gpu", source_gpu_id == target_gpu_id},
        {"copy_direction", "source_to_target"},
        {"peer_access_capability", {
            {"can_access_peer_query_direction", {
                {"accessing_gpu_id", target_gpu_id},
                {"peer_gpu_id", source_gpu_id}
            }}
        }}
    };

    std::string topology_error;
    const std::string topology_class =
        lookup_nvidia_smi_topology_class(source_gpu_id, target_gpu_id, &topology_error);
    if (!topology_class.empty()) {
        info["topology_class"] = topology_class;
    } else if (!topology_error.empty()) {
        info["topology_lookup_error"] = topology_error;
    }

    if (source_gpu_id < 0 || target_gpu_id < 0) {
        info["peer_access_capability"]["can_access_peer"] = false;
        info["peer_access_capability"]["peer_access_required"] = false;
        return info;
    }

    if (source_gpu_id == target_gpu_id) {
        info["peer_access_capability"]["can_access_peer"] = true;
        info["peer_access_capability"]["peer_access_required"] = false;
        return info;
    }

    int can_access_peer = 0;
    const cudaError_t peer_status =
        cudaDeviceCanAccessPeer(&can_access_peer, target_gpu_id, source_gpu_id);
    if (peer_status == cudaSuccess) {
        info["peer_access_capability"]["can_access_peer"] = (can_access_peer != 0);
    } else {
        info["peer_access_capability"]["can_access_peer"] = false;
        info["peer_access_capability"]["can_access_peer_lookup_error"] =
            cudaGetErrorString(peer_status);
    }
    info["peer_access_capability"]["peer_access_required"] = true;

    return info;
}

RecordingValidationGpuPathInfo build_recording_validation_gpu_path_info(int source_gpu_id,
                                                                        int helper_gpu_id)
{
    RecordingValidationGpuPathInfo info;
    info.source_gpu_id = source_gpu_id;
    info.helper_gpu_id = helper_gpu_id;

    const nlohmann::json copy_path_info =
        build_gpu_copy_path_static_topology_info(source_gpu_id, helper_gpu_id);
    if (copy_path_info.contains("topology_class") &&
        copy_path_info["topology_class"].is_string()) {
        info.topology_class = copy_path_info["topology_class"].get<std::string>();
    }
    if (copy_path_info.contains("topology_lookup_error") &&
        copy_path_info["topology_lookup_error"].is_string()) {
        info.topology_error = copy_path_info["topology_lookup_error"].get<std::string>();
    }
    if (copy_path_info.contains("peer_access_capability") &&
        copy_path_info["peer_access_capability"].is_object()) {
        const nlohmann::json& peer_access = copy_path_info["peer_access_capability"];
        if (peer_access.contains("can_access_peer") &&
            peer_access["can_access_peer"].is_boolean()) {
            info.can_access_peer = peer_access["can_access_peer"].get<bool>();
            info.can_access_peer_known = true;
        }
    }

    return info;
}

std::vector<std::string> string_split_char(char* string_c, std::string delimiter) {
    // ... (implementation from project.h)
    std::string s = std::string(string_c);
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    std::string token;
    std::vector<std::string> res;

    while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
        token = s.substr (pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back (token);
    }

    res.push_back (s.substr (pos_start));
    return res;
}

namespace {
constexpr const char* kCameraConfigSchemaId = "orange.camera.config";
constexpr int kCameraConfigSchemaVersion = 4;

std::string lower_ascii_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string normalize_camera_sync_mode_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "ptp_gate" || value == "free_run" || value == "external_trigger" || value == "software_trigger") {
        return value;
    }
    return "free_run";
}

std::string normalize_camera_scan_type_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "area_scan" || value == "line_scan" || value == "unknown") {
        return value;
    }
    return "unknown";
}

std::string normalize_gpio_connector_variant_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "area_scan_12_pin" || value == "area_scan_8_pin" ||
        value == "line_scan_12_pin" || value == "unknown") {
        return value;
    }
    return "unknown";
}

std::string normalize_recording_mode_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "single_session" || value == "split_gop") {
        return value;
    }
    return "single_session";
}

std::string normalize_split_gop_placement_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "single_gpu" || value == "multi_gpu") {
        return value;
    }
    return "single_gpu";
}

std::string normalize_split_gop_source_encoder_policy_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "local_only" || value == "hybrid_split" || value == "pure_offload") {
        return value;
    }
    return "local_only";
}

std::string normalize_split_gop_transfer_mode_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "prepared") {
        return "prepared_nv12";
    }
    if (value == "auto" || value == "raw" || value == "prepared_nv12") {
        return value;
    }
    return "auto";
}

std::string normalize_recording_output_mode_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "exact_size") {
        return "resolution";
    }
    if (value == "factor" || value == "resolution") {
        return value;
    }
    return "factor";
}

int normalize_encoder_toggle_value(int value) {
    return (value == 0 || value == 1) ? value : -1;
}

std::string format_encoder_toggle_value(int value) {
    value = normalize_encoder_toggle_value(value);
    if (value == 0) {
        return "off";
    }
    if (value == 1) {
        return "on";
    }
    return "auto";
}

bool parse_encoder_toggle_json_value(const nlohmann::json& value, int* out_value) {
    if (!out_value) {
        return false;
    }
    if (value.is_boolean()) {
        *out_value = value.get<bool>() ? 1 : 0;
        return true;
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        const int parsed = value.get<int>();
        if (parsed == -1 || parsed == 0 || parsed == 1) {
            *out_value = parsed;
            return true;
        }
        return false;
    }
    if (value.is_string()) {
        const std::string normalized = lower_ascii_copy(trim_ascii_copy(value.get<std::string>()));
        if (normalized == "auto" || normalized == "default" || normalized == "inherit") {
            *out_value = -1;
            return true;
        }
        if (normalized == "off" || normalized == "false" || normalized == "disabled" ||
            normalized == "0") {
            *out_value = 0;
            return true;
        }
        if (normalized == "on" || normalized == "true" || normalized == "enabled" ||
            normalized == "1") {
            *out_value = 1;
            return true;
        }
    }
    return false;
}

bool try_parse_encoder_toggle_field(const nlohmann::json& object,
                                    const char* key,
                                    int* out_value,
                                    std::string* error_out) {
    if (!out_value || !object.contains(key)) {
        return true;
    }
    int parsed = -1;
    if (!parse_encoder_toggle_json_value(object[key], &parsed)) {
        if (error_out) {
            *error_out = std::string("recording.encode.") + key +
                         " must be one of auto, off, on, false, true, -1, 0, or 1";
        }
        return false;
    }
    *out_value = parsed;
    return true;
}

EncoderControlOverrides resolve_encoder_control_overrides(
    const CameraRecordingEncodeConfig& encode,
    const EncoderControlOverrides& runtime_overrides) {
    EncoderControlOverrides resolved;
    resolved.aq = normalize_encoder_toggle_value(encode.aq);
    resolved.temporal_aq = normalize_encoder_toggle_value(encode.temporal_aq);

    if (runtime_overrides.aq >= 0) {
        resolved.aq = runtime_overrides.aq;
    }
    if (runtime_overrides.temporal_aq >= 0) {
        resolved.temporal_aq = runtime_overrides.temporal_aq;
    }
    if (runtime_overrides.lookahead >= 0) {
        resolved.lookahead = runtime_overrides.lookahead;
    }
    if (runtime_overrides.lookahead_depth >= 0) {
        resolved.lookahead_depth = runtime_overrides.lookahead_depth;
    }
    if (runtime_overrides.target_bitrate_bps >= 0) {
        resolved.target_bitrate_bps = runtime_overrides.target_bitrate_bps;
    }
    if (runtime_overrides.max_bitrate_bps >= 0) {
        resolved.max_bitrate_bps = runtime_overrides.max_bitrate_bps;
    }
    if (runtime_overrides.vbv_buffer_size >= 0) {
        resolved.vbv_buffer_size = runtime_overrides.vbv_buffer_size;
    }
    return resolved;
}

std::string normalize_preferred_topology_class_string(std::string value) {
    if (value.empty()) {
        return value;
    }
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

bool try_get_nonnegative_u64(const nlohmann::json& object, const char* key, uint64_t* out_value) {
    if (!out_value || !object.contains(key)) {
        return false;
    }
    const nlohmann::json& value = object[key];
    if (value.is_number_unsigned()) {
        *out_value = value.get<uint64_t>();
        return true;
    }
    if (value.is_number_integer()) {
        const long long parsed = value.get<long long>();
        if (parsed < 0) {
            return false;
        }
        *out_value = static_cast<uint64_t>(parsed);
        return true;
    }
    return false;
}

bool try_get_nonnegative_int(const nlohmann::json& object, const char* key, int* out_value) {
    if (!out_value || !object.contains(key)) {
        return false;
    }
    const nlohmann::json& value = object[key];
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        return false;
    }
    const long long parsed = value.get<long long>();
    if (parsed < 0) {
        return false;
    }
    *out_value = static_cast<int>(parsed);
    return true;
}

void parse_split_gop_encoder_gpu_ids(const nlohmann::json& split_gop,
                                     std::vector<int>* encoder_gpu_ids_out) {
    if (!encoder_gpu_ids_out || !split_gop.contains("encoder_gpu_ids") ||
        !split_gop["encoder_gpu_ids"].is_array()) {
        return;
    }

    std::set<int> seen_gpu_ids;
    for (const auto& value : split_gop["encoder_gpu_ids"]) {
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            continue;
        }
        const long long parsed = value.get<long long>();
        if (parsed < 0) {
            continue;
        }
        const int gpu_id = static_cast<int>(parsed);
        if (seen_gpu_ids.insert(gpu_id).second) {
            encoder_gpu_ids_out->push_back(gpu_id);
        }
    }
}

void normalize_recording_strategy_config(RecordingStrategyConfig* config) {
    if (!config) {
        return;
    }

    config->requested_mode = normalize_recording_mode_string(
        config->requested_mode.empty() ? config->mode : config->requested_mode);
    config->mode = config->requested_mode;
    config->split_gop.placement = normalize_split_gop_placement_string(config->split_gop.placement);
    config->split_gop.source_encoder_policy =
        normalize_split_gop_source_encoder_policy_string(config->split_gop.source_encoder_policy);
    config->split_gop.transfer_mode =
        normalize_split_gop_transfer_mode_string(config->split_gop.transfer_mode);
    if (config->split_gop.encoder_gpu_ids.size() > 1) {
        config->split_gop.placement = "multi_gpu";
    }
    config->split_gop.enabled = config->mode == "split_gop";
}

void normalize_camera_recording_config(CameraRecordingConfig* config) {
    if (!config) {
        return;
    }

    config->preferred_sink_mode =
        normalize_camera_preferred_recording_sink_mode(config->preferred_sink_mode);
    config->encode.codec = lower_ascii_copy(config->encode.codec);
    config->encode.preset = lower_ascii_copy(config->encode.preset);
    config->encode.tuning = lower_ascii_copy(config->encode.tuning);
    config->encode.rate_control_mode = lower_ascii_copy(config->encode.rate_control_mode);
    config->encode.aq = normalize_encoder_toggle_value(config->encode.aq);
    config->encode.temporal_aq = normalize_encoder_toggle_value(config->encode.temporal_aq);
    config->output.mode = normalize_recording_output_mode_string(config->output.mode);
    config->constraints.preferred_topology_class =
        normalize_preferred_topology_class_string(config->constraints.preferred_topology_class);
    normalize_recording_strategy_config(&config->strategy);
}

bool parse_recording_strategy_json_object(const nlohmann::json& recording,
                                          RecordingStrategyConfig* recording_strategy_out,
                                          std::string* error_out) {
    if (error_out) {
        error_out->clear();
    }
    if (!recording_strategy_out) {
        if (error_out) {
            *error_out = "recording strategy destination is null";
        }
        return false;
    }
    if (!recording.is_object()) {
        if (error_out) {
            *error_out = "recording strategy must be a JSON object";
        }
        return false;
    }

    RecordingStrategyConfig recording_strategy;
    recording_strategy.requested_mode =
        normalize_recording_mode_string(recording.value("mode", recording_strategy.requested_mode));
    recording_strategy.mode = recording_strategy.requested_mode;
    if (recording.contains("split_gop") && recording["split_gop"].is_object()) {
        const nlohmann::json& split_gop = recording["split_gop"];
        recording_strategy.split_gop.placement =
            normalize_split_gop_placement_string(
                split_gop.value("placement", recording_strategy.split_gop.placement));
        recording_strategy.split_gop.source_encoder_policy =
            normalize_split_gop_source_encoder_policy_string(split_gop.value(
                "source_encoder_policy", recording_strategy.split_gop.source_encoder_policy));
        recording_strategy.split_gop.transfer_mode =
            normalize_split_gop_transfer_mode_string(
                split_gop.value("transfer_mode", recording_strategy.split_gop.transfer_mode));
        parse_split_gop_encoder_gpu_ids(
            split_gop, &recording_strategy.split_gop.encoder_gpu_ids);
        try_get_nonnegative_u64(
            split_gop, "max_inflight_gops",
            &recording_strategy.split_gop.max_inflight_gops);
        try_get_nonnegative_u64(
            split_gop, "max_buffered_bytes",
            &recording_strategy.split_gop.max_buffered_bytes);
        recording_strategy.split_gop.strict =
            split_gop.value("strict", recording_strategy.split_gop.strict);

        if (split_gop.contains("writer_queue") && split_gop["writer_queue"].is_object()) {
            const nlohmann::json& writer_queue = split_gop["writer_queue"];
            try_get_nonnegative_u64(
                writer_queue, "max_packets",
                &recording_strategy.split_gop.writer_queue.max_packets);
            try_get_nonnegative_u64(
                writer_queue, "max_bytes",
                &recording_strategy.split_gop.writer_queue.max_bytes);
            recording_strategy.split_gop.writer_queue.fail_on_overflow =
                writer_queue.value(
                    "fail_on_overflow",
                    recording_strategy.split_gop.writer_queue.fail_on_overflow);
        }
    }

    normalize_recording_strategy_config(&recording_strategy);
    *recording_strategy_out = std::move(recording_strategy);
    return true;
}

bool parse_camera_recording_json_impl(const nlohmann::json& recording_json,
                                      CameraRecordingConfig* recording_out,
                                      std::string* error_out) {
    if (error_out) {
        error_out->clear();
    }
    if (!recording_out) {
        if (error_out) {
            *error_out = "Internal error: null camera recording config destination";
        }
        return false;
    }
    if (!recording_json.is_object()) {
        if (error_out) {
            *error_out = "Recording config must be a JSON object";
        }
        return false;
    }

    CameraRecordingConfig recording;
    recording.profile_name = recording_json.value("profile_name", recording.profile_name);
    if (recording_json.contains("preferred_sink_mode")) {
        if (!recording_json["preferred_sink_mode"].is_string()) {
            if (error_out) {
                *error_out = "recording.preferred_sink_mode must be a string";
            }
            return false;
        }
        const std::string raw_preferred_sink_mode =
            trim_ascii_copy(recording_json["preferred_sink_mode"].get<std::string>());
        std::string normalized_raw_preferred_sink_mode = raw_preferred_sink_mode;
        std::transform(
            normalized_raw_preferred_sink_mode.begin(),
            normalized_raw_preferred_sink_mode.end(),
            normalized_raw_preferred_sink_mode.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
        recording.preferred_sink_mode =
            normalize_camera_preferred_recording_sink_mode(raw_preferred_sink_mode);
        if (recording.preferred_sink_mode.empty() &&
            !raw_preferred_sink_mode.empty() &&
            normalized_raw_preferred_sink_mode != "default" &&
            normalized_raw_preferred_sink_mode != "auto" &&
            normalized_raw_preferred_sink_mode != "app") {
            if (error_out) {
                *error_out =
                    "recording.preferred_sink_mode must be real, external_ipc, default, auto, app, or empty";
            }
            return false;
        }
    }

    if (recording_json.contains("encode") && recording_json["encode"].is_object()) {
        const nlohmann::json& encode = recording_json["encode"];
        recording.encode.codec =
            lower_ascii_copy(encode.value("codec", recording.encode.codec));
        recording.encode.preset =
            lower_ascii_copy(encode.value("preset", recording.encode.preset));
        recording.encode.tuning =
            lower_ascii_copy(encode.value("tuning", recording.encode.tuning));
        recording.encode.rate_control_mode = lower_ascii_copy(
            encode.value("rate_control_mode", recording.encode.rate_control_mode));
        recording.encode.quality_value =
            encode.value("quality_value", recording.encode.quality_value);
        recording.encode.gop_length =
            encode.value("gop_length", recording.encode.gop_length);
        if (!try_parse_encoder_toggle_field(
                encode, "aq", &recording.encode.aq, error_out) ||
            !try_parse_encoder_toggle_field(
                encode, "temporal_aq", &recording.encode.temporal_aq, error_out)) {
            return false;
        }
        recording.encode.nvenc_direct_input =
            encode.value("nvenc_direct_input", recording.encode.nvenc_direct_input);
    }

    if (recording_json.contains("output") && recording_json["output"].is_object()) {
        const nlohmann::json& output = recording_json["output"];
        recording.output.mode =
            normalize_recording_output_mode_string(
                output.value("mode", recording.output.mode));
        recording.output.downsample_factor =
            output.value("downsample_factor", recording.output.downsample_factor);
        recording.output.requested_width =
            output.value("requested_width", recording.output.requested_width);
        recording.output.requested_height =
            output.value("requested_height", recording.output.requested_height);
    }

    if (recording_json.contains("constraints") && recording_json["constraints"].is_object()) {
        const nlohmann::json& constraints = recording_json["constraints"];
        recording.constraints.require_peer_access =
            constraints.value("require_peer_access", recording.constraints.require_peer_access);
        recording.constraints.preferred_topology_class =
            normalize_preferred_topology_class_string(constraints.value(
                "preferred_topology_class",
                recording.constraints.preferred_topology_class));
    }

    if (recording_json.contains("resources") && recording_json["resources"].is_object()) {
        const nlohmann::json& resources = recording_json["resources"];
        try_get_nonnegative_int(
            resources, "acquire_work_entries",
            &recording.resources.acquire_work_entries);
        try_get_nonnegative_int(
            resources, "encoder_entry_pool_size",
            &recording.resources.encoder_entry_pool_size);
    }

    RecordingStrategyConfig parsed_strategy;
    if (parse_recording_strategy_json_object(recording_json, &parsed_strategy, nullptr)) {
        recording.strategy = std::move(parsed_strategy);
    }

    normalize_camera_recording_config(&recording);
    *recording_out = std::move(recording);
    return true;
}

void parse_recording_config_from_json(const nlohmann::json& camera_config,
                                      CameraParams* camera_params) {
    if (!camera_params) {
        return;
    }

    camera_params->recording = CameraRecordingConfig();
    if (!camera_config.contains("recording") || !camera_config["recording"].is_object()) {
        return;
    }

    CameraRecordingConfig recording;
    if (parse_camera_recording_json_impl(camera_config["recording"], &recording, nullptr)) {
        camera_params->recording = std::move(recording);
    }
}

void parse_crop_pipeline_config_from_json(const nlohmann::json& camera_config,
                                          CameraParams* camera_params) {
    orange::camera_config::parse_crop_pipeline_config(camera_config, camera_params);
}

nlohmann::json build_crop_pipeline_config_json_from_params(const CameraParams& camera_params)
{
    return orange::camera_config::build_crop_pipeline_config(camera_params);
}

nlohmann::json build_recording_strategy_json_object(const RecordingStrategyConfig& recording_strategy_in) {
    RecordingStrategyConfig recording_strategy = recording_strategy_in;
    normalize_recording_strategy_config(&recording_strategy);

    nlohmann::json recording = nlohmann::json::object();
    recording["mode"] = recording_strategy.requested_mode;
    recording["split_gop"] = {
        {"placement", recording_strategy.split_gop.placement},
        {"encoder_gpu_ids", recording_strategy.split_gop.encoder_gpu_ids},
        {"source_encoder_policy", recording_strategy.split_gop.source_encoder_policy},
        {"transfer_mode", recording_strategy.split_gop.transfer_mode},
        {"max_inflight_gops", recording_strategy.split_gop.max_inflight_gops},
        {"max_buffered_bytes", recording_strategy.split_gop.max_buffered_bytes},
        {"strict", recording_strategy.split_gop.strict},
        {"writer_queue", {
            {"max_packets", recording_strategy.split_gop.writer_queue.max_packets},
            {"max_bytes", recording_strategy.split_gop.writer_queue.max_bytes},
            {"fail_on_overflow", recording_strategy.split_gop.writer_queue.fail_on_overflow}
        }}
    };
    return recording;
}

nlohmann::json build_camera_recording_json_impl(const CameraRecordingConfig& recording_in)
{
    CameraRecordingConfig recording = recording_in;
    normalize_camera_recording_config(&recording);

    nlohmann::json recording_json = build_recording_strategy_json_object(recording.strategy);
    recording_json["profile_name"] = recording.profile_name;
    if (!recording.preferred_sink_mode.empty()) {
        recording_json["preferred_sink_mode"] = recording.preferred_sink_mode;
    }
    recording_json["encode"] = {
        {"codec", recording.encode.codec},
        {"preset", recording.encode.preset},
        {"tuning", recording.encode.tuning},
        {"rate_control_mode", recording.encode.rate_control_mode},
        {"quality_value", recording.encode.quality_value},
        {"gop_length", recording.encode.gop_length},
        {"aq", format_encoder_toggle_value(recording.encode.aq)},
        {"temporal_aq", format_encoder_toggle_value(recording.encode.temporal_aq)},
        {"nvenc_direct_input", recording.encode.nvenc_direct_input}
    };
    recording_json["output"] = {
        {"mode", recording.output.mode},
        {"downsample_factor", recording.output.downsample_factor},
        {"requested_width", recording.output.requested_width},
        {"requested_height", recording.output.requested_height}
    };
    recording_json["constraints"] = {
        {"require_peer_access", recording.constraints.require_peer_access},
        {"preferred_topology_class", recording.constraints.preferred_topology_class}
    };
    recording_json["resources"] = {
        {"acquire_work_entries", recording.resources.acquire_work_entries},
        {"encoder_entry_pool_size", recording.resources.encoder_entry_pool_size}
    };
    return recording_json;
}

nlohmann::json build_recording_config_json_from_params(const CameraParams& camera_params)
{
    return build_camera_recording_json_impl(camera_params.recording);
}

std::string canonicalize_gpio_recipe_string(std::string value) {
    const std::string normalized = lower_ascii_copy(value);
    if (normalized == "area_scan_hw_trigger_internal_gpi4" ||
        normalized == "area_scan_hw_trigger_external_gpi4" ||
        normalized == "line_scan_hw_frame_gpi1_internal_line" ||
        normalized == "line_scan_hw_frame_gpi1_encoder_line" ||
        normalized == "line_scan_encoder_frame_encoder_line" ||
        normalized == "line_scan_hw_gate_gpi1_encoder_frame_encoder_line") {
        return normalized;
    }
    return value;
}

std::string canonicalize_camera_serial_string(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
        return !std::isspace(c);
    }).base(), value.end());

    if (value.empty()) {
        return value;
    }

    const bool is_numeric = std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
    if (!is_numeric) {
        return value;
    }

    const auto first_non_zero = value.find_first_not_of('0');
    if (first_non_zero == std::string::npos) {
        return "0";
    }
    return value.substr(first_non_zero);
}

bool contains_case_insensitive(const std::string& haystack, const std::string& needle) {
    return lower_ascii_copy(haystack).find(lower_ascii_copy(needle)) != std::string::npos;
}

bool starts_with_case_insensitive(const std::string& value, const std::string& prefix) {
    const std::string lower_value = lower_ascii_copy(value);
    const std::string lower_prefix = lower_ascii_copy(prefix);
    return lower_value.rfind(lower_prefix, 0) == 0;
}

bool model_is_area_scan_family(const std::string& model) {
    return starts_with_case_insensitive(model, "HB") ||
           starts_with_case_insensitive(model, "HZ") ||
           starts_with_case_insensitive(model, "HR") ||
           starts_with_case_insensitive(model, "HT") ||
           starts_with_case_insensitive(model, "HE");
}

bool model_is_line_scan_family(const std::string& model) {
    return starts_with_case_insensitive(model, "LB") ||
           starts_with_case_insensitive(model, "TLB") ||
           starts_with_case_insensitive(model, "LR") ||
           starts_with_case_insensitive(model, "TLR") ||
           starts_with_case_insensitive(model, "LT") ||
           starts_with_case_insensitive(model, "LZ") ||
           starts_with_case_insensitive(model, "TLZ");
}

bool model_uses_area_scan_8_pin_connector(const std::string& model) {
    return contains_case_insensitive(model, "eros") || starts_with_case_insensitive(model, "HE");
}

void infer_camera_gpio_metadata(CameraParams* camera_params) {
    if (!camera_params) {
        return;
    }

    if (camera_params->camera_scan_type == "unknown") {
        if (camera_params->device_model == "HB-65000GM" ||
            camera_params->device_model == "HB-65000GC" ||
            camera_params->device_model == "HB-7000SC" ||
            camera_params->device_model == "HB-7000SM") {
            camera_params->camera_scan_type = "area_scan";
        } else if (model_is_area_scan_family(camera_params->device_model)) {
            camera_params->camera_scan_type = "area_scan";
        } else if (model_is_line_scan_family(camera_params->device_model)) {
            camera_params->camera_scan_type = "line_scan";
        } else if (contains_case_insensitive(camera_params->device_model, "eros")) {
            camera_params->camera_scan_type = "area_scan";
        }
    }

    if (camera_params->gpio_connector_variant == "unknown") {
        if (camera_params->camera_scan_type == "line_scan") {
            camera_params->gpio_connector_variant = "line_scan_12_pin";
        } else if (camera_params->camera_scan_type == "area_scan") {
            if (model_uses_area_scan_8_pin_connector(camera_params->device_model)) {
                camera_params->gpio_connector_variant = "area_scan_8_pin";
            } else {
                camera_params->gpio_connector_variant = "area_scan_12_pin";
            }
        }
    }
}

void reset_camera_config_extensions(CameraParams* camera_params) {
    camera_params->config_schema_id.clear();
    camera_params->config_schema_version = 0;
    camera_params->device_model.clear();
    camera_params->camera_scan_type = "unknown";
    camera_params->gpio_connector_variant = "unknown";
    camera_params->gpio_recipe.clear();
    camera_params->sync_mode = "free_run";
    camera_params->trigger_enabled = false;
    camera_params->trigger_selector = "AcquisitionStart";
    camera_params->trigger_source = "Software";
    camera_params->trigger_activation = "RisingEdge";
    camera_params->ptp_mode.clear();
    camera_params->gpio_nodes.clear();
    camera_params->recording = CameraRecordingConfig();
    camera_params->crop_pipeline = CameraCropPipelineConfig();
    camera_params->lens_control_enabled = true;
}

void parse_gpio_nodes_from_json(const nlohmann::json& camera_config, CameraParams* camera_params) {
    if (!camera_config.contains("gpio")) {
        return;
    }

    const nlohmann::json* nodes_json = nullptr;
    const nlohmann::json& gpio = camera_config["gpio"];
    if (gpio.is_array()) {
        nodes_json = &gpio;
    } else if (gpio.is_object() && gpio.contains("nodes") && gpio["nodes"].is_array()) {
        nodes_json = &gpio["nodes"];
    }

    if (!nodes_json) {
        return;
    }

    for (const auto& node_json : *nodes_json) {
        if (!node_json.is_object()) {
            continue;
        }
        if (!node_json.contains("name") || !node_json["name"].is_string()) {
            continue;
        }

        CameraGpioNodeConfig node;
        node.name = node_json["name"].get<std::string>();
        node.type = lower_ascii_copy(node_json.value("type", std::string("enum")));

        if (!node_json.contains("value")) {
            std::cerr << "Skipping GPIO node without value: " << node.name << std::endl;
            continue;
        }

        const nlohmann::json& value = node_json["value"];
        if (node.type == "enum") {
            if (!value.is_string()) {
                std::cerr << "Skipping GPIO enum node with non-string value: " << node.name << std::endl;
                continue;
            }
            node.value_string = value.get<std::string>();
        } else if (node.type == "bool") {
            if (!value.is_boolean()) {
                std::cerr << "Skipping GPIO bool node with non-bool value: " << node.name << std::endl;
                continue;
            }
            node.value_bool = value.get<bool>();
        } else if (node.type == "uint") {
            if (!value.is_number_unsigned() && !value.is_number_integer()) {
                std::cerr << "Skipping GPIO uint node with non-integer value: " << node.name << std::endl;
                continue;
            }
            const auto parsed = value.get<long long>();
            if (parsed < 0) {
                std::cerr << "Skipping GPIO uint node with negative value: " << node.name << std::endl;
                continue;
            }
            node.value_uint = static_cast<uint32_t>(parsed);
        } else {
            std::cerr << "Skipping GPIO node with unsupported type `" << node.type
                      << "`: " << node.name << std::endl;
            continue;
        }

        camera_params->gpio_nodes.push_back(std::move(node));
    }
}

std::string config_filename_stem(const std::string& config_path) {
    return canonicalize_camera_serial_string(std::filesystem::path(config_path).stem().string());
}

std::string extract_config_serial_match_key(const std::string& config_path) {
    std::ifstream f(config_path);
    if (!f.good()) {
        return config_filename_stem(config_path);
    }

    try {
        nlohmann::json camera_config = nlohmann::json::parse(f);
        if (camera_config.contains("device_serial_number")) {
            const nlohmann::json& serial = camera_config["device_serial_number"];
            if (serial.is_string()) {
                const std::string value = serial.get<std::string>();
                if (!value.empty()) {
                    return canonicalize_camera_serial_string(value);
                }
            } else if (serial.is_number_integer() || serial.is_number_unsigned()) {
                return canonicalize_camera_serial_string(std::to_string(serial.get<long long>()));
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Failed to parse camera config while matching serial: " << config_path
                  << " (" << ex.what() << ")" << std::endl;
    }

    return config_filename_stem(config_path);
}

std::vector<std::string>::const_iterator find_camera_config_for_serial(
    const std::vector<std::string>& camera_config_files,
    const std::string& camera_serial)
{
    const std::string canonical_camera_serial = canonicalize_camera_serial_string(camera_serial);
    return std::find_if(camera_config_files.begin(), camera_config_files.end(),
                        [&](const std::string& config_path) {
                            return extract_config_serial_match_key(config_path) == canonical_camera_serial;
                        });
}

bool camera_config_schema_version_supported(int schema_version) {
    return schema_version == 1 || schema_version == 2 || schema_version == 3 ||
           schema_version == 4;
}

bool try_parse_source_gpu_id_from_json(const nlohmann::json& camera_config,
                                       int* gpu_id_out,
                                       bool* used_legacy_gpu_id_out,
                                       std::string* error_out) {
    if (gpu_id_out) {
        *gpu_id_out = -1;
    }
    if (used_legacy_gpu_id_out) {
        *used_legacy_gpu_id_out = false;
    }
    if (error_out) {
        error_out->clear();
    }

    const bool has_source_gpu_id = camera_config.contains("source_gpu_id");
    const bool has_legacy_gpu_id = camera_config.contains("gpu_id");
    if (!has_source_gpu_id && !has_legacy_gpu_id) {
        if (error_out) {
            *error_out = "camera config missing source_gpu_id";
        }
        return false;
    }

    auto parse_gpu_value = [&](const nlohmann::json& value, int* parsed_out) -> bool {
        if (!parsed_out) {
            return false;
        }
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            return false;
        }
        const long long parsed = value.get<long long>();
        if (parsed < 0) {
            return false;
        }
        *parsed_out = static_cast<int>(parsed);
        return true;
    };

    int source_gpu_id = -1;
    int legacy_gpu_id = -1;
    if (has_source_gpu_id && !parse_gpu_value(camera_config["source_gpu_id"], &source_gpu_id)) {
        if (error_out) {
            *error_out = "camera config field `source_gpu_id` must be a non-negative integer";
        }
        return false;
    }
    if (has_legacy_gpu_id && !parse_gpu_value(camera_config["gpu_id"], &legacy_gpu_id)) {
        if (error_out) {
            *error_out = "camera config field `gpu_id` must be a non-negative integer";
        }
        return false;
    }
    if (has_source_gpu_id && has_legacy_gpu_id && source_gpu_id != legacy_gpu_id) {
        if (error_out) {
            *error_out = "`source_gpu_id` and legacy `gpu_id` disagree";
        }
        return false;
    }

    if (gpu_id_out) {
        *gpu_id_out = has_source_gpu_id ? source_gpu_id : legacy_gpu_id;
    }
    if (used_legacy_gpu_id_out) {
        *used_legacy_gpu_id_out = !has_source_gpu_id && has_legacy_gpu_id;
    }
    return true;
}

nlohmann::json build_camera_config_json_from_params(const CameraParams& camera_params)
{
    nlohmann::json camera_config = nlohmann::json::object();
    camera_config["name"] = camera_params.camera_name;
    camera_config["width"] = camera_params.width;
    camera_config["height"] = camera_params.height;
    camera_config["frame_rate"] = camera_params.frame_rate;
    camera_config["gain"] = camera_params.gain;
    camera_config["exposure"] = camera_params.exposure;
    camera_config["pixel_format"] = camera_params.pixel_format;
    camera_config["color_temp"] = camera_params.color_temp;
    camera_config["source_gpu_id"] = camera_params.gpu_id;
    camera_config["gpu_direct"] = camera_params.gpu_direct;
    camera_config["focus_uart_bootstrap"] = camera_params.focus_uart_bootstrap;
    camera_config["lens_control_enabled"] = camera_params.lens_control_enabled;
    camera_config["color"] = camera_params.color;
    camera_config["focus"] = camera_params.focus;
    camera_config["iris"] = camera_params.iris;
    camera_config["offset_x"] = camera_params.offsetx;
    camera_config["offset_y"] = camera_params.offsety;
    camera_config["schema_id"] = kCameraConfigSchemaId;
    camera_config["schema_version"] = kCameraConfigSchemaVersion;
    camera_config["device_model_name"] = camera_params.device_model;
    camera_config["device_serial_number"] = canonicalize_camera_serial_string(camera_params.camera_serial);
    camera_config["camera_scan_type"] = normalize_camera_scan_type_string(camera_params.camera_scan_type);
    camera_config["gpio_connector_variant"] =
        normalize_gpio_connector_variant_string(camera_params.gpio_connector_variant);
    camera_config["gpio_recipe"] = canonicalize_gpio_recipe_string(camera_params.gpio_recipe);
    camera_config["sync_mode"] = normalize_camera_sync_mode_string(camera_params.sync_mode);
    camera_config["trigger"] = {
        {"enabled", camera_params.trigger_enabled},
        {"selector", camera_params.trigger_selector},
        {"source", camera_params.trigger_source},
        {"activation", camera_params.trigger_activation}
    };
    camera_config["ptp"] = {
        {"enabled", camera_sync_mode_uses_ptp(&camera_params)}
    };
    if (camera_sync_mode_uses_ptp(&camera_params)) {
        camera_config["ptp"]["mode"] = camera_params.ptp_mode.empty() ? "TwoStep" : camera_params.ptp_mode;
    }

    nlohmann::json gpio_nodes = nlohmann::json::array();
    for (const auto& node : camera_params.gpio_nodes) {
        nlohmann::json node_json;
        node_json["name"] = node.name;
        node_json["type"] = lower_ascii_copy(node.type);
        if (lower_ascii_copy(node.type) == "enum") {
            node_json["value"] = node.value_string;
        } else if (lower_ascii_copy(node.type) == "bool") {
            node_json["value"] = node.value_bool;
        } else if (lower_ascii_copy(node.type) == "uint") {
            node_json["value"] = node.value_uint;
        } else {
            continue;
        }
        gpio_nodes.push_back(std::move(node_json));
    }
    camera_config["gpio"] = {
        {"nodes", std::move(gpio_nodes)}
    };
    camera_config["recording"] = build_recording_config_json_from_params(camera_params);
    camera_config["crop_pipeline"] = build_crop_pipeline_config_json_from_params(camera_params);
    return camera_config;
}

nlohmann::json build_camera_runtime_snapshot(const CameraParams& camera_params)
{
    nlohmann::json snapshot = nlohmann::json::object();
    snapshot["source"] = {
        {"camera_config_path", camera_params.config_path},
        {"configured_source_gpu_id", camera_params.configured_gpu_id},
        {"configured_gpu_id", camera_params.configured_gpu_id},
        {"gpu_id_runtime_overridden", camera_params.gpu_id_runtime_overridden}
    };
    snapshot["runtime"] = build_camera_config_json_from_params(camera_params);
    return snapshot;
}
} // namespace

void load_camera_json_config_files(std::string file_name, CameraParams* camera_params, int camera_id, int num_cameras) {
    // ... (implementation from project.h)
    std::ifstream f(file_name);
    nlohmann::json camera_config = nlohmann::json::parse(f);

    camera_params->camera_id = camera_id;
    camera_params->num_cameras = num_cameras;
    camera_params->need_reorder = false;
    reset_camera_config_extensions(camera_params);

    camera_params->camera_name = camera_config["name"];
    camera_params->width = camera_config["width"];
    camera_params->height = camera_config["height"];
    camera_params->frame_rate = camera_config["frame_rate"];
    camera_params->gain = camera_config["gain"];
    camera_params->exposure = camera_config["exposure"];
    camera_params->pixel_format = camera_config["pixel_format"];
    camera_params->color_temp = camera_config["color_temp"];
    std::string source_gpu_error;
    if (!try_parse_source_gpu_id_from_json(camera_config,
                                           &camera_params->configured_gpu_id,
                                           nullptr,
                                           &source_gpu_error)) {
        throw std::runtime_error("Invalid camera config `" + file_name + "`: " + source_gpu_error);
    }
    camera_params->gpu_id = camera_params->configured_gpu_id;
    camera_params->gpu_id_runtime_overridden = false;
    camera_params->gpu_direct = camera_config["gpu_direct"];
    camera_params->focus_uart_bootstrap = camera_config.value("focus_uart_bootstrap", false);
    camera_params->lens_control_enabled = camera_config.value("lens_control_enabled", true);
    camera_params->color = camera_config["color"];
    camera_params->focus = camera_config["focus"];
    camera_params->iris = camera_config["iris"];
    camera_params->offsetx = camera_config.value("offset_x", 0u);
    camera_params->offsety = camera_config.value("offset_y", 0u);

    camera_params->config_schema_id = camera_config.value("schema_id", std::string());
    camera_params->config_schema_version = camera_config.value("schema_version", 0);
    camera_params->device_model =
        camera_config.value("device_model",
            camera_config.value("device_model_name", camera_params->device_model));
    if (camera_params->camera_serial.empty()) {
        camera_params->camera_serial = canonicalize_camera_serial_string(
            camera_config.value("device_serial_number", camera_params->camera_serial));
    }
    camera_params->camera_scan_type =
        normalize_camera_scan_type_string(camera_config.value("camera_scan_type", camera_params->camera_scan_type));
    camera_params->gpio_connector_variant = normalize_gpio_connector_variant_string(
        camera_config.value("gpio_connector_variant", camera_params->gpio_connector_variant));
    camera_params->gpio_recipe = canonicalize_gpio_recipe_string(camera_config.value("gpio_recipe", std::string()));
    if (!camera_params->config_schema_id.empty() &&
        camera_params->config_schema_id != kCameraConfigSchemaId) {
        std::cerr << "Camera config schema_id mismatch for " << file_name
                  << ": " << camera_params->config_schema_id
                  << " (expected " << kCameraConfigSchemaId << ")" << std::endl;
    }
    if (camera_params->config_schema_version > 0 &&
        !camera_config_schema_version_supported(camera_params->config_schema_version)) {
        std::cerr << "Camera config schema_version mismatch for " << file_name
                  << ": " << camera_params->config_schema_version
                  << " (expected " << kCameraConfigSchemaVersion << ")" << std::endl;
    }

    if (camera_config.contains("sync_mode") && camera_config["sync_mode"].is_string()) {
        camera_params->sync_mode = normalize_camera_sync_mode_string(camera_config["sync_mode"].get<std::string>());
    }

    if (camera_config.contains("trigger") && camera_config["trigger"].is_object()) {
        const nlohmann::json& trigger = camera_config["trigger"];
        camera_params->trigger_enabled = trigger.value("enabled", false);
        camera_params->trigger_selector = trigger.value("selector", camera_params->trigger_selector);
        camera_params->trigger_source = trigger.value("source", camera_params->trigger_source);
        camera_params->trigger_activation = trigger.value("activation", camera_params->trigger_activation);
    }

    if (camera_config.contains("ptp") && camera_config["ptp"].is_object()) {
        const nlohmann::json& ptp = camera_config["ptp"];
        if (ptp.contains("mode") && ptp["mode"].is_string()) {
            camera_params->ptp_mode = ptp["mode"].get<std::string>();
        } else if (!ptp.value("enabled", false)) {
            camera_params->ptp_mode.clear();
        }
        if ((!camera_config.contains("sync_mode") || !camera_config["sync_mode"].is_string()) &&
            ptp.value("enabled", false)) {
            camera_params->sync_mode = "ptp_gate";
        }
    }

    parse_gpio_nodes_from_json(camera_config, camera_params);
    parse_recording_config_from_json(camera_config, camera_params);
    parse_crop_pipeline_config_from_json(camera_config, camera_params);
    infer_camera_gpio_metadata(camera_params);
}

bool parse_recording_strategy_json(const nlohmann::json& recording_json,
                                   RecordingStrategyConfig* recording_strategy_out,
                                   std::string* error_out) {
    return parse_recording_strategy_json_object(recording_json, recording_strategy_out, error_out);
}

nlohmann::json build_recording_strategy_json(const RecordingStrategyConfig& recording_strategy) {
    return build_recording_strategy_json_object(recording_strategy);
}

bool parse_camera_recording_json(const nlohmann::json& recording_json,
                                 CameraRecordingConfig* recording_out,
                                 std::string* error_out) {
    return parse_camera_recording_json_impl(recording_json, recording_out, error_out);
}

nlohmann::json build_camera_recording_json(const CameraRecordingConfig& recording) {
    return build_camera_recording_json_impl(recording);
}

RecordingOutputConfig resolve_effective_recording_output_config(
    const CameraParams& camera_params,
    const CameraRecordingOutputConfig& requested_output,
    std::string* warning_out)
{
    if (warning_out) {
        warning_out->clear();
    }

    constexpr int kMinRecordingOutputDimension = 64;
    auto is_supported_record_output_factor = [](int factor) {
        return factor == 1 || factor == 2 || factor == 4 || factor == 8 || factor == 16;
    };

    std::string mode = normalize_recording_output_mode_string(requested_output.mode);
    int factor = requested_output.downsample_factor;
    int width = requested_output.requested_width;
    int height = requested_output.requested_height;

    if (!is_supported_record_output_factor(factor)) {
        factor = 1;
    }
    if (width < 1) {
        width = 1024;
    }
    if (height < 1) {
        height = 1024;
    }

    RecordingOutputConfig output;
    output.mode = mode;
    output.downsample_factor = factor;
    output.requested_width = width;
    output.requested_height = height;
    output.resolved_width = static_cast<int>(camera_params.width);
    output.resolved_height = static_cast<int>(camera_params.height);
    output.resize_enabled = false;

    auto fallback_to_native = [&](const std::string& warning) {
        if (warning_out) {
            *warning_out = warning;
        }
        output.mode = "factor";
        output.downsample_factor = 1;
        output.requested_width = static_cast<int>(camera_params.width);
        output.requested_height = static_cast<int>(camera_params.height);
        output.resolved_width = static_cast<int>(camera_params.width);
        output.resolved_height = static_cast<int>(camera_params.height);
        output.resize_enabled = false;
    };

    if (mode == "resolution") {
        if (width < kMinRecordingOutputDimension || height < kMinRecordingOutputDimension) {
            fallback_to_native("requested output size is smaller than the minimum supported recording dimension");
            return output;
        }
        if ((width % 2) != 0 || (height % 2) != 0) {
            fallback_to_native("requested output size must have even width and height for NV12");
            return output;
        }
        if (width > static_cast<int>(camera_params.width) || height > static_cast<int>(camera_params.height)) {
            fallback_to_native("requested output size cannot upscale beyond the camera source dimensions");
            return output;
        }
        const int64_t lhs = static_cast<int64_t>(width) * static_cast<int64_t>(camera_params.height);
        const int64_t rhs = static_cast<int64_t>(height) * static_cast<int64_t>(camera_params.width);
        if (lhs != rhs) {
            fallback_to_native("requested output size must preserve the source aspect ratio");
            return output;
        }

        output.resolved_width = width;
        output.resolved_height = height;
        output.resize_enabled =
            output.resolved_width != static_cast<int>(camera_params.width) ||
            output.resolved_height != static_cast<int>(camera_params.height);
        return output;
    }

    if (!is_supported_record_output_factor(factor)) {
        fallback_to_native("recording downsample factor must be one of 1, 2, 4, 8, or 16");
        return output;
    }

    if ((camera_params.width % static_cast<unsigned int>(factor)) != 0 ||
        (camera_params.height % static_cast<unsigned int>(factor)) != 0) {
        fallback_to_native("recording downsample factor must evenly divide the source dimensions");
        return output;
    }

    const int resolved_width = static_cast<int>(camera_params.width / static_cast<unsigned int>(factor));
    const int resolved_height = static_cast<int>(camera_params.height / static_cast<unsigned int>(factor));
    if (resolved_width < kMinRecordingOutputDimension || resolved_height < kMinRecordingOutputDimension) {
        fallback_to_native("recording downsample result is below the minimum supported output dimension");
        return output;
    }
    if ((resolved_width % 2) != 0 || (resolved_height % 2) != 0) {
        fallback_to_native("recording downsample result must have even width and height for NV12");
        return output;
    }

    output.requested_width = resolved_width;
    output.requested_height = resolved_height;
    output.resolved_width = resolved_width;
    output.resolved_height = resolved_height;
    output.resize_enabled = factor != 1;
    return output;
}

bool runtime_env_is_set(const char* name) {
    const char* env = std::getenv(name);
    return env && *env != '\0';
}

bool parse_runtime_env_flag(const char* name, bool default_value) {
    const char* env = std::getenv(name);
    if (!env || *env == '\0') {
        return default_value;
    }
    if (strcmp(env, "0") == 0 || strcmp(env, "false") == 0 || strcmp(env, "FALSE") == 0 ||
        strcmp(env, "off") == 0 || strcmp(env, "OFF") == 0) {
        return false;
    }
    return true;
}

uint64_t parse_runtime_env_u64(const char* name, uint64_t default_value) {
    const char* env = std::getenv(name);
    if (!env || *env == '\0') {
        return default_value;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(env, &end, 10);
    if (end == env || (end && *end != '\0')) {
        return default_value;
    }
    return static_cast<uint64_t>(value);
}

std::string parse_runtime_env_string(const char* name, std::string default_value) {
    const char* env = std::getenv(name);
    if (!env || *env == '\0') {
        return default_value;
    }
    return std::string(env);
}

std::vector<int> parse_runtime_env_int_list(const char* name) {
    std::vector<int> values;
    const char* env = std::getenv(name);
    if (!env || *env == '\0') {
        return values;
    }

    std::stringstream ss(env);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }), token.end());
        if (token.empty()) {
            continue;
        }
        char* end = nullptr;
        const long value = std::strtol(token.c_str(), &end, 10);
        if (end == token.c_str() || (end && *end != '\0')) {
            continue;
        }
        values.push_back(static_cast<int>(value));
    }
    return values;
}

void append_runtime_resolution_note(std::string* note, const std::string& message) {
    if (!note || message.empty()) {
        return;
    }
    if (!note->empty()) {
        *note += "; ";
    }
    *note += message;
}

void apply_runtime_recording_strategy_env_overrides(RecordingStrategyConfig* config) {
    if (!config) {
        return;
    }

    if (runtime_env_is_set("ORANGE_RECORDING_MODE") || runtime_env_is_set("ORANGE_GOP_EXPERIMENT")) {
        const bool legacy_split_gop_enabled = parse_runtime_env_flag("ORANGE_GOP_EXPERIMENT", false);
        config->requested_mode = normalize_recording_mode_string(parse_runtime_env_string(
            "ORANGE_RECORDING_MODE",
            legacy_split_gop_enabled ? "split_gop" : "single_session"));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_SOURCE_ENCODER_POLICY") ||
        runtime_env_is_set("ORANGE_GOP_ENCODER_POLICY")) {
        config->split_gop.source_encoder_policy = normalize_split_gop_source_encoder_policy_string(
            parse_runtime_env_string(
                "ORANGE_SPLIT_GOP_SOURCE_ENCODER_POLICY",
                parse_runtime_env_string(
                    "ORANGE_GOP_ENCODER_POLICY",
                    config->split_gop.source_encoder_policy)));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_TRANSFER_MODE") ||
        runtime_env_is_set("ORANGE_GOP_TRANSFER_MODE")) {
        config->split_gop.transfer_mode = normalize_split_gop_transfer_mode_string(
            parse_runtime_env_string(
                "ORANGE_SPLIT_GOP_TRANSFER_MODE",
                parse_runtime_env_string(
                    "ORANGE_GOP_TRANSFER_MODE",
                    config->split_gop.transfer_mode)));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_PLACEMENT")) {
        config->split_gop.placement = normalize_split_gop_placement_string(
            parse_runtime_env_string("ORANGE_SPLIT_GOP_PLACEMENT", config->split_gop.placement));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_ENCODER_GPU_IDS")) {
        config->split_gop.encoder_gpu_ids = parse_runtime_env_int_list("ORANGE_SPLIT_GOP_ENCODER_GPU_IDS");
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_INFLIGHT_GOPS") ||
        runtime_env_is_set("ORANGE_GOP_MAX_INFLIGHT_GOPS")) {
        config->split_gop.max_inflight_gops = parse_runtime_env_u64(
            "ORANGE_SPLIT_GOP_MAX_INFLIGHT_GOPS",
            parse_runtime_env_u64(
                "ORANGE_GOP_MAX_INFLIGHT_GOPS",
                config->split_gop.max_inflight_gops));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_BUFFERED_BYTES") ||
        runtime_env_is_set("ORANGE_GOP_MAX_BUFFERED_BYTES")) {
        config->split_gop.max_buffered_bytes = parse_runtime_env_u64(
            "ORANGE_SPLIT_GOP_MAX_BUFFERED_BYTES",
            parse_runtime_env_u64(
                "ORANGE_GOP_MAX_BUFFERED_BYTES",
                config->split_gop.max_buffered_bytes));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_WRITER_QUEUE_PACKETS") ||
        runtime_env_is_set("ORANGE_GOP_MAX_WRITER_QUEUE_PACKETS")) {
        config->split_gop.writer_queue.max_packets = parse_runtime_env_u64(
            "ORANGE_SPLIT_GOP_MAX_WRITER_QUEUE_PACKETS",
            parse_runtime_env_u64(
                "ORANGE_GOP_MAX_WRITER_QUEUE_PACKETS",
                config->split_gop.writer_queue.max_packets));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_WRITER_QUEUE_BYTES") ||
        runtime_env_is_set("ORANGE_GOP_MAX_WRITER_QUEUE_BYTES")) {
        config->split_gop.writer_queue.max_bytes = parse_runtime_env_u64(
            "ORANGE_SPLIT_GOP_MAX_WRITER_QUEUE_BYTES",
            parse_runtime_env_u64(
                "ORANGE_GOP_MAX_WRITER_QUEUE_BYTES",
                config->split_gop.writer_queue.max_bytes));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_FAIL_ON_WRITER_OVERFLOW") ||
        runtime_env_is_set("ORANGE_GOP_FAIL_ON_WRITER_OVERFLOW")) {
        config->split_gop.writer_queue.fail_on_overflow = parse_runtime_env_flag(
            "ORANGE_SPLIT_GOP_FAIL_ON_WRITER_OVERFLOW",
            parse_runtime_env_flag(
                "ORANGE_GOP_FAIL_ON_WRITER_OVERFLOW",
                config->split_gop.writer_queue.fail_on_overflow));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_STRICT")) {
        config->split_gop.strict = parse_runtime_env_flag(
            "ORANGE_SPLIT_GOP_STRICT",
            config->split_gop.strict);
    }
}

bool has_runtime_recording_strategy_env_override() {
    return runtime_env_is_set("ORANGE_RECORDING_MODE") ||
           runtime_env_is_set("ORANGE_GOP_EXPERIMENT") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_SOURCE_ENCODER_POLICY") ||
           runtime_env_is_set("ORANGE_GOP_ENCODER_POLICY") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_TRANSFER_MODE") ||
           runtime_env_is_set("ORANGE_GOP_TRANSFER_MODE") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_PLACEMENT") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_ENCODER_GPU_IDS") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_INFLIGHT_GOPS") ||
           runtime_env_is_set("ORANGE_GOP_MAX_INFLIGHT_GOPS") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_BUFFERED_BYTES") ||
           runtime_env_is_set("ORANGE_GOP_MAX_BUFFERED_BYTES") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_WRITER_QUEUE_PACKETS") ||
           runtime_env_is_set("ORANGE_GOP_MAX_WRITER_QUEUE_PACKETS") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_WRITER_QUEUE_BYTES") ||
           runtime_env_is_set("ORANGE_GOP_MAX_WRITER_QUEUE_BYTES") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_FAIL_ON_WRITER_OVERFLOW") ||
           runtime_env_is_set("ORANGE_GOP_FAIL_ON_WRITER_OVERFLOW") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_STRICT");
}

RecordingStrategyConfig resolve_runtime_recording_strategy_config(const CameraParams& camera_params) {
    RecordingStrategyConfig config = camera_params.recording.strategy;
    normalize_recording_strategy_config(&config);
    if (!has_runtime_recording_strategy_env_override()) {
        return config;
    }

    append_runtime_resolution_note(&config.resolution_note, "env override active");
    apply_runtime_recording_strategy_env_overrides(&config);
    normalize_recording_strategy_config(&config);
    return config;
}

ResolvedRecordingConfig build_resolved_recording_config(
    const CameraParams& camera_params,
    const ResolvedRecordingConfigOverrides& overrides)
{
    ResolvedRecordingConfig resolved;
    resolved.source_gpu_id = camera_params.gpu_id;
    resolved.recording_gpu_id =
        overrides.recording_gpu_id >= 0 ? overrides.recording_gpu_id : camera_params.gpu_id;
    resolved.encode = camera_params.recording.encode;
    if (!overrides.codec.empty()) {
        resolved.encode.codec = lower_ascii_copy(overrides.codec);
    }
    if (!overrides.preset.empty()) {
        resolved.encode.preset = lower_ascii_copy(overrides.preset);
    }
    if (!overrides.tuning.empty()) {
        resolved.encode.tuning = lower_ascii_copy(overrides.tuning);
    }
    if (!overrides.rate_control_mode.empty()) {
        resolved.encode.rate_control_mode = lower_ascii_copy(overrides.rate_control_mode);
    }
    if (overrides.quality_value >= 0) {
        resolved.encode.quality_value = overrides.quality_value;
    }
    if (overrides.gop_length >= 0) {
        resolved.encode.gop_length = overrides.gop_length;
    }
    if (overrides.has_nvenc_direct_input_override) {
        resolved.encode.nvenc_direct_input = overrides.nvenc_direct_input;
    } else {
        resolved.encode.nvenc_direct_input = parse_runtime_env_flag(
            "ORANGE_NVENC_DIRECT_INPUT",
            resolved.encode.nvenc_direct_input);
    }
    const CameraRecordingOutputConfig output_preferences =
        overrides.has_output_preferences_override
            ? overrides.output_preferences
            : camera_params.recording.output;
    resolved.output = resolve_effective_recording_output_config(camera_params, output_preferences, nullptr);
    resolved.strategy = resolve_runtime_recording_strategy_config(camera_params);
    resolved.constraints = camera_params.recording.constraints;
    resolved.resources = camera_params.recording.resources;
    resolved.encoder_control_overrides =
        resolve_encoder_control_overrides(resolved.encode, overrides.encoder_control_overrides);
    resolved.importance_map = overrides.importance_map;
    resolved.base_folder_name = overrides.base_folder_name;
    resolved.pre_encoder_reference_capture = overrides.pre_encoder_reference_capture;
    return resolved;
}

std::string get_current_utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm utc_time = *std::gmtime(&time_t_now);
    std::ostringstream oss;
    oss << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

namespace {
constexpr const char* kCalibrationRegistrySchemaId = "orange.calibration.registry";
constexpr int kCalibrationRegistrySchemaVersion = 1;

std::mutex& recording_snapshot_mutex() {
    static std::mutex m;
    return m;
}

std::mutex& calibration_registry_mutex() {
    static std::mutex m;
    return m;
}

std::mutex& ptp_sync_summary_mutex() {
    static std::mutex m;
    return m;
}

std::mutex& camera_config_mutex() {
    static std::mutex m;
    return m;
}

nlohmann::json build_recording_sync_snapshot(bool sync_camera_enabled,
                                             const PTPParams* ptp_params,
                                             int num_cameras) {
    nlohmann::json sync = nlohmann::json::object();
    sync["schema_version"] = 1;
    sync["captured_at_utc"] = get_current_utc_timestamp();
    sync["camera_sync_enabled"] = sync_camera_enabled;
    sync["num_cameras_expected"] = num_cameras;

    const bool has_ptp_state = (ptp_params != nullptr);
    const bool network_sync = has_ptp_state ? ptp_params->network_sync : false;
    sync["mode"] = !sync_camera_enabled ? "none" : (network_sync ? "ptp_network" : "ptp_local");
    sync["network_sync"] = network_sync;

    nlohmann::json gate_times = nlohmann::json::object();
    if (has_ptp_state && ptp_params->ptp_global_time != 0) {
        gate_times["start_ns"] = ptp_params->ptp_global_time;
    }
    if (has_ptp_state && ptp_params->ptp_stop_time != 0) {
        gate_times["stop_ns"] = ptp_params->ptp_stop_time;
    }
    sync["gate_times"] = gate_times;

    sync["barriers"] = {
        {"start", {
            {"participants_reached", has_ptp_state ? ptp_params->ptp_counter : 0},
            {"all_reached", has_ptp_state ? ptp_params->ptp_start_reached : false}
        }},
        {"stop", {
            {"participants_reached", has_ptp_state ? ptp_params->ptp_stop_counter : 0},
            {"all_reached", has_ptp_state ? ptp_params->ptp_stop_reached : false}
        }}
    };

    sync["signals"] = {
        {"start_observed", has_ptp_state ? ptp_params->network_set_start_ptp : false},
        {"stop_observed", has_ptp_state ? ptp_params->network_set_stop_ptp : false}
    };

    return sync;
}

nlohmann::json build_ptp_sync_summary_base(const std::string& recording_folder,
                                          const std::string& recording_id,
                                          int num_cameras,
                                          bool sync_camera_enabled,
                                          const PTPParams* ptp_params) {
    nlohmann::json summary = nlohmann::json::object();
    summary["schema_version"] = 1;
    summary["recording_id"] =
        recording_id.empty() ? std::filesystem::path(recording_folder).filename().string() : recording_id;
    summary["recording_folder"] = recording_folder;
    summary["created_at_utc"] = get_current_utc_timestamp();
    summary["updated_at_utc"] = summary["created_at_utc"];
    summary["sync"] = build_recording_sync_snapshot(sync_camera_enabled, ptp_params, num_cameras);
    summary["cameras"] = nlohmann::json::object();
    return summary;
}

std::string read_file_to_string(const std::string& path, std::string* error) {
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        if (error) {
            *error = "failed to open file";
        }
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool write_json_atomic(const std::filesystem::path& path,
                       const nlohmann::json& data,
                       std::filesystem::perms perms,
                       bool set_perms,
                       const char* label,
                       std::string* error_out = nullptr) {
    if (error_out) {
        error_out->clear();
    }
    std::filesystem::path tmp = path;
    tmp += ".tmp";

    std::ofstream out(tmp.string(), std::ios::trunc);
    if (!out.is_open()) {
        if (error_out) {
            *error_out = std::string("failed to open temp file: ") + tmp.string();
        }
        std::cerr << "Failed to write " << label << " temp file: " << tmp.string() << std::endl;
        return false;
    }
    out << data.dump(2) << std::endl;
    out.close();

    if (set_perms) {
        std::error_code ec;
        std::filesystem::permissions(tmp, perms, std::filesystem::perm_options::replace, ec);
        if (ec) {
            if (error_out && error_out->empty()) {
                *error_out = std::string("failed to set permissions on temp file: ") + ec.message();
            }
            std::cerr << "Failed to set permissions on " << tmp.string() << " ("
                      << ec.message() << ")" << std::endl;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        if (error_out) {
            *error_out = std::string("failed to rename temp file: ") + ec.message();
        }
        std::cerr << "Failed to rename " << label << " temp file: " << tmp.string()
                  << " -> " << path.string() << " (" << ec.message() << ")" << std::endl;
        std::filesystem::remove(tmp);
        return false;
    }

    return true;
}

bool write_latest_recording_pointer(const std::string& base_folder,
                                    const std::string& recording_folder,
                                    const std::string& recording_id,
                                    const std::string& timestamp_utc) {
    if (recording_folder.empty()) {
        return false;
    }

    nlohmann::json pointer;
    pointer["recording_id"] = recording_id;
    pointer["timestamp_utc"] = timestamp_utc;
    pointer["recording_folder"] = recording_folder;
    pointer["snapshot_path"] = (std::filesystem::path(recording_folder) / "recording_snapshot.json").string();
    pointer["recording_session_manifest_path"] =
        (std::filesystem::path(recording_folder) / "recording_session.json").string();

    bool wrote_any = false;
    bool attempted_any = false;
    AppStorageConfig app_storage_config;
    {
        std::string orange_root_warning;
        const std::string orange_root_dir = build_default_orange_root_dir(&orange_root_warning);
        if (!orange_root_dir.empty()) {
            std::string app_storage_error;
            if (!load_app_storage_config(orange_root_dir, &app_storage_config, &app_storage_error)) {
                std::cerr << "App storage config warning: " << app_storage_error << std::endl;
                app_storage_config.schema_id = kAppConfigSchemaId;
                app_storage_config.schema_version = kAppConfigSchemaVersion;
                app_storage_config.default_recording_root = default_recording_root_for_orange_root(orange_root_dir);
                app_storage_config.write_local_pointer = true;
                app_storage_config.canonical_pointer_root =
                    default_canonical_pointer_root_for_orange_root(orange_root_dir);
                app_storage_config.write_run_pointer = true;
                app_storage_config.run_pointer_path = default_run_pointer_path();
            }
        } else {
            if (!orange_root_warning.empty()) {
                std::cerr << "App storage config warning: " << orange_root_warning << std::endl;
            }
            app_storage_config.schema_id = kAppConfigSchemaId;
            app_storage_config.schema_version = kAppConfigSchemaVersion;
            app_storage_config.write_local_pointer = true;
            app_storage_config.write_run_pointer = true;
            app_storage_config.run_pointer_path = default_run_pointer_path();
        }
    }

    if (app_storage_config.write_local_pointer && !base_folder.empty()) {
        attempted_any = true;
        std::filesystem::path meta_dir = std::filesystem::path(base_folder) / ".orange";
        std::filesystem::path pointer_path = meta_dir / "latest_recording.json";

        {
            orange::ScopedFsuid fsuid_guard;
            (void)fsuid_guard;

            std::error_code ec;
            std::filesystem::create_directories(meta_dir, ec);
            if (ec) {
                std::cerr << "Error creating metadata folder: " << meta_dir.string()
                          << " (" << ec.message() << ")" << std::endl;
            } else {
                if (!write_json_atomic(pointer_path, pointer, std::filesystem::perms::unknown, false,
                                       "latest recording pointer")) {
                    std::cerr << "Failed to write latest recording pointer: " << pointer_path.string() << std::endl;
                } else {
                    wrote_any = true;
                }
            }
        }
    }

    if (!app_storage_config.canonical_pointer_root.empty()) {
        attempted_any = true;
        std::filesystem::path canonical_root = app_storage_config.canonical_pointer_root;
        std::filesystem::path pointer_path = canonical_root / "latest_recording.json";

        {
            orange::ScopedFsuid fsuid_guard;
            (void)fsuid_guard;

            std::error_code ec;
            std::filesystem::create_directories(canonical_root, ec);
            if (ec) {
                std::cerr << "Error creating canonical metadata folder: " << canonical_root.string()
                          << " (" << ec.message() << ")" << std::endl;
            } else {
                if (!write_json_atomic(pointer_path, pointer, std::filesystem::perms::unknown, false,
                                       "canonical latest recording pointer")) {
                    std::cerr << "Failed to write canonical latest recording pointer: "
                              << pointer_path.string() << std::endl;
                } else {
                    wrote_any = true;
                }
            }
        }
    }

    if (app_storage_config.write_run_pointer && !app_storage_config.run_pointer_path.empty()) {
        std::filesystem::path run_pointer = app_storage_config.run_pointer_path;
        std::filesystem::path run_dir = run_pointer.parent_path();
        attempted_any = true;
        if (run_dir.empty()) {
            std::cerr << "Invalid run metadata pointer path (missing parent directory): "
                      << run_pointer.string() << std::endl;
            return wrote_any || !attempted_any;
        }
        std::error_code ec;
        std::filesystem::create_directories(run_dir, ec);
        if (ec) {
            std::cerr << "Error creating run metadata folder: " << run_dir.string()
                      << " (" << ec.message() << ")" << std::endl;
        } else {
            std::filesystem::permissions(
                run_dir,
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                    std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
                    std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                    std::filesystem::perms::others_exec,
                std::filesystem::perm_options::replace,
                ec);
            if (ec) {
                std::cerr << "Failed to set permissions on " << run_dir.string()
                          << " (" << ec.message() << ")" << std::endl;
            }

            std::filesystem::perms run_perms =
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                std::filesystem::perms::group_read | std::filesystem::perms::others_read;
            if (!write_json_atomic(run_pointer, pointer, run_perms, true, "run latest recording pointer")) {
                std::cerr << "Failed to write run latest recording pointer: " << run_pointer.string() << std::endl;
            } else {
                wrote_any = true;
            }
        }
    }

    return wrote_any || !attempted_any;
}
} // namespace

std::string get_current_time_milliseconds() {
    // ... (implementation from project.h)
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto time = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%H_%M_%S_") << std::setfill('0') << std::setw(3) << ms.count();

    return oss.str();
}

std::string get_current_date() {
    // ... (implementation from project.h)
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm local_time = *std::localtime(&time_t_now);
    std::ostringstream oss;
    oss << std::put_time(&local_time, "%Y_%m_%d");
    return oss.str();
}

std::string get_current_date_time() {
    // ... (implementation from project.h)
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm local_time = *std::localtime(&time_t_now);
    std::ostringstream oss;
    oss << std::put_time(&local_time, "%Y_%m_%d_%H_%M_%S");
    return oss.str();
}

std::string format_elapsed_time(std::chrono::seconds elapsed_seconds) {
    // ... (implementation from project.h)
    int hours = static_cast<int>(elapsed_seconds.count() / 3600);
    int minutes = static_cast<int>((elapsed_seconds.count() % 3600) / 60);
    int seconds = static_cast<int>(elapsed_seconds.count() % 60);

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << hours << ":"
        << std::setfill('0') << std::setw(2) << minutes << ":"
        << std::setfill('0') << std::setw(2) << seconds;
    return oss.str();
}

void init_galvo_camera_params(CameraParams* camera_params, int camera_id, int num_cameras, int gain, int exposure)
{
    // ... (implementation from project.h)
    camera_params->width = 1280;
    camera_params->height = 1280;
    camera_params->frame_rate = 100;
    camera_params->gain = gain;
    camera_params->exposure = exposure;
    camera_params->pixel_format = "BayerRG8";
    camera_params->color_temp = "CT_3000K";
    camera_params->camera_id = camera_id;
    camera_params->gpu_id = 1;
    camera_params->configured_gpu_id = camera_params->gpu_id;
    camera_params->gpu_id_runtime_overridden = false;
    camera_params->num_cameras = num_cameras;
    camera_params->gpu_direct = false;
    camera_params->focus_uart_bootstrap = false;
    camera_params->lens_control_enabled = true;
    camera_params->need_reorder = false;
    camera_params->color = true;
    camera_params->iris = 0;
}

void init_65MP_camera_params_mono(CameraParams* camera_params, int camera_id, int num_cameras, int gain, int exposure, int gpu_id, int frame_rate)
{
    // ... (implementation from project.h)
    camera_params->width = 512;
    camera_params->height = 512;
    camera_params->frame_rate = frame_rate;
    camera_params->gain = gain;
    camera_params->exposure = exposure;
    camera_params->pixel_format = "Mono8";
    camera_params->gpu_id = gpu_id;
    camera_params->configured_gpu_id = camera_params->gpu_id;
    camera_params->gpu_id_runtime_overridden = false;
    camera_params->num_cameras = num_cameras;
    camera_params->gpu_direct = false;
    camera_params->focus_uart_bootstrap = false;
    camera_params->lens_control_enabled = true;
    camera_params->need_reorder = false;
    camera_params->focus = 4311;
    camera_params->camera_id = camera_id;
    camera_params->color = false;
    camera_params->iris = 0;
}

void init_65MP_camera_params_color(CameraParams* camera_params, int camera_id, int num_cameras, int gain, int exposure, int gpu_id, int frame_rate)
{
    // ... (implementation from project.h)
    camera_params->width = 512; 
    camera_params->height = 512; 
    camera_params->frame_rate = frame_rate;
    camera_params->gain = gain;
    camera_params->exposure = exposure;
    camera_params->pixel_format = "BayerGB8";
    camera_params->gpu_id = gpu_id;
    camera_params->configured_gpu_id = camera_params->gpu_id;
    camera_params->gpu_id_runtime_overridden = false;
    camera_params->num_cameras = num_cameras;
    camera_params->gpu_direct = false;
    camera_params->focus_uart_bootstrap = false;
    camera_params->lens_control_enabled = true;
    camera_params->need_reorder = false;
    camera_params->focus = 4419;
    camera_params->camera_id = camera_id;
    camera_params->color = true;
    camera_params->color_temp = "CT_3000K";
    camera_params->iris = 0;
}

void init_7MP_camera_params_color(CameraParams* camera_params, int camera_id, int num_cameras, int gain, int exposure, int gpu_id, int frame_rate)
{
    // ... (implementation from project.h)
    camera_params->width = 3208;
    camera_params->height = 2200;
    camera_params->frame_rate = frame_rate;
    camera_params->gain = gain;
    camera_params->exposure = exposure;
    camera_params->pixel_format = "BayerRG8";
    camera_params->color_temp = "CT_3000K";
    camera_params->gpu_id = gpu_id;
    camera_params->configured_gpu_id = camera_params->gpu_id;
    camera_params->gpu_id_runtime_overridden = false;
    camera_params->num_cameras = num_cameras;
    camera_params->gpu_direct = false;
    camera_params->focus_uart_bootstrap = false;
    camera_params->lens_control_enabled = true;
    camera_params->need_reorder = false;
    camera_params->focus = 345;
    camera_params->camera_id = camera_id;
    camera_params->color = true;
    camera_params->iris = 0;
}

void init_7MP_camera_params_mono(CameraParams* camera_params, int camera_id, int num_cameras, int gain, int exposure, int gpu_id, int frame_rate)
{
    // ... (implementation from project.h)
    camera_params->width = 3208;
    camera_params->height = 2200;
    camera_params->frame_rate = frame_rate;
    camera_params->gain = gain;
    camera_params->exposure = exposure;
    camera_params->pixel_format = "Mono8";
    camera_params->color_temp = "CT_3000K";
    camera_params->gpu_id = gpu_id;
    camera_params->configured_gpu_id = camera_params->gpu_id;
    camera_params->gpu_id_runtime_overridden = false;
    camera_params->num_cameras = num_cameras;
    camera_params->gpu_direct = false;
    camera_params->focus_uart_bootstrap = false;
    camera_params->lens_control_enabled = true;
    camera_params->need_reorder = false;
    camera_params->focus = 4700;
    camera_params->camera_id = camera_id;
    camera_params->color = false;
    camera_params->iris = 0;
}

bool make_folder(std::string folder_name)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    // ... (implementation from project.h)
    if (!std::filesystem::exists(folder_name))
    {
        if(!std::filesystem::create_directories(folder_name)) {
            std::cerr << "Error creating folder: " << folder_name << std::endl;
        }
        return false;
    }
    return true;
}

bool ensure_directory_exists(const std::string& folder_name, std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (folder_name.empty()) {
        if (error_out) {
            *error_out = "folder path missing";
        }
        return false;
    }

    const std::filesystem::path folder_path(folder_name);
    std::error_code ec;
    if (std::filesystem::exists(folder_path, ec)) {
        if (ec) {
            if (error_out) {
                *error_out = std::string("failed to query folder: ") + ec.message();
            }
            return false;
        }
        if (!std::filesystem::is_directory(folder_path, ec) || ec) {
            if (error_out) {
                *error_out = ec ? std::string("failed to inspect folder: ") + ec.message()
                                : "path exists but is not a directory";
            }
            return false;
        }
        return true;
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    if (!std::filesystem::create_directories(folder_path, ec) && ec) {
        if (error_out) {
            *error_out = std::string("failed to create folder: ") + ec.message();
        }
        return false;
    }
    return true;
}

void list_child_directories(const std::string& root_folder, std::vector<std::string>& child_directories)
{
    child_directories.clear();
    std::error_code ec;
    if (!std::filesystem::exists(root_folder, ec) || ec) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(root_folder, ec)) {
        if (ec) {
            child_directories.clear();
            return;
        }
        if (entry.is_directory(ec) && !ec) {
            child_directories.push_back(entry.path().string());
        }
    }
    std::sort(child_directories.begin(), child_directories.end());
}

void update_camera_configs(std::vector<std::string>& camera_config_files, std::string input_folder)
{
    // ... (implementation from project.h)
    camera_config_files.clear();
    std::string camera_config_dir = input_folder;
    std::error_code ec;
    std::filesystem::directory_iterator entries(camera_config_dir, ec);
    if (ec) {
        std::cerr << "Failed to scan camera config directory `"
                  << camera_config_dir << "`: " << ec.message() << std::endl;
        return;
    }
    for (const auto &entry : entries)
    {
        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec) || entry_ec) {
            continue;
        }
        const std::filesystem::path path = entry.path();
        const std::string filename = path.filename().string();
        if (!filename.empty() && filename.front() == '.') {
            continue;
        }
        if (path.extension() == ".json") {
            camera_config_files.push_back(path.string());
        }
    }
    std::sort(camera_config_files.begin(), camera_config_files.end());
}

std::string build_camera_config_path(const std::string& config_folder, const CameraParams& camera_params)
{
    if (config_folder.empty() || camera_params.camera_serial.empty()) {
        return {};
    }
    return (std::filesystem::path(config_folder) /
            (canonicalize_camera_serial_string(camera_params.camera_serial) + ".json")).string();
}

void assign_camera_config_paths(CameraParams* cameras_params, int num_cameras, const std::string& config_folder)
{
    if (!cameras_params) {
        return;
    }
    for (int i = 0; i < num_cameras; ++i) {
        cameras_params[i].config_path = build_camera_config_path(config_folder, cameras_params[i]);
    }
}

void select_cameras_have_configs(std::vector<std::string>& camera_config_files, GigEVisionDeviceInfo* device_info, bool* check, int cam_count)
{
    // ... (implementation from project.h)
    for (int i=0; i<cam_count; i++) {
        std::string camera_serial = device_info[i].serialNumber;
        auto it = find_camera_config_for_serial(camera_config_files, camera_serial);
        if (it != camera_config_files.end()) {
            check[i] = true;
        } else {
            check[i] = false;
        }
    }
}

bool set_camera_params(CameraParams* camera_params, GigEVisionDeviceInfo* device_info, std::vector<std::string>& camera_config_files, int camera_idx, int num_cameras)
{
    // ... (implementation from project.h)
    camera_params->camera_serial.assign(canonicalize_camera_serial_string(device_info->serialNumber));
    camera_params->device_model = device_info->modelName;
    camera_params->camera_name = camera_params->camera_serial;
    camera_params->config_path.clear();

    auto it = find_camera_config_for_serial(camera_config_files, camera_params->camera_serial);

    if (it == camera_config_files.end())
    {
        if (strcmp(device_info->modelName, "HB-65000GM")==0) {
            int gpu_id = 0;
            init_65MP_camera_params_mono(camera_params, camera_idx, num_cameras, 2000, 1000, gpu_id, 400);
        } else if (strcmp(device_info->modelName, "HB-7000SC")==0) {
            int gpu_id = 0;
            init_7MP_camera_params_color(camera_params, camera_idx, num_cameras, 1500, 3000, gpu_id, 30);
        } else if (strcmp(device_info->modelName, "HB-65000GC")==0) {
            int gpu_id = 0;
            init_65MP_camera_params_color(camera_params, camera_idx, num_cameras, 2000, 28000, gpu_id, 10);
        } else if (strcmp(device_info->modelName, "HB-7000SM")==0) {
            int gpu_id = 0;
            init_7MP_camera_params_mono(camera_params, camera_idx, num_cameras, 1000, 3000, gpu_id, 30);
        } else {
            printf("Use default parameters. \n");
            return false;
        }
    } else {
        auto config_idx = std::distance(camera_config_files.cbegin(), it);
        std::cout << "Load camera json file: " << camera_config_files[config_idx] << std::endl;
        camera_params->config_path = camera_config_files[config_idx];
        load_camera_json_config_files(camera_config_files[config_idx], camera_params, camera_idx, num_cameras);
    }
    infer_camera_gpio_metadata(camera_params);
    return true;
}

bool write_recording_snapshot(const std::string& recording_folder,
                              const std::string& recording_id,
                              const CameraParams* cameras_params,
                              int num_cameras,
                              const std::string& base_folder,
                              bool update_latest_pointer,
                              bool sync_camera_enabled,
                              const PTPParams* ptp_params,
                              const std::string& recording_sink_mode) {
    if (!cameras_params || num_cameras <= 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(recording_snapshot_mutex());

    nlohmann::json snapshot;
    std::string resolved_recording_id = recording_id.empty() ? get_current_date_time() : recording_id;
    std::string timestamp_utc = get_current_utc_timestamp();
    snapshot["schema_version"] = 2;
    snapshot["recording_id"] = resolved_recording_id;
    snapshot["timestamp_utc"] = timestamp_utc;
    const nlohmann::json source_version = build_source_version_snapshot();
    snapshot["source_version"] = source_version;
    snapshot["producer_version"] =
        source_version.value("commit_short", std::string("unknown"));
    snapshot["session"] = {
        {"recording_sink_mode", recording_sink_mode.empty() ? "real" : recording_sink_mode},
        {"full_frame_video_enabled",
         recording_sink_mode.empty() || recording_sink_mode == "real"},
        {"system_cpu", build_system_cpu_runtime_snapshot()},
        {"yolo_worker", build_yolo_worker_runtime_snapshot(cameras_params, num_cameras)}
    };

    nlohmann::json cameras = nlohmann::json::object();
    nlohmann::json camera_runtime = nlohmann::json::object();

    for (int i = 0; i < num_cameras; ++i) {
        const CameraParams& params = cameras_params[i];
        std::string config_error;
        std::string config_contents;
        nlohmann::json config_json;
        bool config_ok = false;

        if (!params.config_path.empty()) {
            config_contents = read_file_to_string(params.config_path, &config_error);
            if (!config_contents.empty()) {
                try {
                    config_json = nlohmann::json::parse(config_contents);
                    config_ok = true;
                } catch (const std::exception& ex) {
                    config_error = std::string("config parse failed: ") + ex.what();
                }
            } else if (config_error.empty()) {
                config_error = "config file empty";
            }
        } else {
            config_error = "config path missing";
        }

        std::string camera_key = params.camera_serial;
        if (camera_key.empty() && config_ok && config_json.contains("device_serial_number") &&
            config_json["device_serial_number"].is_string()) {
            camera_key = config_json["device_serial_number"].get<std::string>();
        }
        if (camera_key.empty()) {
            camera_key = std::to_string(params.camera_id);
        }
        if (config_ok) {
            cameras[camera_key] = config_json;
        } else {
            cameras[camera_key] = nullptr;
            if (!config_error.empty()) {
                std::cerr << "Camera " << params.camera_id << " config missing: " << config_error << std::endl;
            }
        }
        camera_runtime[camera_key] = build_camera_runtime_snapshot(params);
    }

    snapshot["cameras"] = cameras;
    snapshot["camera_runtime"] = camera_runtime;
    snapshot["recording_outputs"] = build_initial_recording_outputs_snapshot(
        cameras_params,
        num_cameras,
        recording_sink_mode);
    snapshot["sync"] = build_recording_sync_snapshot(sync_camera_enabled, ptp_params, num_cameras);

    nlohmann::json gpu_inventory = nlohmann::json::object();
    std::set<int> seen_gpu_ids;
    for (int i = 0; i < num_cameras; ++i) {
        const int gpu_id = cameras_params[i].gpu_id;
        if (!seen_gpu_ids.insert(gpu_id).second) {
            continue;
        }
        gpu_inventory[std::to_string(gpu_id)] = build_gpu_runtime_info(gpu_id);
    }
    snapshot["gpu_inventory"] = gpu_inventory;

    bool wrote_snapshot = false;
    if (!recording_folder.empty()) {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        std::filesystem::path out_path = std::filesystem::path(recording_folder) / "recording_snapshot.json";
        if (!write_json_atomic(out_path, snapshot, std::filesystem::perms::unknown, false, "recording snapshot")) {
            return false;
        }
        wrote_snapshot = true;
    }

    if (wrote_snapshot && update_latest_pointer) {
        if (!write_latest_recording_pointer(base_folder, recording_folder, resolved_recording_id, timestamp_utc)) {
            std::cerr << "Failed to update latest recording pointer in base folder." << std::endl;
        }
    }

    return true;
}

bool initialize_ptp_sync_summary(const std::string& recording_folder,
                                 const std::string& recording_id,
                                 int num_cameras,
                                 bool sync_camera_enabled,
                                 const PTPParams* ptp_params) {
    if (recording_folder.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(ptp_sync_summary_mutex());
    const std::filesystem::path summary_path =
        std::filesystem::path(recording_folder) / "ptp_sync_summary.json";
    const nlohmann::json summary = build_ptp_sync_summary_base(
        recording_folder,
        recording_id,
        num_cameras,
        sync_camera_enabled,
        ptp_params);

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    return write_json_atomic(summary_path, summary, std::filesystem::perms::unknown, false, "ptp sync summary");
}

bool update_ptp_sync_summary_camera(const std::string& recording_folder,
                                    const std::string& camera_serial,
                                    const nlohmann::json& camera_summary) {
    if (recording_folder.empty() || camera_serial.empty()) {
        return false;
    }

    const std::filesystem::path summary_path =
        std::filesystem::path(recording_folder) / "ptp_sync_summary.json";

    std::lock_guard<std::mutex> lock(ptp_sync_summary_mutex());

    nlohmann::json summary;
    std::string error;
    const std::string contents = read_file_to_string(summary_path.string(), &error);
    if (!contents.empty()) {
        try {
            summary = nlohmann::json::parse(contents);
        } catch (const std::exception& ex) {
            std::cerr << "Failed to parse ptp sync summary: " << summary_path.string()
                      << " (" << ex.what() << ")" << std::endl;
            summary = nlohmann::json::object();
        }
    } else {
        if (!error.empty() && error != "failed to open file") {
            std::cerr << "Failed to read ptp sync summary: " << summary_path.string()
                      << " (" << error << ")" << std::endl;
        }
        summary = build_ptp_sync_summary_base(recording_folder, "", 0, false, nullptr);
    }

    if (!summary.is_object()) {
        summary = nlohmann::json::object();
    }
    if (!summary.contains("cameras") || !summary["cameras"].is_object()) {
        summary["cameras"] = nlohmann::json::object();
    }

    summary["updated_at_utc"] = get_current_utc_timestamp();
    summary["cameras"][camera_serial] = camera_summary;

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    return write_json_atomic(summary_path, summary, std::filesystem::perms::unknown, false, "ptp sync summary");
}

bool update_recording_snapshot_encoder(const std::string& recording_folder,
                                       const std::string& camera_serial,
                                       const nlohmann::json& encoder_info) {
    if (recording_folder.empty() || camera_serial.empty()) {
        return false;
    }

    const std::filesystem::path snapshot_path =
        std::filesystem::path(recording_folder) / "recording_snapshot.json";

    std::lock_guard<std::mutex> lock(recording_snapshot_mutex());

    std::string error;
    std::string contents = read_file_to_string(snapshot_path.string(), &error);
    if (contents.empty()) {
        std::cerr << "Failed to read recording snapshot: " << snapshot_path.string()
                  << " (" << (error.empty() ? "empty file" : error) << ")" << std::endl;
        return false;
    }

    nlohmann::json snapshot;
    try {
        snapshot = nlohmann::json::parse(contents);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to parse recording snapshot: " << snapshot_path.string()
                  << " (" << ex.what() << ")" << std::endl;
        return false;
    }

    if (!snapshot.is_object()) {
        snapshot = nlohmann::json::object();
    }
    if (!snapshot.contains("encoders") || !snapshot["encoders"].is_object()) {
        snapshot["encoders"] = nlohmann::json::object();
    }

    snapshot["schema_version"] = 2;
    snapshot["encoders"][camera_serial] = encoder_info;
    if (snapshot.contains("recording_outputs") &&
        snapshot["recording_outputs"].is_object() &&
        snapshot["recording_outputs"].contains(camera_serial) &&
        snapshot["recording_outputs"][camera_serial].is_object() &&
        snapshot["recording_outputs"][camera_serial].contains("full") &&
        snapshot["recording_outputs"][camera_serial]["full"].is_object()) {
        nlohmann::json& full_output = snapshot["recording_outputs"][camera_serial]["full"];
        if (encoder_info.contains("codec") && encoder_info["codec"].is_string()) {
            full_output["codec"] = encoder_info["codec"];
        }
        if (encoder_info.contains("tuning") && encoder_info["tuning"].is_string()) {
            full_output["tuning"] = encoder_info["tuning"];
        }
        if (encoder_info.contains("path") && encoder_info["path"].is_string()) {
            full_output["video"] = encoder_info["path"];
        }
        if (encoder_info.contains("resolution") && encoder_info["resolution"].is_object()) {
            const nlohmann::json& resolution = encoder_info["resolution"];
            if (resolution.contains("width")) {
                full_output["width"] = resolution["width"];
            }
            if (resolution.contains("height")) {
                full_output["height"] = resolution["height"];
            }
        }
    }
    if (snapshot.contains("recording_outputs") &&
        snapshot["recording_outputs"].is_object() &&
        snapshot["recording_outputs"].contains(camera_serial) &&
        snapshot["recording_outputs"][camera_serial].is_object() &&
        snapshot["recording_outputs"][camera_serial].contains("full")) {
        if (!snapshot["encoders"][camera_serial].contains("outputs") ||
            !snapshot["encoders"][camera_serial]["outputs"].is_object()) {
            snapshot["encoders"][camera_serial]["outputs"] = nlohmann::json::object();
        }
        snapshot["encoders"][camera_serial]["outputs"]["full"] =
            snapshot["recording_outputs"][camera_serial]["full"];
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    if (!write_json_atomic(snapshot_path, snapshot, std::filesystem::perms::unknown, false, "recording snapshot")) {
        return false;
    }

    return true;
}

bool update_recording_snapshot_pipeline_metrics(const std::string& recording_folder,
                                                const std::string& camera_serial,
                                                const nlohmann::json& pipeline_info) {
    if (recording_folder.empty() || camera_serial.empty()) {
        return false;
    }

    const std::filesystem::path snapshot_path =
        std::filesystem::path(recording_folder) / "recording_snapshot.json";

    std::lock_guard<std::mutex> lock(recording_snapshot_mutex());

    std::string error;
    std::string contents = read_file_to_string(snapshot_path.string(), &error);
    if (contents.empty()) {
        std::cerr << "Failed to read recording snapshot: " << snapshot_path.string()
                  << " (" << (error.empty() ? "empty file" : error) << ")" << std::endl;
        return false;
    }

    nlohmann::json snapshot;
    try {
        snapshot = nlohmann::json::parse(contents);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to parse recording snapshot: " << snapshot_path.string()
                  << " (" << ex.what() << ")" << std::endl;
        return false;
    }

    if (!snapshot.is_object()) {
        snapshot = nlohmann::json::object();
    }
    if (!snapshot.contains("pipeline_metrics") || !snapshot["pipeline_metrics"].is_object()) {
        snapshot["pipeline_metrics"] = nlohmann::json::object();
    }

    snapshot["pipeline_metrics"][camera_serial] = pipeline_info;

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    if (!write_json_atomic(snapshot_path, snapshot, std::filesystem::perms::unknown, false, "recording snapshot")) {
        return false;
    }

    return true;
}

bool update_recording_snapshot_gpu_monitoring(const std::string& recording_folder,
                                              const std::string& monitor_name,
                                              const nlohmann::json& monitor_info) {
    if (recording_folder.empty() || monitor_name.empty()) {
        return false;
    }

    const std::filesystem::path snapshot_path =
        std::filesystem::path(recording_folder) / "recording_snapshot.json";

    std::lock_guard<std::mutex> lock(recording_snapshot_mutex());

    std::string error;
    std::string contents = read_file_to_string(snapshot_path.string(), &error);
    if (contents.empty()) {
        std::cerr << "Failed to read recording snapshot: " << snapshot_path.string()
                  << " (" << (error.empty() ? "empty file" : error) << ")" << std::endl;
        return false;
    }

    nlohmann::json snapshot;
    try {
        snapshot = nlohmann::json::parse(contents);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to parse recording snapshot: " << snapshot_path.string()
                  << " (" << ex.what() << ")" << std::endl;
        return false;
    }

    if (!snapshot.is_object()) {
        snapshot = nlohmann::json::object();
    }
    if (!snapshot.contains("gpu_monitoring") || !snapshot["gpu_monitoring"].is_object()) {
        snapshot["gpu_monitoring"] = nlohmann::json::object();
    }

    snapshot["gpu_monitoring"][monitor_name] = monitor_info;

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    if (!write_json_atomic(snapshot_path, snapshot, std::filesystem::perms::unknown, false, "recording snapshot")) {
        return false;
    }

    return true;
}

bool update_recording_snapshot_session_artifacts(const std::string& recording_folder,
                                                 const nlohmann::json& session_info) {
    if (recording_folder.empty() || !session_info.is_object()) {
        return false;
    }

    const std::filesystem::path snapshot_path =
        std::filesystem::path(recording_folder) / "recording_snapshot.json";

    std::lock_guard<std::mutex> lock(recording_snapshot_mutex());

    nlohmann::json snapshot;
    if (!read_recording_snapshot_locked(snapshot_path, &snapshot)) {
        return false;
    }
    if (!snapshot.contains("session") || !snapshot["session"].is_object()) {
        snapshot["session"] = nlohmann::json::object();
    }
    for (auto it = session_info.begin(); it != session_info.end(); ++it) {
        snapshot["session"][it.key()] = it.value();
    }
    snapshot["session"]["updated_at_utc"] = get_current_utc_timestamp();

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    return write_json_atomic(
        snapshot_path,
        snapshot,
        std::filesystem::perms::unknown,
        false,
        "recording snapshot");
}

bool update_recording_snapshot_recording_outputs(const std::string& recording_folder,
                                                 const nlohmann::json& recording_outputs) {
    if (recording_folder.empty() || !recording_outputs.is_object()) {
        return false;
    }

    const std::filesystem::path snapshot_path =
        std::filesystem::path(recording_folder) / "recording_snapshot.json";

    std::lock_guard<std::mutex> lock(recording_snapshot_mutex());

    nlohmann::json snapshot;
    if (!read_recording_snapshot_locked(snapshot_path, &snapshot)) {
        return false;
    }

    snapshot["schema_version"] = 2;
    nlohmann::json merged_outputs =
        snapshot.value("recording_outputs", nlohmann::json::object());
    if (!merged_outputs.is_object()) {
        merged_outputs = nlohmann::json::object();
    }
    for (auto camera_it = recording_outputs.begin(); camera_it != recording_outputs.end(); ++camera_it) {
        if (!camera_it.value().is_object()) {
            continue;
        }
        if (!merged_outputs.contains(camera_it.key()) ||
            !merged_outputs[camera_it.key()].is_object()) {
            merged_outputs[camera_it.key()] = nlohmann::json::object();
        }
        for (auto output_it = camera_it.value().begin();
             output_it != camera_it.value().end();
             ++output_it) {
            nlohmann::json merged_output =
                merged_outputs[camera_it.key()].value(output_it.key(), nlohmann::json::object());
            if (!merged_output.is_object()) {
                merged_output = nlohmann::json::object();
            }
            if (output_it.value().is_object()) {
                for (auto field_it = output_it.value().begin();
                     field_it != output_it.value().end();
                     ++field_it) {
                    merged_output[field_it.key()] = field_it.value();
                }
            } else {
                merged_output = output_it.value();
            }
            merged_outputs[camera_it.key()][output_it.key()] = merged_output;
        }
    }
    snapshot["recording_outputs"] = merged_outputs;
    snapshot["recording_outputs_updated_at_utc"] = get_current_utc_timestamp();
    if (!snapshot.contains("encoders") || !snapshot["encoders"].is_object()) {
        snapshot["encoders"] = nlohmann::json::object();
    }
    for (auto camera_it = merged_outputs.begin(); camera_it != merged_outputs.end(); ++camera_it) {
        if (!camera_it.value().is_object()) {
            continue;
        }
        if (!snapshot["encoders"].contains(camera_it.key()) ||
            !snapshot["encoders"][camera_it.key()].is_object()) {
            snapshot["encoders"][camera_it.key()] = nlohmann::json::object();
        }
        snapshot["encoders"][camera_it.key()]["outputs"] = camera_it.value();
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    return write_json_atomic(
        snapshot_path,
        snapshot,
        std::filesystem::perms::unknown,
        false,
        "recording snapshot");
}

bool update_recording_snapshot_model(const std::string& recording_folder,
                                     const std::string& camera_serial,
                                     const std::string& model_kind,
                                     const nlohmann::json& model_info) {
    if (recording_folder.empty() || camera_serial.empty() || model_kind.empty()) {
        return false;
    }

    const std::filesystem::path snapshot_path =
        std::filesystem::path(recording_folder) / "recording_snapshot.json";

    std::lock_guard<std::mutex> lock(recording_snapshot_mutex());

    nlohmann::json snapshot;
    if (!read_recording_snapshot_locked(snapshot_path, &snapshot)) {
        return false;
    }

    if (!snapshot.contains("models") || !snapshot["models"].is_object()) {
        snapshot["models"] = nlohmann::json::object();
    }
    if (!snapshot["models"].contains(camera_serial) ||
        !snapshot["models"][camera_serial].is_object()) {
        snapshot["models"][camera_serial] = nlohmann::json::object();
    }

    snapshot["models"][camera_serial][model_kind] = model_info;

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    if (!write_json_atomic(snapshot_path, snapshot, std::filesystem::perms::unknown, false, "recording snapshot")) {
        return false;
    }

    return true;
}

bool update_recording_snapshot_crop_output(const std::string& recording_folder,
                                           const std::string& camera_serial,
                                           const nlohmann::json& crop_output_info) {
    if (recording_folder.empty() || camera_serial.empty()) {
        return false;
    }

    const std::filesystem::path snapshot_path =
        std::filesystem::path(recording_folder) / "recording_snapshot.json";

    std::lock_guard<std::mutex> lock(recording_snapshot_mutex());

    nlohmann::json snapshot;
    if (!read_recording_snapshot_locked(snapshot_path, &snapshot)) {
        return false;
    }

    if (!snapshot.contains("crop_outputs") || !snapshot["crop_outputs"].is_object()) {
        snapshot["crop_outputs"] = nlohmann::json::object();
    }

    snapshot["schema_version"] = 2;
    snapshot["crop_outputs"][camera_serial] = crop_output_info;
    const nlohmann::json crop_descriptor =
        build_crop_recording_output_from_snapshot(camera_serial, crop_output_info);
    if (crop_descriptor.is_object()) {
        if (!snapshot.contains("recording_outputs") ||
            !snapshot["recording_outputs"].is_object()) {
            snapshot["recording_outputs"] = nlohmann::json::object();
        }
        if (!snapshot["recording_outputs"].contains(camera_serial) ||
            !snapshot["recording_outputs"][camera_serial].is_object()) {
            snapshot["recording_outputs"][camera_serial] = nlohmann::json::object();
        }
        snapshot["recording_outputs"][camera_serial]["crop"] = crop_descriptor;
        if (!snapshot.contains("encoders") || !snapshot["encoders"].is_object()) {
            snapshot["encoders"] = nlohmann::json::object();
        }
        if (!snapshot["encoders"].contains(camera_serial) ||
            !snapshot["encoders"][camera_serial].is_object()) {
            snapshot["encoders"][camera_serial] = nlohmann::json::object();
        }
        if (!snapshot["encoders"][camera_serial].contains("outputs") ||
            !snapshot["encoders"][camera_serial]["outputs"].is_object()) {
            snapshot["encoders"][camera_serial]["outputs"] = nlohmann::json::object();
        }
        snapshot["encoders"][camera_serial]["outputs"]["crop"] = crop_descriptor;
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    if (!write_json_atomic(snapshot_path, snapshot, std::filesystem::perms::unknown, false, "recording snapshot")) {
        return false;
    }

    return true;
}

bool update_recording_snapshot_spatial_calibration(const std::string& recording_folder,
                                                   const std::string& camera_serial,
                                                   const nlohmann::json& spatial_calibration) {
    if (recording_folder.empty() || camera_serial.empty()) {
        return false;
    }

    orange::spatial::CameraSpatialCalibration calibration;
    std::string validation_error;
    if (!orange::spatial::parse_camera_spatial_calibration_json(
            spatial_calibration,
            &calibration,
            &validation_error)) {
        std::cerr << "Failed to validate spatial calibration snapshot metadata for camera "
                  << camera_serial << ": " << validation_error << std::endl;
        return false;
    }

    const std::filesystem::path snapshot_path =
        std::filesystem::path(recording_folder) / "recording_snapshot.json";

    std::lock_guard<std::mutex> lock(recording_snapshot_mutex());

    nlohmann::json snapshot;
    if (!read_recording_snapshot_locked(snapshot_path, &snapshot)) {
        return false;
    }

    if (!orange::spatial::apply_camera_spatial_calibration_to_snapshot_json(
            &snapshot,
            camera_serial,
            calibration,
            &validation_error)) {
        std::cerr << "Failed to apply spatial calibration snapshot metadata for camera "
                  << camera_serial << ": " << validation_error << std::endl;
        return false;
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    if (!write_json_atomic(snapshot_path, snapshot, std::filesystem::perms::unknown, false, "recording snapshot")) {
        return false;
    }

    return true;
}

bool update_recording_snapshot_spatial_calibration_from_artifact(const std::string& recording_folder,
                                                                 const std::string& camera_serial,
                                                                 const std::string& artifact_dir,
                                                                 std::string* error_out) {
    if (error_out) {
        error_out->clear();
    }
    if (recording_folder.empty() || camera_serial.empty() || artifact_dir.empty()) {
        if (error_out) {
            *error_out = "recording folder, camera serial, and artifact directory are required";
        }
        return false;
    }

    orange::spatial::CameraSpatialCalibration calibration;
    if (!orange::spatial::load_camera_spatial_calibration_from_artifact_dir(
            artifact_dir,
            &calibration,
            error_out)) {
        return false;
    }

    const std::filesystem::path snapshot_path =
        std::filesystem::path(recording_folder) / "recording_snapshot.json";

    std::lock_guard<std::mutex> lock(recording_snapshot_mutex());

    nlohmann::json snapshot;
    if (!read_recording_snapshot_locked(snapshot_path, &snapshot)) {
        if (error_out && error_out->empty()) {
            *error_out = "failed to read recording snapshot";
        }
        return false;
    }

    if (!orange::spatial::apply_camera_spatial_calibration_to_snapshot_json(
            &snapshot,
            camera_serial,
            calibration,
            error_out)) {
        return false;
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::string write_error;
    if (!write_json_atomic(
            snapshot_path,
            snapshot,
            std::filesystem::perms::unknown,
            false,
            "recording snapshot",
            &write_error)) {
        if (error_out && error_out->empty()) {
            *error_out = write_error.empty() ? "failed to write recording snapshot" : write_error;
        }
        return false;
    }

    return true;
}

std::string build_model_id_from_path(const std::string& model_path)
{
    if (model_path.empty()) {
        return "unknown";
    }
    try {
        const std::filesystem::path path(model_path);
        const std::string stem = path.stem().string();
        if (!stem.empty()) {
            return stem;
        }
        const std::string filename = path.filename().string();
        if (!filename.empty()) {
            return filename;
        }
    } catch (...) {
    }
    return model_path;
}

bool read_camera_config_snapshot(const CameraParams& camera_params,
                                 std::string* config_contents,
                                 std::string* error_out) {
    if (config_contents) {
        config_contents->clear();
    }
    if (error_out) {
        error_out->clear();
    }
    if (camera_params.config_path.empty()) {
        if (error_out) {
            *error_out = "config path missing";
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(camera_config_mutex());
    std::string read_error;
    const std::string contents = read_file_to_string(camera_params.config_path, &read_error);
    if (contents.empty()) {
        if (error_out) {
            *error_out = read_error.empty() ? "config file empty" : read_error;
        }
        return false;
    }

    if (config_contents) {
        *config_contents = contents;
    }
    return true;
}

bool save_camera_json_config(const CameraParams& camera_params,
                             std::string* error_out) {
    if (error_out) {
        error_out->clear();
    }
    if (camera_params.config_path.empty()) {
        if (error_out) {
            *error_out = "config path missing";
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(camera_config_mutex());
    const std::filesystem::path config_path(camera_params.config_path);
    std::string mkdir_error;
    if (!ensure_directory_exists(config_path.parent_path().string(), &mkdir_error)) {
        if (error_out) {
            *error_out = mkdir_error.empty() ? "failed to create config folder" : mkdir_error;
        }
        return false;
    }

    std::string read_error;
    const std::string contents = read_file_to_string(camera_params.config_path, &read_error);
    nlohmann::json camera_config;
    if (contents.empty()) {
        if (read_error == "failed to open file" || read_error.empty()) {
            camera_config = nlohmann::json::object();
        } else {
            if (error_out) {
                *error_out = read_error;
            }
            return false;
        }
    } else {
        try {
            camera_config = nlohmann::json::parse(contents);
        } catch (const std::exception& ex) {
            if (error_out) {
                *error_out = std::string("failed to parse config json: ") + ex.what();
            }
            return false;
        }
    }
    if (!camera_config.is_object()) {
        if (error_out) {
            *error_out = "camera config must be a JSON object";
        }
        return false;
    }

    camera_config = build_camera_config_json_from_params(camera_params);

    std::error_code perms_error;
    const std::filesystem::file_status status = std::filesystem::status(config_path, perms_error);
    const bool set_perms = !perms_error;
    const std::filesystem::perms perms = set_perms ? status.permissions() : std::filesystem::perms::unknown;

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::string write_error;
    if (!write_json_atomic(config_path, camera_config, perms, set_perms, "camera config", &write_error)) {
        if (error_out) {
            if (!write_error.empty()) {
                *error_out = std::string("failed to write camera config: ") + camera_params.config_path +
                             " (" + write_error + ")";
            } else {
                *error_out = std::string("failed to write camera config: ") + camera_params.config_path;
            }
        }
        return false;
    }
    return true;
}

bool update_calibration_artifact_registry(const std::string& artifact_root_dir,
                                          const nlohmann::json& manifest,
                                          std::string* error_out) {
    if (artifact_root_dir.empty()) {
        if (error_out) {
            *error_out = "Calibration artifact root is empty.";
        }
        return false;
    }
    if (!manifest.is_object()) {
        if (error_out) {
            *error_out = "Calibration manifest is not a JSON object.";
        }
        return false;
    }
    if (!manifest.contains("artifact_id") || !manifest["artifact_id"].is_string()) {
        if (error_out) {
            *error_out = "Calibration manifest is missing artifact_id.";
        }
        return false;
    }
    if (!manifest.contains("artifact_schema_id") || !manifest["artifact_schema_id"].is_string()) {
        if (error_out) {
            *error_out = "Calibration manifest is missing artifact_schema_id.";
        }
        return false;
    }

    const std::string artifact_id = manifest["artifact_id"].get<std::string>();
    const std::string artifact_schema_id = manifest["artifact_schema_id"].get<std::string>();
    const std::filesystem::path registry_path = std::filesystem::path(artifact_root_dir) / "index.json";

    std::lock_guard<std::mutex> lock(calibration_registry_mutex());

    nlohmann::json registry;
    if (std::filesystem::exists(registry_path)) {
        std::string read_error;
        const std::string existing = read_file_to_string(registry_path.string(), &read_error);
        if (existing.empty()) {
            if (error_out) {
                *error_out = "Failed to read calibration registry: " +
                             (read_error.empty() ? registry_path.string() : read_error);
            }
            return false;
        }
        try {
            registry = nlohmann::json::parse(existing);
        } catch (const std::exception& ex) {
            if (error_out) {
                *error_out = std::string("Failed to parse calibration registry: ") + ex.what();
            }
            return false;
        }
    }

    if (!registry.is_object()) {
        registry = nlohmann::json::object();
    }
    registry["schema_id"] = kCalibrationRegistrySchemaId;
    registry["schema_version"] = kCalibrationRegistrySchemaVersion;
    registry["artifact_root"] = artifact_root_dir;
    registry["updated_utc"] = get_current_utc_timestamp();
    if (!registry.contains("artifacts_by_id") || !registry["artifacts_by_id"].is_object()) {
        registry["artifacts_by_id"] = nlohmann::json::object();
    }
    if (!registry.contains("latest_by_schema") || !registry["latest_by_schema"].is_object()) {
        registry["latest_by_schema"] = nlohmann::json::object();
    }

    std::string fingerprint;
    if (manifest.contains("calibration_ref") && manifest["calibration_ref"].is_object()) {
        fingerprint = manifest["calibration_ref"].value("fingerprint", "");
    }

    nlohmann::json entry;
    entry["artifact_id"] = artifact_id;
    entry["artifact_schema_id"] = artifact_schema_id;
    entry["artifact_schema_version"] = manifest.value("artifact_schema_version", 0);
    entry["created_utc"] = manifest.value("created_utc", "");
    entry["fingerprint"] = fingerprint;
    entry["relative_manifest_path"] =
        (std::filesystem::path(artifact_id) / "manifest.json").generic_string();
    if (manifest.contains("producer")) {
        entry["producer"] = manifest["producer"];
    }
    if (manifest.contains("compatibility")) {
        entry["compatibility"] = manifest["compatibility"];
    }
    if (manifest.contains("summary")) {
        entry["summary"] = manifest["summary"];
    }

    registry["artifacts_by_id"][artifact_id] = entry;
    registry["latest_by_schema"][artifact_schema_id] = artifact_id;
    registry["artifact_count"] = registry["artifacts_by_id"].size();

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    if (!write_json_atomic(registry_path, registry, std::filesystem::perms::unknown, false,
                           "calibration registry")) {
        if (error_out) {
            *error_out = "Failed to write calibration registry: " + registry_path.string();
        }
        return false;
    }

    return true;
}

void allocate_camera_frame_buffers(CameraEmergent* ecams, CameraParams* cameras_params, int evt_buffer_size, int num_cameras)
{
    // ... (implementation from project.h)
    for (int i = 0; i < num_cameras; i++)
    {
        camera_open_stream(&ecams[i].camera, &cameras_params[i], "project_allocate_camera_frame_buffers");
        ecams[i].evt_frame = new Emergent::CEmergentFrame[evt_buffer_size];
        ecams[i].evt_frame_count = evt_buffer_size;
        allocate_frame_buffer(&ecams[i].camera, ecams[i].evt_frame, &cameras_params[i], evt_buffer_size);
        if (cameras_params[i].need_reorder && cameras_params[i].gpu_direct)
        {
            allocate_frame_reorder_buffer(&ecams[i].camera, &ecams[i].frame_reorder, &cameras_params[i]);
        }
    }
}

void client_send_bringup_message(EnetContext* enet_context, flatbuffers::FlatBufferBuilder* builder, ENetPeer *server_connection, int cam_count, FetchGame::ManagerState server_state)
{
    // ... (implementation from project.h)
    char hostname[100];
    gethostname(hostname, 100);
    builder->Clear();
    auto server_name = builder->CreateString(hostname);
    auto message_fb = FetchGame::Createbring_up_message(*builder, server_name, cam_count);
    FetchGame::ServerBuilder server_builder(*builder);
    server_builder.add_signal_type(FetchGame::SignalType_ClientBringup);
    server_builder.add_server_mesg(message_fb);
    server_builder.add_server_state(server_state);
    auto server_fb = server_builder.Finish();
    builder->Finish(server_fb);
    uint8_t *server_buffer = builder->GetBufferPointer();
    int server_buf_size = builder->GetSize();
    ENetPacket* enet_packet = enet_packet_create(server_buffer, server_buf_size, 0);
    enet_peer_send(server_connection, 0, enet_packet);
}

void client_send_state_update_message(EnetContext* enet_context, flatbuffers::FlatBufferBuilder* builder, ENetPeer *server_connection, FetchGame::ManagerState server_state)
{
    // ... (implementation from project.h)
    builder->Clear();
    FetchGame::ServerBuilder server_builder(*builder);
    server_builder.add_signal_type(FetchGame::SignalType_ClientStateUpdate);
    server_builder.add_server_state(server_state);
    auto server_fb = server_builder.Finish();
    builder->Finish(server_fb);
    uint8_t *server_buffer = builder->GetBufferPointer();
    int server_buf_size = builder->GetSize();
    ENetPacket* enet_packet = enet_packet_create(server_buffer, server_buf_size, 0);
    enet_peer_send(server_connection, 0, enet_packet);
}

void host_broadcast_open_cameras(flatbuffers::FlatBufferBuilder* builder, EnetContext* server, std::string config_file_name)
{
    // ... (implementation from project.h)
    builder->Clear();
    auto config_message = builder->CreateString(config_file_name);
    FetchGame::ServerBuilder server_builder(*builder);
    server_builder.add_config_folder(config_message);
    server_builder.add_control(FetchGame::ServerControl_OPENCAMERA);
    auto my_server = server_builder.Finish();
    builder->Finish(my_server);
    uint8_t *server_buffer = builder->GetBufferPointer();
    int server_buf_size = builder->GetSize();
    ENetPacket* enet_packet = enet_packet_create(server_buffer, server_buf_size, 0);
    enet_host_broadcast(server->m_pNetwork, 0, enet_packet);
}

void host_broadcast_start_threads(flatbuffers::FlatBufferBuilder* builder, EnetContext* server, std::string record_folder_name, std::string encoder_basic_setup)
{
    // ... (implementation from project.h)
    builder->Clear();
    auto record_folder_message = builder->CreateString(record_folder_name);
    auto encoder_setup_message = builder->CreateString(encoder_basic_setup);
    FetchGame::ServerBuilder server_builder(*builder);
    server_builder.add_record_folder(record_folder_message);
    server_builder.add_encoder_setup(encoder_setup_message);
    server_builder.add_control(FetchGame::ServerControl_STARTTHREAD);
    auto my_server = server_builder.Finish();
    builder->Finish(my_server);
    uint8_t *server_buffer = builder->GetBufferPointer();
    int server_buf_size = builder->GetSize();
    ENetPacket* enet_packet = enet_packet_create(server_buffer, server_buf_size, 0);
    enet_host_broadcast(server->m_pNetwork, 0, enet_packet);
}

void host_broadcast_set_start_ptp(flatbuffers::FlatBufferBuilder* builder, EnetContext* server, unsigned long long ptp_global_time)
{
    // ... (implementation from project.h)
    builder->Clear();
    FetchGame::ServerBuilder server_builder(*builder);
    server_builder.add_control(FetchGame::ServerControl_STARTRECORDING);
    server_builder.add_ptp_global_time(ptp_global_time);
    auto my_server = server_builder.Finish();
    builder->Finish(my_server);
    uint8_t *server_buffer = builder->GetBufferPointer();
    int server_buf_size = builder->GetSize();
    ENetPacket* enet_packet = enet_packet_create(server_buffer, server_buf_size, 0);
    enet_host_broadcast(server->m_pNetwork, 0, enet_packet);
}
