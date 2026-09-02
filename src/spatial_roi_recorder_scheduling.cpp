#include "spatial_roi_recorder_scheduling.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <pthread.h>
#include <sched.h>
#include <set>
#include <sstream>
#include <utility>

namespace orange::spatial_roi::recording {
namespace {

constexpr std::size_t kMaxCpuListBytes = 256U;

bool fail(std::string* error_out, std::string message)
{
    if (error_out != nullptr) *error_out = std::move(message);
    return false;
}

bool parse_decimal(std::string_view value, int* parsed_out)
{
    if (value.empty() || parsed_out == nullptr) return false;
    unsigned long long parsed = 0;
    for (const char byte : value) {
        if (byte < '0' || byte > '9') return false;
        const unsigned digit = static_cast<unsigned>(byte - '0');
        if (parsed > (std::numeric_limits<unsigned long long>::max() - digit) /
                         10ULL) {
            return false;
        }
        parsed = parsed * 10ULL + digit;
    }
    if (parsed >= static_cast<unsigned long long>(CPU_SETSIZE)) return false;
    *parsed_out = static_cast<int>(parsed);
    return true;
}

std::string canonical_cpu_list(const std::vector<int>& cpus)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < cpus.size();) {
        const int first = cpus[index];
        int last = first;
        std::size_t next = index + 1U;
        while (next < cpus.size() && cpus[next] == last + 1) {
            last = cpus[next++];
        }
        if (index != 0U) output << ',';
        output << first;
        if (last != first) output << '-' << last;
        index = next;
    }
    return output.str();
}

std::vector<int> cpuset_to_vector(const cpu_set_t& set)
{
    std::vector<int> result;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &set)) result.push_back(cpu);
    }
    return result;
}

std::string policy_name(const int policy)
{
    switch (policy) {
        case SCHED_OTHER: return "SCHED_OTHER";
        case SCHED_FIFO: return "SCHED_FIFO";
        case SCHED_RR: return "SCHED_RR";
#ifdef SCHED_BATCH
        case SCHED_BATCH: return "SCHED_BATCH";
#endif
#ifdef SCHED_IDLE
        case SCHED_IDLE: return "SCHED_IDLE";
#endif
#ifdef SCHED_DEADLINE
        case SCHED_DEADLINE: return "SCHED_DEADLINE";
#endif
        default: return "UNKNOWN(" + std::to_string(policy) + ")";
    }
}

void observe_kernel_isolation(SpatialRoiRecorderSchedulingSnapshot* snapshot)
{
    if (snapshot == nullptr) return;
    std::ifstream input("/sys/devices/system/cpu/isolated");
    if (!input) {
        snapshot->kernel_isolation_observation_error =
            "could not open /sys/devices/system/cpu/isolated";
        return;
    }
    std::string value;
    if (!std::getline(input, value) && !input.eof()) {
        snapshot->kernel_isolation_observation_error =
            "could not read /sys/devices/system/cpu/isolated";
        return;
    }
    if (!value.empty() && value.back() == '\r') value.pop_back();
    if (value.empty()) {
        snapshot->kernel_isolation_observed = true;
        return;
    }
    SpatialRoiRecorderCpuList parsed;
    std::string parse_error;
    if (!parse_spatial_roi_recorder_cpu_list(value, &parsed, &parse_error)) {
        snapshot->kernel_isolation_observation_error =
            "kernel isolated CPU list is invalid: " + parse_error;
        return;
    }
    snapshot->kernel_isolation_observed = true;
    snapshot->kernel_isolated_cpus = std::move(parsed.cpus);
    snapshot->kernel_isolated_cpu_list = std::move(parsed.canonical);
}

bool observe_current_scheduling(SpatialRoiRecorderSchedulingSnapshot* snapshot,
                                std::string* error_out)
{
    if (snapshot == nullptr) return fail(error_out, "scheduling snapshot is null");
    cpu_set_t effective;
    CPU_ZERO(&effective);
    if (::sched_getaffinity(0, sizeof(effective), &effective) != 0) {
        return fail(error_out,
                    std::string("sched_getaffinity failed: ") +
                        std::strerror(errno));
    }
    snapshot->effective_cpus = cpuset_to_vector(effective);
    snapshot->effective_cpu_list =
        canonical_cpu_list(snapshot->effective_cpus);
    if (snapshot->effective_cpus.empty()) {
        return fail(error_out, "effective recorder CPU affinity is empty");
    }

    int policy = 0;
    sched_param parameters{};
    const int scheduling_error =
        ::pthread_getschedparam(::pthread_self(), &policy, &parameters);
    if (scheduling_error != 0) {
        return fail(error_out,
                    std::string("pthread_getschedparam failed: ") +
                        std::strerror(scheduling_error));
    }
    snapshot->scheduler_policy = policy_name(policy);
    snapshot->scheduler_priority = parameters.sched_priority;
    observe_kernel_isolation(snapshot);
    return true;
}

}  // namespace

bool parse_spatial_roi_recorder_cpu_list(
    const std::string_view value,
    SpatialRoiRecorderCpuList* parsed_out,
    std::string* error_out)
{
    if (error_out != nullptr) error_out->clear();
    if (parsed_out == nullptr) return fail(error_out, "CPU-list destination is null");
    *parsed_out = SpatialRoiRecorderCpuList{};
    if (value.empty() || value.size() > kMaxCpuListBytes) {
        return fail(error_out, "CPU list must contain 1-256 bytes");
    }

    std::set<int> unique;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t comma = value.find(',', begin);
        const std::size_t end =
            comma == std::string_view::npos ? value.size() : comma;
        const std::string_view item = value.substr(begin, end - begin);
        if (item.empty()) return fail(error_out, "CPU list contains an empty item");
        const std::size_t dash = item.find('-');
        if (dash != std::string_view::npos &&
            item.find('-', dash + 1U) != std::string_view::npos) {
            return fail(error_out, "CPU-list range contains more than one dash");
        }

        int first = -1;
        int last = -1;
        if (dash == std::string_view::npos) {
            if (!parse_decimal(item, &first)) {
                return fail(error_out, "CPU list contains an invalid CPU index");
            }
            last = first;
        } else {
            if (!parse_decimal(item.substr(0, dash), &first) ||
                !parse_decimal(item.substr(dash + 1U), &last) || last < first) {
                return fail(error_out, "CPU list contains an invalid range");
            }
        }
        for (int cpu = first; cpu <= last; ++cpu) {
            if (!unique.insert(cpu).second) {
                return fail(error_out, "CPU list contains a duplicate or overlapping CPU");
            }
        }
        if (comma == std::string_view::npos) break;
        if (comma + 1U == value.size()) {
            return fail(error_out, "CPU list contains an empty item");
        }
        begin = comma + 1U;
    }

    parsed_out->cpus.assign(unique.begin(), unique.end());
    parsed_out->canonical = canonical_cpu_list(parsed_out->cpus);
    return true;
}

bool initialize_spatial_roi_recorder_scheduling(
    const std::string& requested_cpu_list,
    const std::string& configuration_source,
    SpatialRoiRecorderSchedulingSnapshot* snapshot_out,
    std::string* error_out)
{
    if (error_out != nullptr) error_out->clear();
    if (snapshot_out == nullptr) {
        return fail(error_out, "scheduling snapshot destination is null");
    }
    *snapshot_out = SpatialRoiRecorderSchedulingSnapshot{};
    if (requested_cpu_list.empty() != configuration_source.empty()) {
        snapshot_out->error =
            "requested CPU affinity and its authority source must be supplied together";
        return fail(error_out, snapshot_out->error);
    }
    snapshot_out->configured = !requested_cpu_list.empty();
    snapshot_out->configuration_source = snapshot_out->configured
                                                  ? configuration_source
                                                  : "inherited_process_affinity";
    snapshot_out->requested_cpu_list = requested_cpu_list;

    if (snapshot_out->configured) {
        SpatialRoiRecorderCpuList parsed;
        std::string parse_error;
        if (configuration_source.empty()) {
            parse_error = "configured CPU affinity has no authority source";
        } else if (!parse_spatial_roi_recorder_cpu_list(
                       requested_cpu_list, &parsed, &parse_error)) {
            // parse_error already describes the exact bounded failure.
        }
        if (!parse_error.empty()) {
            snapshot_out->error = parse_error;
            (void)observe_current_scheduling(snapshot_out, nullptr);
            return fail(error_out, parse_error);
        }
        snapshot_out->canonical_requested_cpu_list = parsed.canonical;

        cpu_set_t requested;
        CPU_ZERO(&requested);
        for (const int cpu : parsed.cpus) CPU_SET(cpu, &requested);
        if (::sched_setaffinity(0, sizeof(requested), &requested) != 0) {
            const std::string message =
                std::string("sched_setaffinity failed: ") + std::strerror(errno);
            snapshot_out->error = message;
            (void)observe_current_scheduling(snapshot_out, nullptr);
            return fail(error_out, message);
        }
        snapshot_out->affinity_syscall_succeeded = true;
    }

    std::string observation_error;
    if (!observe_current_scheduling(snapshot_out, &observation_error)) {
        snapshot_out->error = observation_error;
        return fail(error_out, observation_error);
    }
    if (snapshot_out->configured &&
        snapshot_out->effective_cpu_list !=
            snapshot_out->canonical_requested_cpu_list) {
        const std::string message =
            "effective recorder CPU affinity does not exactly match the requested mask";
        snapshot_out->error = message;
        return fail(error_out, message);
    }
    if (snapshot_out->configured) {
        snapshot_out->effective_mask_verified = true;
        snapshot_out->affinity_applied = true;
    }
    return true;
}

nlohmann::json spatial_roi_recorder_scheduling_to_json(
    const SpatialRoiRecorderSchedulingSnapshot& snapshot)
{
    return {
        {"schema_id", "orange.spatial_roi_recorder.scheduling"},
        {"schema_version", 1},
        {"scope", "camera_recorder_inherited_thread_mask"},
        {"configuration_mode", snapshot.configured ? "explicit" : "inherited"},
        {"configuration_source", snapshot.configuration_source},
        {"requested_cpu_list",
         snapshot.configured ? nlohmann::json(snapshot.requested_cpu_list)
                             : nlohmann::json(nullptr)},
        {"canonical_requested_cpu_list",
         snapshot.configured
             ? nlohmann::json(snapshot.canonical_requested_cpu_list)
             : nlohmann::json(nullptr)},
        {"affinity_syscall_succeeded", snapshot.affinity_syscall_succeeded},
        {"affinity_applied", snapshot.affinity_applied},
        {"effective_mask_verified", snapshot.effective_mask_verified},
        {"effective_cpu_list", snapshot.effective_cpu_list},
        {"effective_cpus", snapshot.effective_cpus},
        {"kernel_isolated_cpu_list",
         snapshot.kernel_isolated_cpu_list.empty()
             ? nlohmann::json(nullptr)
             : nlohmann::json(snapshot.kernel_isolated_cpu_list)},
        {"kernel_isolated_cpus", snapshot.kernel_isolated_cpus},
        {"kernel_isolation_observed", snapshot.kernel_isolation_observed},
        {"kernel_isolation_observation_error",
         snapshot.kernel_isolation_observation_error.empty()
             ? nlohmann::json(nullptr)
             : nlohmann::json(snapshot.kernel_isolation_observation_error)},
        {"scheduler",
         {{"policy", snapshot.scheduler_policy},
          {"priority", snapshot.scheduler_priority}}},
        {"application_phase", "before_recorder_authority_and_worker_initialization"},
        {"thread_inheritance",
         "recorder_threads_created_after_this_snapshot_inherit_the_effective_mask"},
        {"error", snapshot.error.empty() ? nlohmann::json(nullptr)
                                         : nlohmann::json(snapshot.error)},
    };
}

}  // namespace orange::spatial_roi::recording
