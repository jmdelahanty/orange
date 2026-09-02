#include "spatial_roi_recorder_scheduling.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace recording = orange::spatial_roi::recording;

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void test_strict_cpu_list_parser()
{
    recording::SpatialRoiRecorderCpuList parsed;
    std::string error;
    require(recording::parse_spatial_roi_recorder_cpu_list(
                "35-37,3-5,39", &parsed, &error),
            error);
    require(parsed.canonical == "3-5,35-37,39",
            "CPU list was not sorted and canonicalized");
    require(parsed.cpus.size() == 7U && parsed.cpus.front() == 3 &&
                parsed.cpus.back() == 39,
            "CPU list did not preserve the exact set");

    for (const std::string value : {
             "", ",1", "1,", "1,,2", "1-", "-1", "2-1", "1-2-3",
             "1,1", "1-3,2", "1 2", "cpu1", "1024"}) {
        require(!recording::parse_spatial_roi_recorder_cpu_list(
                    value, &parsed, &error),
                "invalid CPU list was accepted: " + value);
        require(!error.empty(), "invalid CPU list had no failure reason");
    }
}

void test_inherited_and_explicit_observation()
{
    recording::SpatialRoiRecorderSchedulingSnapshot inherited;
    std::string error;
    require(recording::initialize_spatial_roi_recorder_scheduling(
                "", "", &inherited, &error),
            error);
    require(!inherited.configured && !inherited.affinity_applied &&
                !inherited.effective_cpus.empty() &&
                !inherited.effective_cpu_list.empty() &&
                !inherited.scheduler_policy.empty(),
            "inherited scheduling observation is incomplete");

    // Applying the already-effective set exercises sched_setaffinity and the
    // exact readback gate without changing the test process's placement.
    recording::SpatialRoiRecorderSchedulingSnapshot explicit_snapshot;
    require(recording::initialize_spatial_roi_recorder_scheduling(
                inherited.effective_cpu_list,
                "spatial_roi_recorder_scheduling_tests",
                &explicit_snapshot,
                &error),
            error);
    require(explicit_snapshot.configured &&
                explicit_snapshot.affinity_syscall_succeeded &&
                explicit_snapshot.affinity_applied &&
                explicit_snapshot.effective_mask_verified &&
                explicit_snapshot.effective_cpu_list ==
                    inherited.effective_cpu_list,
            "explicit scheduling application did not read back exactly");

    recording::SpatialRoiRecorderSchedulingSnapshot child_snapshot;
    std::string child_error;
    std::thread child([&]() {
        (void)recording::initialize_spatial_roi_recorder_scheduling(
            "", "", &child_snapshot, &child_error);
    });
    child.join();
    require(child_error.empty() &&
                child_snapshot.effective_cpu_list ==
                    explicit_snapshot.effective_cpu_list &&
                child_snapshot.scheduler_policy ==
                    explicit_snapshot.scheduler_policy,
            "thread created after affinity application did not inherit the mask/policy");

    const auto payload =
        recording::spatial_roi_recorder_scheduling_to_json(explicit_snapshot);
    require(payload.at("schema_id") ==
                "orange.spatial_roi_recorder.scheduling" &&
                payload.at("schema_version") == 1 &&
                payload.at("scope") ==
                    "camera_recorder_inherited_thread_mask" &&
                payload.at("scheduler").at("policy") ==
                    explicit_snapshot.scheduler_policy &&
                payload.dump().size() < 8192U,
            "scheduling lifecycle payload is incomplete or unexpectedly large");
}

void test_configured_source_is_mandatory()
{
    recording::SpatialRoiRecorderSchedulingSnapshot snapshot;
    std::string error;
    require(!recording::initialize_spatial_roi_recorder_scheduling(
                "0", "", &snapshot, &error) &&
                error.find("authority source") != std::string::npos,
            "configured affinity without an authority source was accepted");
    require(!recording::initialize_spatial_roi_recorder_scheduling(
                "", "unexpected_source", &snapshot, &error) &&
                error.find("supplied together") != std::string::npos,
            "authority source without configured affinity was accepted");
}

}  // namespace

int main()
{
    try {
        test_strict_cpu_list_parser();
        test_inherited_and_explicit_observation();
        test_configured_source_is_mandatory();
        std::cout << "spatial ROI recorder scheduling tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "spatial ROI recorder scheduling tests failed: "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
