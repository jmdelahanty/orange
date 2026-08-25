#include "shaman_v2.h"
#include "shaman_v2_live_state.h"
#include "frame_ipc_manager.h"
#include "shaman_v2_recording_identity.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string unique_queue_name(const std::string& suffix)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "/orange_shaman_v2_test_" + std::to_string(getpid()) + "_" +
           std::to_string(now) + "_" + suffix;
}

shaman_v2::Slot make_slot(uint64_t frame_id)
{
    shaman_v2::Slot slot;
    slot.state_frame_id = frame_id;
    slot.source_frame_id = frame_id;
    slot.camera_frame_id = frame_id + 1000;
    slot.recording_frame_id = frame_id + 10;
    shaman_v2::copy_recording_identity_token(
        slot.recording_identity_token,
        orange::shaman_v2_recording_identity::token_for_recording_id(
            "recording-test"));
    slot.camera_timestamp_ns = 123456789ULL + frame_id;
    slot.timestamp_sys_ns = 987654321ULL + frame_id;
    slot.camera_id = 2010096;
    shaman_v2::copy_camera_serial(slot.camera_serial, "2010096");
    slot.source_width_px = 4512;
    slot.source_height_px = 4512;
    slot.detection_status = static_cast<uint32_t>(shaman_v2::DetectionStatus::kDetections);
    slot.pose_status = static_cast<uint32_t>(shaman_v2::PoseStatus::kPoses);
    slot.object_count = 1;
    slot.objects[0].x_px = 10.0f;
    slot.objects[0].y_px = 20.0f;
    slot.objects[0].width_px = 30.0f;
    slot.objects[0].height_px = 40.0f;
    slot.objects[0].confidence = 0.9f;
    slot.objects[0].label_id = 2;
    slot.objects[0].track_id = 7;
    slot.objects[0].flags = shaman_v2::kObjectHasBbox | shaman_v2::kObjectHasPose;
    slot.objects[0].keypoint_count = 2;
    slot.objects[0].keypoints[0].x_px = 11.0f;
    slot.objects[0].keypoints[0].y_px = 21.0f;
    slot.objects[0].keypoints[0].confidence = 0.8f;
    slot.objects[0].keypoints[0].label_id = 1;
    slot.objects[0].keypoints[0].flags = shaman_v2::kKeypointVisible;
    slot.objects[0].keypoints[1].x_px = 12.0f;
    slot.objects[0].keypoints[1].y_px = 22.0f;
    slot.objects[0].keypoints[1].confidence = 0.7f;
    slot.objects[0].keypoints[1].label_id = 2;
    return slot;
}

void test_queue_name_helper()
{
    require(shaman_v2::queue_name_for_camera_serial("2010096") == "/shm_cam_2010096_v2",
            "queue name should use actual camera serial plus _v2 suffix");

    bool threw = false;
    try {
        (void)shaman_v2::queue_name_for_camera_serial("");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "empty camera serial should be rejected");
}

void test_default_abi_headers()
{
    shaman_v2::Slot slot;
    shaman_v2::SharedQueue queue;
    require(shaman_v2::slot_header_valid(slot), "default slot header should be valid");
    require(shaman_v2::queue_header_valid(queue), "default queue header should be valid");
    require(slot.payload_kind ==
                static_cast<uint32_t>(shaman_v2::PayloadKind::kLatestTrackingState),
            "default payload kind should be latest_tracking_state");
    require(slot.detection_status ==
                static_cast<uint32_t>(shaman_v2::DetectionStatus::kDisabled),
            "default detection status should be disabled");
}

void test_push_pop_latest_state()
{
    const std::string name = unique_queue_name("push_pop");
    shaman_v2::unlink_queue(name);

    {
        shaman_v2::SharedLiveStateQueue writer(name, true);
        shaman_v2::SharedLiveStateQueue reader(name, false);

        require(writer.push_latest_state(make_slot(42)), "push latest-state slot should succeed");

        shaman_v2::Slot out;
        require(reader.pop(out), "reader should pop pushed slot");
        require(out.sequence_id == 1, "first sequence id should be 1");
        require(out.state_frame_id == 42, "state frame id should round trip");
        require(out.source_frame_id == 42, "source frame id should round trip");
        require(out.camera_frame_id == 1042, "camera frame id should round trip");
        require(std::strcmp(
                    out.recording_identity_token,
                    orange::shaman_v2_recording_identity::token_for_recording_id(
                        "recording-test").c_str()) == 0,
                "recording identity token should round trip");
        require(std::strcmp(out.camera_serial, "2010096") == 0,
                "camera serial should round trip");
        require(out.object_count == 1, "object count should round trip");
        require(out.objects[0].keypoint_count == 2, "keypoint count should round trip");
        require(out.objects[0].keypoints[1].x_px == 12.0f, "keypoint x should round trip");
        require(out.orange_publish_timestamp_us_epoch > 0,
                "writer should stamp publish epoch timestamp");
        require(out.orange_publish_timestamp_us_monotonic > 0,
                "writer should stamp publish monotonic timestamp");
        require(reader.empty(), "queue should be empty after pop");
    }

    shaman_v2::unlink_queue(name);
}

void test_queue_full_failure_counter()
{
    const std::string name = unique_queue_name("full");
    shaman_v2::unlink_queue(name);

    {
        shaman_v2::SharedLiveStateQueue writer(name, true);
        for (uint32_t i = 0; i < shaman_v2::kQueueSize - 1; ++i) {
            require(writer.push_latest_state(make_slot(i + 1)), "ring capacity push should succeed");
        }
        require(!writer.push_latest_state(make_slot(999)), "full ring push should fail");
        require(writer.push_failures() == 1, "push failure counter should increment");
        writer.note_stale_suppressed();
        require(writer.stale_suppressed() == 1, "stale suppression counter should increment");
    }

    shaman_v2::unlink_queue(name);
}

void corrupt_schema_version(const std::string& name)
{
    const int fd = shm_open(name.c_str(), O_RDWR, shaman_v2::kIpcMode);
    require(fd >= 0, "raw shm_open for corruption should succeed");
    void* mapped = mmap(
        nullptr,
        sizeof(shaman_v2::SharedQueue),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd,
        0);
    require(mapped != MAP_FAILED, "raw mmap for corruption should succeed");
    auto* queue = static_cast<shaman_v2::SharedQueue*>(mapped);
    queue->schema_version = 999;
    munmap(mapped, sizeof(shaman_v2::SharedQueue));
    close(fd);
}

void test_reader_rejects_abi_mismatch()
{
    const std::string name = unique_queue_name("abi");
    shaman_v2::unlink_queue(name);

    bool rejected = false;
    {
        shaman_v2::SharedLiveStateQueue writer(name, true);
        corrupt_schema_version(name);
        try {
            shaman_v2::SharedLiveStateQueue reader(name, false);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
    }
    require(rejected, "reader should reject incompatible v2 queue header");

    shaman_v2::unlink_queue(name);
}

void test_live_state_publisher_suppresses_stale_updates()
{
    const std::string name = unique_queue_name("publisher_stale");
    shaman_v2::unlink_queue(name);

    {
        shaman_v2::SharedLiveStateQueue writer(name, true);
        shaman_v2::SharedLiveStateQueue reader(name, false);
        shaman_v2::LiveStatePublisher publisher(writer);

        require(publisher.publish_base_frame(make_slot(10)), "base frame 10 should publish");

        shaman_v2::Slot stale_yolo = make_slot(9);
        stale_yolo.detection_status =
            static_cast<uint32_t>(shaman_v2::DetectionStatus::kDetections);
        require(!publisher.publish_yolo_result(stale_yolo), "stale YOLO should be suppressed");

        shaman_v2::Slot stale_pose = make_slot(9);
        stale_pose.pose_status = static_cast<uint32_t>(shaman_v2::PoseStatus::kPoses);
        require(!publisher.publish_pose_result(stale_pose), "stale pose should be suppressed");

        const shaman_v2::LiveStateCounters counters = publisher.counters_snapshot();
        require(counters.frames_published == 1, "one base frame should publish");
        require(counters.yolo_updates_published == 0, "stale YOLO should not publish");
        require(counters.pose_updates_published == 0, "stale pose should not publish");
        require(counters.yolo_stale_suppressed == 1, "YOLO stale counter should increment");
        require(counters.pose_stale_suppressed == 1, "pose stale counter should increment");
        require(writer.stale_suppressed() == 2, "queue stale counter should increment");

        shaman_v2::Slot out;
        require(reader.pop(out), "reader should see only the base frame");
        require(out.state_frame_id == 10, "base frame id should be preserved");
        require(!reader.pop(out), "reader should not see stale updates");
    }

    shaman_v2::unlink_queue(name);
}

void test_live_state_publisher_applies_pending_same_frame_updates()
{
    const std::string name = unique_queue_name("publisher_pending");
    shaman_v2::unlink_queue(name);

    {
        shaman_v2::SharedLiveStateQueue writer(name, true);
        shaman_v2::SharedLiveStateQueue reader(name, false);
        shaman_v2::LiveStatePublisher publisher(writer);

        shaman_v2::Slot yolo = make_slot(20);
        yolo.detection_status =
            static_cast<uint32_t>(shaman_v2::DetectionStatus::kDetections);
        require(!publisher.publish_yolo_result(yolo), "future YOLO should be held pending");

        shaman_v2::Slot pose = make_slot(20);
        pose.pose_status = static_cast<uint32_t>(shaman_v2::PoseStatus::kPoses);
        pose.objects[0].keypoints[0].x_px = 123.0f;
        require(!publisher.publish_pose_result(pose), "future pose should be held pending");

        require(publisher.publish_base_frame(make_slot(20)),
                "base frame should publish and then flush same-frame pending updates");

        shaman_v2::Slot out;
        require(reader.pop(out), "base state should be first");
        require(out.sequence_id == 1, "base sequence should be 1");
        require(reader.pop(out), "pending YOLO state should publish after base");
        require(out.sequence_id == 2, "YOLO sequence should be 2");
        require(out.detection_status ==
                    static_cast<uint32_t>(shaman_v2::DetectionStatus::kDetections),
                "YOLO status should be applied");
        require(reader.pop(out), "pending pose state should publish after YOLO");
        require(out.sequence_id == 3, "pose sequence should be 3");
        require(out.pose_status == static_cast<uint32_t>(shaman_v2::PoseStatus::kPoses),
                "pose status should be applied");
        require(out.objects[0].keypoints[0].x_px == 123.0f,
                "pose keypoint should be applied");
        require(!reader.pop(out), "no extra slots should publish");

        const shaman_v2::LiveStateCounters counters = publisher.counters_snapshot();
        require(counters.frames_published == 1, "base publish counter should be 1");
        require(counters.yolo_updates_published == 1, "YOLO publish counter should be 1");
        require(counters.pose_updates_published == 1, "pose publish counter should be 1");
    }

    shaman_v2::unlink_queue(name);
}

CameraParams make_test_camera(const std::string& serial)
{
    CameraParams camera{};
    camera.camera_serial = serial;
    camera.camera_id = 42;
    camera.width = 4512;
    camera.height = 4512;
    return camera;
}

std::vector<shaman_v2::Slot> wait_for_v2_slots(shaman_v2::SharedLiveStateQueue& reader,
                                               size_t expected_count)
{
    std::vector<shaman_v2::Slot> slots;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && slots.size() < expected_count) {
        shaman_v2::Slot slot;
        while (reader.pop(slot)) {
            slots.push_back(slot);
        }
        if (slots.size() >= expected_count) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return slots;
}

void wait_for_v2_stale_counter(FrameIPCManager& manager, uint64_t expected_count)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const shaman_v2::LiveStateCounters counters = manager.getV2Counters();
        if (counters.yolo_stale_suppressed >= expected_count) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("timed out waiting for v2 stale counter");
}

void test_frame_ipc_manager_opt_in_v2_base_yolo_and_stale()
{
    const std::string serial = "v2t" + std::to_string(getpid());
    const std::string v1_name = "/shm_cam_" + serial;
    const std::string v2_name = shaman_v2::queue_name_for_camera_serial(serial);
    shaman_v2::unlink_queue(v1_name);
    shaman_v2::unlink_queue(v2_name);

    {
        CameraParams camera = make_test_camera(serial);
        FrameIPCManager manager(&camera, true /* force_v2_live_state */);
        require(manager.isEnabled(), "v1 frame IPC manager should initialize");
        require(manager.isV2Enabled(), "v2 frame IPC manager should initialize when forced");
        require(manager.getV2QueueName() == v2_name, "v2 queue name should use camera serial");

        shaman_v2::SharedLiveStateQueue reader(v2_name, false);
        FrameIPCFrameIdentity identity;
        identity.legacy_frame_id = 1;
        identity.state_frame_id = 101;
        identity.camera_frame_id = 900;
        identity.recording_frame_id = 37;
        identity.recording_identity_token =
            orange::shaman_v2_recording_identity::token_for_recording_id(
                "recording-a");
        identity.camera_timestamp_ns = 111;
        identity.timestamp_sys_ns = 222;
        require(manager.sendFrame(identity, true), "base frame enqueue should succeed");

        shaman::Object detection{};
        detection.rect.x = 10.0f;
        detection.rect.y = 20.0f;
        detection.rect.width = 30.0f;
        detection.rect.height = 40.0f;
        detection.label = 3;
        detection.prob = 0.95f;
        require(manager.updateFrameWithDetections(1, 101, {detection}),
                "same-frame detection enqueue should succeed");

        std::vector<shaman_v2::Slot> slots = wait_for_v2_slots(reader, 2);
        require(slots.size() == 2, "v2 reader should receive base and YOLO slots");
        require(slots[0].state_frame_id == 101, "base v2 state frame should use local identity");
        require(slots[0].camera_frame_id == 900,
                "base v2 state should preserve camera frame identity");
        require(slots[0].recording_frame_id == 37,
                "base v2 state should preserve recording frame identity");
        require(std::strcmp(
                    slots[0].recording_identity_token,
                    identity.recording_identity_token.c_str()) == 0,
                "base v2 state should preserve recording membership token");
        require(slots[0].camera_timestamp_ns == 111 && slots[0].timestamp_sys_ns == 222,
                "base v2 state should preserve both producer timestamps");
        require(slots[0].detection_status ==
                    static_cast<uint32_t>(shaman_v2::DetectionStatus::kPending),
                "base v2 detection status should be pending");
        require(slots[1].state_frame_id == 101,
                "YOLO v2 state should remain bound to local identity");
        require(slots[1].camera_frame_id == 900 && slots[1].recording_frame_id == 37,
                "YOLO v2 update should retain base-frame identities");
        require(std::strcmp(
                    slots[1].recording_identity_token,
                    identity.recording_identity_token.c_str()) == 0,
                "YOLO v2 update should retain the base recording token");
        require(slots[1].detection_status ==
                    static_cast<uint32_t>(shaman_v2::DetectionStatus::kDetections),
                "YOLO v2 detection status should be detections");
        require(slots[1].object_count == 1, "YOLO v2 object count should be 1");
        require(slots[1].objects[0].x_px == 10.0f, "YOLO v2 object should round trip");

        identity.legacy_frame_id = 2;
        identity.state_frame_id = 102;
        identity.camera_frame_id = 901;
        identity.recording_frame_id = 38;
        identity.recording_identity_token =
            orange::shaman_v2_recording_identity::token_for_recording_id(
                "recording-b");
        identity.camera_timestamp_ns = 333;
        identity.timestamp_sys_ns = 444;
        require(manager.sendFrame(identity, true), "second base frame enqueue should succeed");
        slots = wait_for_v2_slots(reader, 1);
        require(slots.size() == 1 && slots[0].state_frame_id == 102,
                "v2 reader should receive second base frame");
        require(manager.updateFrameWithDetections(1, 101, {detection}),
                "stale detection enqueue should still be accepted by async manager");
        wait_for_v2_stale_counter(manager, 1);
        slots = wait_for_v2_slots(reader, 1);
        require(slots.empty(), "stale v2 detection should not publish a slot");

        manager.stop();
    }

    shaman_v2::unlink_queue(v1_name);
    shaman_v2::unlink_queue(v2_name);
}

void test_frame_ipc_manager_publishes_v2_pose_update()
{
    const std::string serial = "v2pose" + std::to_string(getpid());
    const std::string v1_name = "/shm_cam_" + serial;
    const std::string v2_name = shaman_v2::queue_name_for_camera_serial(serial);
    shaman_v2::unlink_queue(v1_name);
    shaman_v2::unlink_queue(v2_name);

    {
        CameraParams camera = make_test_camera(serial);
        FrameIPCManager manager(&camera, true /* force_v2_live_state */);
        require(manager.isEnabled(), "v1 frame IPC manager should initialize for pose test");
        require(manager.isV2Enabled(), "v2 frame IPC manager should initialize for pose test");

        shaman_v2::SharedLiveStateQueue reader(v2_name, false);
        require(manager.sendFrame(11, 1111, true), "pose test base frame enqueue should succeed");

        shaman_v2::Slot pose;
        pose.state_frame_id = 11;
        pose.source_frame_id = 11;
        pose.camera_frame_id = 1011;
        pose.recording_frame_id = 11;
        pose.camera_timestamp_ns = 2222;
        pose.timestamp_sys_ns = 3333;
        pose.detection_status = static_cast<uint32_t>(shaman_v2::DetectionStatus::kDetections);
        pose.pose_status = static_cast<uint32_t>(shaman_v2::PoseStatus::kPoses);
        pose.object_count = 1;
        pose.objects[0].x_px = 100.0f;
        pose.objects[0].y_px = 200.0f;
        pose.objects[0].width_px = 30.0f;
        pose.objects[0].height_px = 40.0f;
        pose.objects[0].confidence = 0.8f;
        pose.objects[0].flags = shaman_v2::kObjectHasBbox | shaman_v2::kObjectHasPose;
        pose.objects[0].keypoint_count = 1;
        pose.objects[0].keypoints[0].x_px = 111.0f;
        pose.objects[0].keypoints[0].y_px = 222.0f;
        pose.objects[0].keypoints[0].confidence = 0.7f;
        pose.objects[0].keypoints[0].flags = shaman_v2::kKeypointVisible;
        require(manager.updateFrameWithPoseResult(pose),
                "same-frame v2 pose update should enqueue");

        const std::vector<shaman_v2::Slot> slots = wait_for_v2_slots(reader, 2);
        require(slots.size() == 2, "v2 reader should receive base and pose slots");
        require(slots[0].state_frame_id == 11, "pose test base frame id should be 11");
        require(slots[1].state_frame_id == 11, "pose update frame id should be 11");
        require(slots[1].pose_status ==
                    static_cast<uint32_t>(shaman_v2::PoseStatus::kPoses),
                "pose update status should be poses");
        require(slots[1].detection_status ==
                    static_cast<uint32_t>(shaman_v2::DetectionStatus::kDetections),
                "pose update should preserve detection result status");
        require(slots[1].object_count == 1, "pose update should carry one object");
        require(slots[1].objects[0].keypoint_count == 1,
                "pose update should carry one keypoint");
        require(slots[1].objects[0].keypoints[0].x_px == 111.0f,
                "pose update keypoint should round trip");

        manager.stop();
    }

    shaman_v2::unlink_queue(v1_name);
    shaman_v2::unlink_queue(v2_name);
}

void test_frame_ipc_manager_publishes_terminal_zero_detection_state()
{
    const std::string serial = "v2zero" + std::to_string(getpid());
    const std::string v1_name = "/shm_cam_" + serial;
    const std::string v2_name = shaman_v2::queue_name_for_camera_serial(serial);
    shaman_v2::unlink_queue(v1_name);
    shaman_v2::unlink_queue(v2_name);

    {
        CameraParams camera = make_test_camera(serial);
        FrameIPCManager manager(&camera, true /* force_v2_live_state */);
        shaman_v2::SharedLiveStateQueue reader(v2_name, false);
        require(manager.sendFrame(21, 1234, true),
                "zero-detection base frame should enqueue");
        require(manager.updateFrameWithDetectionResult(
                    21,
                    21,
                    shaman_v2::DetectionStatus::kZeroDetections,
                    {}),
                "zero-detection terminal update should enqueue");

        const auto slots = wait_for_v2_slots(reader, 2);
        require(slots.size() == 2,
                "zero-detection frame should publish base and terminal states");
        require(slots[0].detection_status ==
                    static_cast<uint32_t>(shaman_v2::DetectionStatus::kPending),
                "base state should remain explicitly pending");
        require(slots[1].detection_status ==
                    static_cast<uint32_t>(shaman_v2::DetectionStatus::kZeroDetections),
                "terminal state should explicitly report zero detections");
        require(slots[1].object_count == 0,
                "zero-detection terminal state should carry no objects");
        manager.stop();
    }

    shaman_v2::unlink_queue(v1_name);
    shaman_v2::unlink_queue(v2_name);
}

} // namespace

int main()
{
    try {
        test_queue_name_helper();
        test_default_abi_headers();
        test_push_pop_latest_state();
        test_queue_full_failure_counter();
        test_reader_rejects_abi_mismatch();
        test_live_state_publisher_suppresses_stale_updates();
        test_live_state_publisher_applies_pending_same_frame_updates();
        test_frame_ipc_manager_opt_in_v2_base_yolo_and_stale();
        test_frame_ipc_manager_publishes_v2_pose_update();
        test_frame_ipc_manager_publishes_terminal_zero_detection_state();
    } catch (const std::exception& ex) {
        std::cerr << "shaman_v2_tests failed: " << ex.what() << std::endl;
        return 1;
    }

    std::cout << "shaman_v2_tests passed" << std::endl;
    return 0;
}
