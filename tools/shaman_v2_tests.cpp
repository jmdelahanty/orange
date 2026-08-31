#include "shaman_v2.h"
#include "shaman_v2_live_state.h"
#include "frame_ipc_manager.h"
#include "shaman_v2_recording_identity.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
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
    slot.source_detection_count = 1;
    slot.retained_detection_count = 1;
    slot.transmitted_object_count = 1;
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

shaman_v2::Slot make_base_pending_slot(uint64_t frame_id)
{
    shaman_v2::Slot slot = make_slot(frame_id);
    slot.detection_status = static_cast<uint32_t>(
        shaman_v2::DetectionStatus::kPending);
    slot.pose_status = static_cast<uint32_t>(
        shaman_v2::PoseStatus::kNotRequested);
    slot.detection_reason = static_cast<uint32_t>(
        shaman_v2::DetectionResultReason::kNone);
    slot.object_count = 0;
    slot.source_detection_count = 0;
    slot.retained_detection_count = 0;
    slot.transmitted_object_count = 0;
    slot.objects_truncated = 0;
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

    const std::string hyphen_lock = shaman_v2::index_lock_path("/shm_cam_a-b_v2");
    const std::string underscore_lock =
        shaman_v2::index_lock_path("/shm_cam_a_b_v2");
    require(hyphen_lock != underscore_lock,
            "index lock helper must disambiguate sanitized queue-name aliases");
    require(hyphen_lock.rfind(".lock") == hyphen_lock.size() - 5 &&
                underscore_lock.rfind(".lock") == underscore_lock.size() - 5,
            "index lock helper must use a stable .lock suffix");
}

void test_default_abi_headers()
{
    shaman_v2::Slot slot;
    shaman_v2::SharedQueue queue;
    require(shaman_v2::kSchemaVersion == 4, "grouped-live ABI should be revision 4");
    require(shaman_v2::slot_header_valid(slot), "default slot header should be valid");
    require(shaman_v2::queue_header_valid(queue), "default queue header should be valid");
    require(slot.payload_kind ==
                static_cast<uint32_t>(shaman_v2::PayloadKind::kLatestTrackingState),
            "default payload kind should be latest_tracking_state");
    require(slot.detection_status ==
                static_cast<uint32_t>(shaman_v2::DetectionStatus::kDisabled),
            "default detection status should be disabled");
    require(slot.object_order == shaman_v2::kObjectOrderUnorderedPayloadLocal,
            "object order must be explicitly unordered_payload_local");
}

void test_queue_single_writer_and_restart_generation()
{
    const std::string name = unique_queue_name("generation");
    shaman_v2::unlink_queue(name);

    uint64_t first_generation = 0;
    {
        shaman_v2::SharedLiveStateQueue writer(name, true);
        first_generation = writer.producer_generation();
        require(first_generation > 0, "first writer generation should be nonzero");
        require(writer.producer_instance_id() > 0,
                "first writer instance identity should be nonzero");

        bool second_writer_rejected = false;
        try {
            shaman_v2::SharedLiveStateQueue duplicate(name, true);
        } catch (const std::exception&) {
            second_writer_rejected = true;
        }
        require(second_writer_rejected,
                "a second simultaneous SHAMAN writer must be rejected");
        require(writer.push_latest_state(make_slot(71)),
                "first writer should publish a state before restart");
    }

    {
        shaman_v2::SharedLiveStateQueue writer(name, true);
        require(writer.producer_generation() > first_generation,
                "writer restart must advance producer generation");
        shaman_v2::SharedLiveStateQueue reader(name, false);
        shaman_v2::Slot out;
        require(!reader.pop(out),
                "restart reset must discard predecessor slots before publishing");
        require(writer.push_latest_state(make_slot(72)),
                "restarted writer should publish a state");
        require(reader.pop(out), "reader should receive restarted state");
        require(out.producer_generation == writer.producer_generation(),
                "slot should carry current producer generation");
        require(out.producer_instance_id == writer.producer_instance_id(),
                "slot should carry current producer instance identity");
    }

    shaman_v2::unlink_queue(name);
}

void test_writer_recovers_interrupted_reset()
{
    const std::string name = unique_queue_name("interrupted_reset");
    shaman_v2::unlink_queue(name);

    uint64_t previous_generation = 0;
    {
        shaman_v2::SharedLiveStateQueue writer(name, true);
        previous_generation = writer.producer_generation();
    }

    // Simulate a writer dying after marking a valid ABI4 queue uninitialized,
    // but before it can finish clearing and publishing the new generation.
    const int fd = shm_open(name.c_str(), O_RDWR, shaman_v2::kIpcMode);
    require(fd >= 0, "interrupted-reset raw shm_open should succeed");
    void* mapped = mmap(
        nullptr,
        sizeof(shaman_v2::SharedQueue),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd,
        0);
    require(mapped != MAP_FAILED, "interrupted-reset raw mmap should succeed");
    auto* queue = static_cast<shaman_v2::SharedQueue*>(mapped);
    require(shaman_v2::queue_header_valid(*queue),
            "interrupted-reset fixture must retain a valid ABI4 header");
    queue->initialized.store(false, std::memory_order_release);
    munmap(mapped, sizeof(shaman_v2::SharedQueue));
    close(fd);

    {
        // A writer must self-heal this valid-but-interrupted reset rather than
        // requiring the operator to unlink the queue.
        shaman_v2::SharedLiveStateQueue writer(name, true);
        require(writer.producer_generation() > previous_generation,
                "interrupted reset should advance the producer generation");
        require(writer.push_latest_state(make_slot(81)),
                "self-healed writer should publish after interrupted reset");
        shaman_v2::SharedLiveStateQueue reader(name, false);
        shaman_v2::Slot out;
        require(reader.pop(out), "reader should consume self-healed state");
        require(out.state_frame_id == 81,
                "self-healed queue should not expose predecessor slots");
    }

    shaman_v2::unlink_queue(name);
}

void test_concurrent_reader_during_writer_resets()
{
    const std::string name = unique_queue_name("generation_race");
    shaman_v2::unlink_queue(name);
    {
        auto writer = std::make_unique<shaman_v2::SharedLiveStateQueue>(name, true);
        shaman_v2::SharedLiveStateQueue reader(name, false);
        std::atomic<bool> stop{false};
        std::atomic<bool> bad_slot{false};
        std::thread reader_thread([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                shaman_v2::Slot slot;
                while (reader.pop(slot)) {
                    if (!shaman_v2::slot_header_valid(slot) ||
                        slot.producer_generation == 0 ||
                        slot.producer_instance_id == 0) {
                        bad_slot.store(true, std::memory_order_release);
                    }
                }
                std::this_thread::yield();
            }
        });

        for (uint64_t restart = 0; restart < 8; ++restart) {
            require(writer->push_latest_state(make_slot(100 + restart)),
                    "race writer should publish before reset");
            std::this_thread::yield();
            // The writer's authority lock and sidecar index lock make this
            // reset atomic with respect to every reader tail update.
            writer.reset();
            writer = std::make_unique<shaman_v2::SharedLiveStateQueue>(name, true);
        }
        stop.store(true, std::memory_order_release);
        reader_thread.join();
        require(!bad_slot.load(std::memory_order_acquire),
                "concurrent reset reader accepted a malformed/stale slot");
    }
    shaman_v2::unlink_queue(name);
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

void test_shm_push_rejects_malformed_grouped_payload()
{
    const std::string name = unique_queue_name("payload_boundary");
    shaman_v2::unlink_queue(name);
    {
        shaman_v2::SharedLiveStateQueue writer(name, true);

        auto bad_counts = make_slot(1001);
        bad_counts.retained_detection_count = 2;
        require(!writer.push_latest_state(bad_counts),
                "SHM push must reject inconsistent grouped counts");

        auto bad_order = make_slot(1002);
        bad_order.object_order = static_cast<uint32_t>(
            shaman_v2::ObjectOrder::kUnspecified);
        require(!writer.push_latest_state(bad_order),
                "SHM push must reject unspecified object order");

        auto bad_reason = make_slot(1003);
        bad_reason.detection_reason = static_cast<uint32_t>(
            shaman_v2::DetectionResultReason::kProcessingFailed);
        require(!writer.push_latest_state(bad_reason),
                "SHM push must reject a success status with failure reason");
        require(writer.push_failures() == 3,
                "malformed grouped payloads must increment SHM failures");
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

void test_pose_before_yolo_does_not_seed_detection_completeness()
{
    const std::string name = unique_queue_name("pose_before_yolo");
    shaman_v2::unlink_queue(name);

    {
        shaman_v2::SharedLiveStateQueue writer(name, true);
        shaman_v2::SharedLiveStateQueue reader(name, false);
        shaman_v2::LiveStatePublisher publisher(writer);

        require(publisher.publish_base_frame(make_base_pending_slot(30)),
                "pending base frame should publish");

        shaman_v2::Slot pose = make_slot(30);
        pose.objects[0].keypoints[0].x_px = 123.0f;
        require(publisher.publish_pose_result(pose),
                "early pose should publish an interim pose status");

        shaman_v2::Slot out;
        require(reader.pop(out), "base slot should publish first");
        require(out.detection_status == static_cast<uint32_t>(
                    shaman_v2::DetectionStatus::kPending) &&
                    out.object_count == 0 &&
                    out.source_detection_count == 0 &&
                    out.retained_detection_count == 0 &&
                    out.transmitted_object_count == 0,
                "base should remain detection-pending");
        require(reader.pop(out), "early pose interim slot should publish");
        require(out.pose_status == static_cast<uint32_t>(
                    shaman_v2::PoseStatus::kPoses) &&
                    out.detection_status == static_cast<uint32_t>(
                        shaman_v2::DetectionStatus::kPending) &&
                    out.object_count == 0 &&
                    out.source_detection_count == 0 &&
                    out.retained_detection_count == 0 &&
                    out.transmitted_object_count == 0,
                "early pose must not seed authoritative detection completeness");

        shaman_v2::Slot yolo = make_base_pending_slot(30);
        yolo.detection_status = static_cast<uint32_t>(
            shaman_v2::DetectionStatus::kDetections);
        yolo.pose_status = static_cast<uint32_t>(
            shaman_v2::PoseStatus::kPoses);
        yolo.object_count = 4;
        yolo.source_detection_count = 4;
        yolo.retained_detection_count = 4;
        yolo.transmitted_object_count = 4;
        for (uint32_t index = 0; index < yolo.object_count; ++index) {
            auto& object = yolo.objects[index];
            object.x_px = index == 0 ? 10.0f : 100.0f + index * 40.0f;
            object.y_px = index == 0 ? 20.0f : 200.0f + index * 30.0f;
            object.width_px = 30.0f;
            object.height_px = 40.0f;
            object.confidence = 0.8f;
            object.label_id = static_cast<int32_t>(index + 7);
            object.track_id = static_cast<int32_t>(index);
            object.flags = shaman_v2::kObjectHasBbox;
        }
        require(publisher.publish_yolo_result(yolo),
                "same-frame YOLO terminal should publish after early pose");
        require(reader.pop(out), "authoritative YOLO slot should publish");
        require(out.detection_status == static_cast<uint32_t>(
                    shaman_v2::DetectionStatus::kDetections) &&
                    out.object_count == 4 &&
                    out.source_detection_count == 4 &&
                    out.retained_detection_count == 4 &&
                    out.transmitted_object_count == 4,
                "YOLO terminal should establish the complete four-box vector");
        require(out.objects[0].keypoint_count == 2 &&
                    out.objects[0].keypoints[0].x_px == 123.0f,
                "early pose evidence should merge into the matching YOLO box");
        require(out.objects[1].keypoint_count == 0 &&
                    out.objects[2].keypoint_count == 0 &&
                    out.objects[3].keypoint_count == 0,
                "early pose must not invent keypoints for other boxes");
        require(!reader.pop(out), "pose-before-YOLO test should have no extra slots");
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
        require(slots[1].source_detection_count == 1 &&
                    slots[1].retained_detection_count == 1 &&
                    slots[1].transmitted_object_count == 1 &&
                    slots[1].objects_truncated == 0,
                "YOLO v2 grouped counts should describe the complete payload");
        require(slots[1].object_order == shaman_v2::kObjectOrderUnorderedPayloadLocal,
                "YOLO object order should be explicitly unordered_payload_local");
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
        FrameIPCFrameIdentity base_identity;
        base_identity.legacy_frame_id = 11;
        base_identity.state_frame_id = 11;
        base_identity.camera_frame_id = 1011;
        base_identity.recording_frame_id = 211;
        base_identity.camera_timestamp_ns = 1111;
        require(manager.sendFrame(base_identity, true),
                "pose test base frame enqueue should succeed");

        std::vector<shaman::Object> detections(4);
        for (std::size_t index = 0; index < detections.size(); ++index) {
            detections[index].rect.x = index == 0 ? 100.0f
                                                  : 300.0f + index * 20.0f;
            detections[index].rect.y = index == 0 ? 200.0f
                                                  : 300.0f + index * 20.0f;
            detections[index].rect.width = 30.0f;
            detections[index].rect.height = 40.0f;
            detections[index].label = static_cast<int>(index + 7);
            detections[index].prob = 0.8f;
        }
        require(manager.updateFrameWithDetections(11, 11, std::move(detections)),
                "pose test YOLO vector should enqueue before pose");

        shaman_v2::Slot pose;
        pose.state_frame_id = 11;
        pose.source_frame_id = 11;
        pose.camera_frame_id = 1011;
        // Recording-local numbering intentionally differs from stream-local
        // state identity. Pose must remain bound to local frame 11.
        pose.recording_frame_id = 211;
        pose.camera_timestamp_ns = 2222;
        pose.timestamp_sys_ns = 3333;
        pose.detection_status = static_cast<uint32_t>(shaman_v2::DetectionStatus::kDetections);
        pose.pose_status = static_cast<uint32_t>(shaman_v2::PoseStatus::kPoses);
        pose.object_count = 1;
        pose.source_detection_count = 1;
        pose.retained_detection_count = 1;
        pose.transmitted_object_count = 1;
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
        auto oversized_pose = pose;
        oversized_pose.object_count = shaman_v2::kMaxObjects + 1;
        require(!manager.updateFrameWithPoseResult(std::move(oversized_pose)),
                "pose object count above ABI capacity must fail closed");
        auto oversized_keypoints = pose;
        oversized_keypoints.objects[0].keypoint_count =
            shaman_v2::kMaxKeypointsPerObject + 1;
        require(!manager.updateFrameWithPoseResult(
                    std::move(oversized_keypoints)),
                "pose keypoint count above ABI capacity must fail closed");
        require(manager.updateFrameWithPoseResult(pose),
                "same-frame v2 pose update should enqueue");

        const std::vector<shaman_v2::Slot> slots = wait_for_v2_slots(reader, 3);
        require(slots.size() == 3,
                "v2 reader should receive base, YOLO, and pose slots");
        require(slots[0].state_frame_id == 11, "pose test base frame id should be 11");
        require(slots[1].object_count == 4 &&
                    slots[1].transmitted_object_count == 4,
                "YOLO pose test should establish the complete four-box vector");
        require(slots[2].state_frame_id == 11, "pose update frame id should be 11");
        require(slots[2].source_frame_id == 11,
                "pose source frame should use stream-local local_frame_id");
        require(slots[2].recording_frame_id == 211,
                "pose recording identity should remain metadata only");
        require(slots[2].pose_status ==
                    static_cast<uint32_t>(shaman_v2::PoseStatus::kPoses),
                "pose update status should be poses");
        require(slots[2].detection_status ==
                    static_cast<uint32_t>(shaman_v2::DetectionStatus::kDetections),
                "pose update should preserve detection result status");
        require(slots[2].object_count == 4 &&
                    slots[2].transmitted_object_count == 4 &&
                    slots[2].retained_detection_count == 4,
                "pose update must preserve the complete four-box detection vector");
        require(slots[2].objects[0].label_id == 7 &&
                    slots[2].objects[0].keypoint_count == 1,
                "pose should enrich the uniquely matching detection in place");
        require(slots[2].objects[1].keypoint_count == 0 &&
                    slots[2].objects[2].keypoint_count == 0 &&
                    slots[2].objects[3].keypoint_count == 0,
                "pose enrichment must not invent keypoints for other detections");
        require(slots[2].objects[0].keypoints[0].x_px == 111.0f,
                "pose update keypoint should round trip");

        manager.stop();
    }

    shaman_v2::unlink_queue(v1_name);
    shaman_v2::unlink_queue(v2_name);
}

void test_frame_ipc_manager_fails_closed_on_object_overflow()
{
    const std::string serial = "v2overflow" + std::to_string(getpid());
    const std::string v1_name = "/shm_cam_" + serial;
    const std::string v2_name = shaman_v2::queue_name_for_camera_serial(serial);
    shaman_v2::unlink_queue(v1_name);
    shaman_v2::unlink_queue(v2_name);

    {
        CameraParams camera = make_test_camera(serial);
        FrameIPCManager manager(&camera, true /* force_v2_live_state */);
        shaman_v2::SharedLiveStateQueue reader(v2_name, false);
        require(manager.sendFrame(31, 3131, true),
                "overflow base frame should enqueue");

        std::vector<shaman::Object> detections(shaman_v2::kMaxObjects + 1);
        for (std::size_t i = 0; i < detections.size(); ++i) {
            detections[i].rect.x = static_cast<float>(i);
            detections[i].rect.width = 10.0f;
            detections[i].rect.height = 10.0f;
        }
        const uint64_t model_hash = shaman_v2::fnv1a64("grouped-test-model");
        require(manager.updateFrameWithDetectionResult(
                    31,
                    31,
                    shaman_v2::DetectionStatus::kFailed,
                    std::move(detections),
                    shaman_v2::kMaxObjects + 1,
                    shaman_v2::kMaxObjects + 1,
                    model_hash,
                    shaman_v2::DetectionResultReason::kObjectsTruncated),
                "overflow detection update should enqueue for explicit failure publication");

        auto slots = wait_for_v2_slots(reader, 2);
        require(slots.size() == 2,
                "overflow frame should publish base and terminal failed states");
        const shaman_v2::Slot& terminal = slots[1];
        require(terminal.detection_status ==
                    static_cast<uint32_t>(shaman_v2::DetectionStatus::kFailed),
                "overflow must not publish a complete detections status");
        require(terminal.detection_reason == static_cast<uint32_t>(
                    shaman_v2::DetectionResultReason::kObjectsTruncated),
                "overflow must carry a stable truncation reason");
        require(terminal.source_detection_count == shaman_v2::kMaxObjects + 1 &&
                    terminal.retained_detection_count == shaman_v2::kMaxObjects + 1 &&
                    terminal.transmitted_object_count == shaman_v2::kMaxObjects &&
                    terminal.objects_truncated == 1,
                "overflow counts must distinguish source, retained, and transmitted objects");
        require(terminal.detection_model_id_hash == model_hash,
                "detection model identity hash should round trip");
        manager.stop();
    }

    shaman_v2::unlink_queue(v1_name);
    shaman_v2::unlink_queue(v2_name);
}

void test_frame_ipc_manager_publishes_enqueue_rejection_failure()
{
    const std::string serial = "v2enqueuefail" + std::to_string(getpid());
    const std::string v1_name = "/shm_cam_" + serial;
    const std::string v2_name = shaman_v2::queue_name_for_camera_serial(serial);
    shaman_v2::unlink_queue(v1_name);
    shaman_v2::unlink_queue(v2_name);

    {
        CameraParams camera = make_test_camera(serial);
        FrameIPCManager manager(&camera, true /* force_v2_live_state */);
        shaman_v2::SharedLiveStateQueue reader(v2_name, false);
        const uint64_t model_hash = shaman_v2::fnv1a64("enqueue-failure-model");
        require(manager.sendFrame(41, 4141, true),
                "enqueue-rejection base frame should enqueue");
        require(manager.publishYoloWorkerEnqueueRejected(41, 41, model_hash),
                "YOLO enqueue rejection should enqueue terminal v2 failure");
        auto slots = wait_for_v2_slots(reader, 2);
        require(slots.size() == 2,
                "enqueue rejection should publish base and terminal failure");
        require(slots[1].detection_status == static_cast<uint32_t>(
                    shaman_v2::DetectionStatus::kFailed),
                "enqueue rejection terminal state should be failed");
        require(slots[1].detection_reason == static_cast<uint32_t>(
                    shaman_v2::DetectionResultReason::kYoloWorkerEnqueueRejected),
                "enqueue rejection should carry stable failure reason");
        require(slots[1].detection_model_id_hash == model_hash,
                "enqueue rejection should retain detection model identity");
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

        auto slots = wait_for_v2_slots(reader, 2);
        require(slots.size() == 2,
                "zero-detection frame should publish base and terminal states");
        require(slots[0].detection_status ==
                    static_cast<uint32_t>(shaman_v2::DetectionStatus::kPending),
                "base state should remain explicitly pending");
        require(slots[1].detection_status ==
                    static_cast<uint32_t>(shaman_v2::DetectionStatus::kZeroDetections),
                "terminal state should explicitly report zero detections");
        require(slots[1].detection_reason == static_cast<uint32_t>(
                    shaman_v2::DetectionResultReason::kNoSourceDetections),
                "zero source result should carry a stable zero reason");
        require(slots[1].object_count == 0,
                "zero-detection terminal state should carry no objects");

        require(manager.sendFrame(22, 1235, true),
                "all-masked zero-detection base frame should enqueue");
        require(manager.updateFrameWithDetectionResult(
                    22,
                    22,
                    shaman_v2::DetectionStatus::kZeroDetections,
                    {},
                    3,
                    0,
                    shaman_v2::fnv1a64("mask-test-model"),
                    shaman_v2::DetectionResultReason::kAllDetectionsRejectedByMask),
                "all-masked zero-detection terminal update should enqueue");
        slots = wait_for_v2_slots(reader, 2);
        require(slots.size() == 2 &&
                    slots[1].detection_reason == static_cast<uint32_t>(
                        shaman_v2::DetectionResultReason::kAllDetectionsRejectedByMask) &&
                    slots[1].source_detection_count == 3 &&
                    slots[1].retained_detection_count == 0,
                "all-masked zero result must distinguish its source count/reason");
        manager.stop();
    }

    shaman_v2::unlink_queue(v1_name);
    shaman_v2::unlink_queue(v2_name);
}

void test_frame_ipc_manager_pending_requires_live_terminal_capability()
{
    const std::string serial = "v2pending" + std::to_string(getpid());
    const std::string v1_name = "/shm_cam_" + serial;
    const std::string v2_name = shaman_v2::queue_name_for_camera_serial(serial);
    shaman_v2::unlink_queue(v1_name);
    shaman_v2::unlink_queue(v2_name);

    {
        CameraParams camera = make_test_camera(serial);
        FrameIPCManager manager(&camera, true /* force_v2_live_state */);
        require(manager.isEnabled() && manager.isV2Enabled(),
                "pending-capability test requires v1 and v2 IPC");
        shaman_v2::SharedLiveStateQueue reader(v2_name, false);

        FrameIPCFrameIdentity identity;
        identity.legacy_frame_id = 1;
        identity.state_frame_id = 1;
        identity.camera_frame_id = 1001;
        // YOLO can still run for recording/analytics while live publication
        // is disabled. The explicit false terminal capability must therefore
        // produce not_scheduled rather than a permanently pending base.
        require(manager.sendFrame(identity, true, false),
                "non-live YOLO base frame enqueue should succeed");
        auto slots = wait_for_v2_slots(reader, 1);
        require(slots.size() == 1 &&
                    slots[0].detection_status == static_cast<uint32_t>(
                        shaman_v2::DetectionStatus::kNotScheduled),
                "base must not be pending when no live result can be published");

        identity.legacy_frame_id = 2;
        identity.state_frame_id = 2;
        identity.camera_frame_id = 1002;
        require(manager.sendFrame(identity, true, true),
                "synthetic live base frame enqueue should succeed");
        shaman::Object synthetic_detection{};
        synthetic_detection.rect.width = 10.0f;
        synthetic_detection.rect.height = 10.0f;
        synthetic_detection.prob = 0.5f;
        require(manager.publishSyntheticYoloResult(
                    2,
                    2,
                    {synthetic_detection},
                    1,
                    1,
                    shaman_v2::fnv1a64("synthetic-test-model"),
                    shaman_v2::DetectionStatus::kDetections,
                    shaman_v2::DetectionResultReason::kNone),
                "synthetic terminal v2 result should enqueue");
        slots = wait_for_v2_slots(reader, 2);
        require(slots.size() == 2 &&
                    slots[0].detection_status == static_cast<uint32_t>(
                        shaman_v2::DetectionStatus::kPending) &&
                    slots[1].detection_status == static_cast<uint32_t>(
                        shaman_v2::DetectionStatus::kDetections),
                "synthetic live result must terminate its pending base");
        require((slots[1].objects[0].flags & shaman_v2::kObjectSynthetic) != 0,
                "synthetic live object must be explicitly marked synthetic");
        require(manager.getUpdatesSent() == 0,
                "synthetic v2 terminal result must not alter the legacy stream");
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
        test_queue_single_writer_and_restart_generation();
        test_writer_recovers_interrupted_reset();
        test_concurrent_reader_during_writer_resets();
        test_push_pop_latest_state();
        test_queue_full_failure_counter();
        test_shm_push_rejects_malformed_grouped_payload();
        test_reader_rejects_abi_mismatch();
        test_live_state_publisher_suppresses_stale_updates();
        test_live_state_publisher_applies_pending_same_frame_updates();
        test_pose_before_yolo_does_not_seed_detection_completeness();
        test_frame_ipc_manager_opt_in_v2_base_yolo_and_stale();
        test_frame_ipc_manager_publishes_v2_pose_update();
        test_frame_ipc_manager_publishes_terminal_zero_detection_state();
        test_frame_ipc_manager_pending_requires_live_terminal_capability();
        test_frame_ipc_manager_fails_closed_on_object_overflow();
        test_frame_ipc_manager_publishes_enqueue_rejection_failure();
    } catch (const std::exception& ex) {
        std::cerr << "shaman_v2_tests failed: " << ex.what() << std::endl;
        return 1;
    }

    std::cout << "shaman_v2_tests passed" << std::endl;
    return 0;
}
