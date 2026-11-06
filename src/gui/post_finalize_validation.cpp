#include "gui/post_finalize_validation.h"

#include "fsuid_guard.h"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace orange::gui {
namespace {

constexpr const char* kValidatorScript = "validate_gui_ptp_recording.py";
constexpr const char* kPython = "/usr/bin/python3";
// Far above the ~1 minute a 30-minute recording takes; a run past this is hung.
constexpr auto kTimeout = std::chrono::minutes(20);
constexpr std::size_t kFailureLinesShown = 12;

struct Run {
    pid_t pid = -1;
    std::string recording_folder;
    std::string log_path;
    std::chrono::steady_clock::time_point started{};
    PostFinalizeValidationView view;
};

Run& run_state()
{
    static Run run;
    return run;
}

std::filesystem::path repo_root()
{
    std::array<char, 4096> buffer{};
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    std::error_code cwd_ec;
    std::vector<std::filesystem::path> candidates = {std::filesystem::current_path(cwd_ec)};
    if (length > 0) {
        // <tree>/targets/release/orange
        candidates.push_back(std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)))
                                 .parent_path()
                                 .parent_path()
                                 .parent_path());
    }
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate / "scripts" / kValidatorScript, ec)) {
            return std::filesystem::absolute(candidate, ec);
        }
    }
    return {};
}

// "0-5,7,9-11" -> the listed CPUs.
std::vector<int> parse_cpu_list(const std::string& text)
{
    std::vector<int> cpus;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, ',')) {
        if (part.empty() || part == "\n") {
            continue;
        }
        const std::size_t dash = part.find('-');
        try {
            const int first = std::stoi(part.substr(0, dash));
            const int last = dash == std::string::npos ? first : std::stoi(part.substr(dash + 1));
            for (int cpu = first; cpu <= last; ++cpu) {
                cpus.push_back(cpu);
            }
        } catch (const std::exception&) {
            return {};
        }
    }
    return cpus;
}

std::string read_first_line(const char* path)
{
    std::ifstream input(path);
    std::string line;
    std::getline(input, line);
    return line;
}

// Online CPUs that are not isolated: where housekeeping belongs.
cpu_set_t housekeeping_cpus(int* count_out)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    const std::vector<int> online = parse_cpu_list(read_first_line("/sys/devices/system/cpu/online"));
    const std::vector<int> isolated = parse_cpu_list(read_first_line("/sys/devices/system/cpu/isolated"));
    int count = 0;
    for (const int cpu : online) {
        if (cpu >= 0 && cpu < CPU_SETSIZE &&
            std::find(isolated.begin(), isolated.end(), cpu) == isolated.end()) {
            CPU_SET(cpu, &set);
            ++count;
        }
    }
    *count_out = count;
    return set;
}

std::string tail_of_file(const std::string& path, std::size_t max_bytes)
{
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    std::string text = contents.str();
    if (text.size() > max_bytes) {
        text = text.substr(text.size() - max_bytes);
    }
    return text;
}

void finish(Run& run, const std::string& status, const std::string& detail)
{
    run.view.status = status;
    run.view.detail = detail;
    run.view.elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - run.started).count();
    run.pid = -1;
}

void print_result(const Run& run)
{
    const PostFinalizeValidationView& v = run.view;
    std::cout << "[GUI][validation] " << v.recording_name << ": ";
    if (v.status == "pass") {
        std::cout << "PASS (" << v.warnings << " warnings)";
    } else if (v.status == "fail") {
        std::cout << "FAIL (" << v.failures << " failures, " << v.warnings << " warnings)";
    } else {
        std::cout << v.status << (v.detail.empty() ? "" : ": " + v.detail);
    }
    std::cout << " in " << static_cast<int>(v.elapsed_seconds) << " s; " << v.json_path << std::endl;
    for (const std::string& line : v.failure_lines) {
        std::cout << "[GUI][validation]   FAIL " << line << std::endl;
    }
}

void reap_finished(Run& run, int wait_status)
{
    run.view.failure_lines.clear();
    if (!WIFEXITED(wait_status)) {
        finish(run, "error",
               "validator ended by signal " + std::to_string(WTERMSIG(wait_status)) + "; log " + run.log_path);
        return;
    }
    const int code = WEXITSTATUS(wait_status);
    nlohmann::json result;
    try {
        std::ifstream input(run.view.json_path);
        input >> result;
    } catch (const std::exception&) {
        finish(run, "error",
               "validator exit " + std::to_string(code) + " without a result: " + tail_of_file(run.log_path, 400));
        return;
    }
    const auto failures = result.value("failures", nlohmann::json::array());
    const auto warnings = result.value("warnings", nlohmann::json::array());
    run.view.failures = static_cast<int>(failures.size());
    run.view.warnings = static_cast<int>(warnings.size());
    for (const auto& line : failures) {
        if (run.view.failure_lines.size() >= kFailureLinesShown) {
            run.view.failure_lines.push_back("... and " +
                                             std::to_string(failures.size() - kFailureLinesShown) + " more");
            break;
        }
        run.view.failure_lines.push_back(line.is_string() ? line.get<std::string>() : line.dump());
    }
    const std::string status = result.value("status", "");
    if ((status == "pass" && code == 0) || (status == "fail" && code == 1)) {
        finish(run, status, "");
    } else {
        finish(run, "error", "validator exit " + std::to_string(code) + " with status '" + status + "'");
    }
}

void stop_child(Run& run)
{
    if (run.pid <= 0) {
        return;
    }
    // The child leads its own process group (setsid), so this reaches anything it started.
    ::kill(-run.pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        int status = 0;
        if (::waitpid(run.pid, &status, WNOHANG) == run.pid) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::kill(-run.pid, SIGKILL);
    int status = 0;
    ::waitpid(run.pid, &status, 0);
}

}  // namespace

void StartPostFinalizeValidation(const std::string& recording_folder)
{
    Run& run = run_state();
    if (run.pid > 0) {
        CancelPostFinalizeValidation("a newer recording finalized");
    }
    run = Run{};
    run.recording_folder = recording_folder;
    run.started = std::chrono::steady_clock::now();
    run.view.recording_name = std::filesystem::path(recording_folder).filename().string();

    const std::filesystem::path root = repo_root();
    if (root.empty()) {
        finish(run, "error", std::string("scripts/") + kValidatorScript + " not found next to the binary");
        print_result(run);
        return;
    }

    // Who the validator runs as, and where its result goes.
    uid_t uid = ::getuid();
    gid_t gid = ::getgid();
    const bool drop_root = sudo_invoker_ids(&uid, &gid);
    std::vector<char> passwd_buffer(16384);
    struct passwd pw {};
    struct passwd* found = nullptr;
    if (::getpwuid_r(uid, &pw, passwd_buffer.data(), passwd_buffer.size(), &found) != 0 || !found) {
        finish(run, "error", "no passwd entry for uid " + std::to_string(uid));
        print_result(run);
        return;
    }
    const std::string home = pw.pw_dir;
    std::vector<gid_t> groups(64);
    int group_count = static_cast<int>(groups.size());
    if (::getgrouplist(pw.pw_name, gid, groups.data(), &group_count) < 0) {
        groups.resize(static_cast<std::size_t>(group_count));
        ::getgrouplist(pw.pw_name, gid, groups.data(), &group_count);
    }
    groups.resize(static_cast<std::size_t>(std::max(group_count, 0)));

    const std::filesystem::path out_dir = std::filesystem::path(home) / "orange_data" / "recording_validation";
    run.view.json_path = (out_dir / (run.view.recording_name + ".json")).string();
    run.log_path = (out_dir / (run.view.recording_name + ".log")).string();
    int log_fd = -1;
    {
        orange::ScopedFsuid fsuid_guard;
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        // A stale result from an earlier run must not be read as this one's.
        std::filesystem::remove(run.view.json_path, ec);
        log_fd = ::open(run.log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
    }
    if (log_fd < 0) {
        finish(run, "error", "cannot open " + run.log_path + ": " + std::strerror(errno));
        print_result(run);
        return;
    }
    const int null_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);

    int cpu_count = 0;
    const cpu_set_t cpus = housekeeping_cpus(&cpu_count);

    // Everything the child needs is built before fork: after fork it may only
    // make async-signal-safe calls (the GUI is multi-threaded).
    const std::string script = (root / "scripts" / kValidatorScript).string();
    const std::string root_text = root.string();
    std::vector<std::string> arguments = {
        kPython, "-E", "-s", script, recording_folder,
        "--require-nic-port-counters", "--json-out", run.view.json_path,
    };
    std::vector<std::string> environment = {
        "PATH=/usr/bin:/bin",
        "HOME=" + home,
        "LANG=C.UTF-8",
        "PYTHONDONTWRITEBYTECODE=1",
    };
    std::vector<char*> argv;
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (std::string& entry : environment) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        if (null_fd >= 0) {
            ::dup2(null_fd, STDIN_FILENO);
        }
        ::dup2(log_fd, STDOUT_FILENO);
        ::dup2(log_fd, STDERR_FILENO);
        // The GUI's sockets, GPU handles and lock files stay with the GUI.
        ::syscall(SYS_close_range, 3U, ~0U, 0U);
        if (cpu_count > 0) {
            ::sched_setaffinity(0, sizeof(cpus), &cpus);
        }
        ::setpriority(PRIO_PROCESS, 0, 19);
        constexpr int kIoprioWhoProcess = 1;
        constexpr int kIoprioClassIdle = 3;
        ::syscall(SYS_ioprio_set, kIoprioWhoProcess, 0, kIoprioClassIdle << 13);
        if (drop_root) {
            if (::setgroups(groups.size(), groups.data()) != 0 || ::setresgid(gid, gid, gid) != 0 ||
                ::setresuid(uid, uid, uid) != 0) {
                ::_exit(126);
            }
        }
        if (::chdir(root_text.c_str()) != 0) {
            ::_exit(126);
        }
        ::execve(kPython, argv.data(), envp.data());
        ::_exit(127);
    }
    ::close(log_fd);
    if (null_fd >= 0) {
        ::close(null_fd);
    }
    if (pid < 0) {
        finish(run, "error", std::string("fork: ") + std::strerror(errno));
        print_result(run);
        return;
    }
    run.pid = pid;
    run.view.status = "running";
    std::cout << "[GUI][validation] " << run.view.recording_name << ": validating in the background (pid " << pid
              << ", " << cpu_count << " housekeeping cores, nice 19); result " << run.view.json_path << std::endl;
}

void PollPostFinalizeValidation()
{
    Run& run = run_state();
    if (run.pid <= 0) {
        return;
    }
    int status = 0;
    const pid_t reaped = ::waitpid(run.pid, &status, WNOHANG);
    if (reaped == run.pid) {
        reap_finished(run, status);
        print_result(run);
        return;
    }
    if (reaped < 0 && errno == ECHILD) {
        finish(run, "error", "validator process vanished");
        print_result(run);
        return;
    }
    if (std::chrono::steady_clock::now() - run.started > kTimeout) {
        stop_child(run);
        finish(run, "error", "validator ran past 20 minutes and was stopped; log " + run.log_path);
        print_result(run);
    }
}

void CancelPostFinalizeValidation(const char* reason)
{
    Run& run = run_state();
    if (run.pid <= 0) {
        return;
    }
    stop_child(run);
    finish(run, "cancelled", std::string(reason ? reason : "cancelled") + "; rerun: python3 scripts/" +
                                 kValidatorScript + " " + run.recording_folder + " --require-nic-port-counters");
    print_result(run);
}

PostFinalizeValidationView PostFinalizeValidationStatus()
{
    PostFinalizeValidationView view = run_state().view;
    if (view.status == "running") {
        view.elapsed_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - run_state().started).count();
    }
    return view;
}

}  // namespace orange::gui
