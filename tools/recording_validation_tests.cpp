#include "recording_validation.h"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Lookup = RecordingValidationGpuPathLookup;

RecordingValidationCameraInput make_split_gop_camera(const std::string& serial,
                                                     const bool record_enabled,
                                                     const int source_gpu_id,
                                                     const std::vector<int>& encoder_gpu_ids,
                                                     const std::string& preferred_topology = "PIX",
                                                     const bool require_peer_access = true)
{
    RecordingValidationCameraInput camera;
    camera.camera_serial = serial;
    camera.record_enabled = record_enabled;
    camera.source_gpu_id = source_gpu_id;
    camera.strategy.requested_mode = "split_gop";
    camera.strategy.mode = "split_gop";
    camera.strategy.split_gop.enabled = true;
    camera.strategy.split_gop.placement = "multi_gpu";
    camera.strategy.split_gop.source_encoder_policy = "hybrid_split";
    camera.strategy.split_gop.transfer_mode = "raw";
    camera.strategy.split_gop.encoder_gpu_ids = encoder_gpu_ids;
    camera.constraints.preferred_topology_class = preferred_topology;
    camera.constraints.require_peer_access = require_peer_access;
    return camera;
}

Lookup make_lookup(const std::string& topology_class, const bool can_access_peer)
{
    return [topology_class, can_access_peer](const int source_gpu_id, const int helper_gpu_id) {
        RecordingValidationGpuPathInfo info;
        info.source_gpu_id = source_gpu_id;
        info.helper_gpu_id = helper_gpu_id;
        info.topology_class = topology_class;
        info.can_access_peer = can_access_peer;
        info.can_access_peer_known = true;
        return info;
    };
}

RecordingValidationCameraInput make_single_session_camera(const std::string& serial,
                                                          const bool record_enabled,
                                                          const int source_gpu_id)
{
    RecordingValidationCameraInput camera;
    camera.camera_serial = serial;
    camera.record_enabled = record_enabled;
    camera.source_gpu_id = source_gpu_id;
    camera.strategy.requested_mode = "single_session";
    camera.strategy.mode = "single_session";
    camera.strategy.split_gop.enabled = false;
    return camera;
}

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_valid_pix_pair_passes()
{
    const std::vector<RecordingValidationCameraInput> cameras = {
        make_split_gop_camera("2010096", true, 5, {5, 6})
    };
    const auto summaries = validate_recording_configuration(cameras, make_lookup("PIX", true));
    require(summaries.size() == 1, "expected one summary");
    require(summaries[0].valid(), "PIX peer-capable pair should validate");
    require(summaries[0].helper_gpu_ids.size() == 1 && summaries[0].helper_gpu_ids[0] == 6,
            "helper GPU should resolve to non-source GPU 6");
}

void test_topology_mismatch_fails()
{
    const std::vector<RecordingValidationCameraInput> cameras = {
        make_split_gop_camera("2010096", true, 5, {5, 4}, "PIX", true)
    };
    const auto summaries = validate_recording_configuration(cameras, make_lookup("PHB", true));
    require(!summaries[0].valid(), "PHB pair should fail PIX validation");
    require(!summaries[0].local_errors.empty(), "expected local topology error");
}

void test_peer_access_requirement_fails()
{
    const std::vector<RecordingValidationCameraInput> cameras = {
        make_split_gop_camera("2010096", true, 5, {5, 6}, "PIX", true)
    };
    const auto summaries = validate_recording_configuration(cameras, make_lookup("PIX", false));
    require(!summaries[0].valid(), "missing peer access should fail when required");
    require(!summaries[0].local_errors.empty(), "expected peer-access error");
}

void test_overlapping_gpu_claims_fail()
{
    const std::vector<RecordingValidationCameraInput> cameras = {
        make_split_gop_camera("2010095", true, 1, {1, 2}),
        make_split_gop_camera("2010096", true, 2, {2, 3})
    };
    const auto summaries = validate_recording_configuration(cameras, make_lookup("PIX", true));
    require(summaries.size() == 2, "expected two summaries");
    require(!summaries[0].valid() && !summaries[1].valid(),
            "overlapping claimed GPU sets should fail");
    require(!summaries[0].session_errors.empty() && !summaries[1].session_errors.empty(),
            "expected session conflict errors");
}

void test_non_record_camera_is_ignored_for_conflicts()
{
    const std::vector<RecordingValidationCameraInput> cameras = {
        make_split_gop_camera("2010095", true, 1, {1, 2}),
        make_split_gop_camera("2010096", false, 2, {2, 3})
    };
    const auto summaries = validate_recording_configuration(cameras, make_lookup("PIX", true));
    require(summaries.size() == 2, "expected two summaries");
    require(summaries[0].valid(), "record-enabled camera should stay valid when other camera is not recording");
    require(summaries[1].session_errors.empty(),
            "non-record camera should not participate in session conflicts");
}

void test_multiple_helpers_fail_in_current_gui_mode()
{
    const std::vector<RecordingValidationCameraInput> cameras = {
        make_split_gop_camera("2010096", true, 5, {5, 6, 7})
    };
    const auto summaries = validate_recording_configuration(cameras, make_lookup("PIX", true));
    require(!summaries[0].valid(), "multiple helper GPUs should fail current GUI validation");
    require(!summaries[0].local_errors.empty(), "expected helper-count validation error");
}

void test_preflight_is_noop_without_split_gop_recording()
{
    const std::vector<RecordingValidationCameraInput> cameras = {
        make_single_session_camera("2010096", true, 5)
    };
    const RecordingPreflightResult result =
        run_recording_preflight(cameras, make_lookup("PIX", true));
    require(result.ok, "single-session recording should not fail split-GOP preflight");
    require(!result.has_record_enabled_split_gop,
            "single-session recording should not be treated as split-GOP preflight");
    require(result.errors.empty(), "single-session preflight should produce no errors");
}

void test_preflight_flattens_conflicts()
{
    const std::vector<RecordingValidationCameraInput> cameras = {
        make_split_gop_camera("2010095", true, 1, {1, 2}),
        make_split_gop_camera("2010096", true, 2, {2, 3})
    };
    const RecordingPreflightResult result =
        run_recording_preflight(cameras, make_lookup("PIX", true));
    require(!result.ok, "conflicting split-GOP session should fail preflight");
    require(result.has_record_enabled_split_gop,
            "preflight should report split-GOP participation");
    require(!result.errors.empty(), "preflight should flatten conflict errors");
}

}  // namespace

int main()
{
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    const TestCase tests[] = {
        {"valid_pix_pair_passes", &test_valid_pix_pair_passes},
        {"topology_mismatch_fails", &test_topology_mismatch_fails},
        {"peer_access_requirement_fails", &test_peer_access_requirement_fails},
        {"overlapping_gpu_claims_fail", &test_overlapping_gpu_claims_fail},
        {"non_record_camera_is_ignored_for_conflicts", &test_non_record_camera_is_ignored_for_conflicts},
        {"multiple_helpers_fail_in_current_gui_mode", &test_multiple_helpers_fail_in_current_gui_mode},
        {"preflight_is_noop_without_split_gop_recording", &test_preflight_is_noop_without_split_gop_recording},
        {"preflight_flattens_conflicts", &test_preflight_flattens_conflicts},
    };

    for (const auto& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << test.name << ": " << e.what() << "\n";
            return EXIT_FAILURE;
        }
    }

    std::cout << "All recording validation tests passed.\n";
    return EXIT_SUCCESS;
}
