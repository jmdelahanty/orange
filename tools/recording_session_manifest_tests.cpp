#include "session/recording_session.h"
#include "NvEncoder/Logger.h"

#include <iostream>
#include <stdexcept>
#include <string>

simplelogger::Logger* logger =
    simplelogger::LoggerFactory::CreateConsoleLogger();

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

orange::session::RecordingSessionCameraArtifact make_camera_artifact(
    const std::string& serial,
    const int frame_count)
{
    orange::session::RecordingSessionCameraArtifact artifact;
    artifact.camera_serial = serial;
    artifact.video_path = "Cam" + serial + ".mp4";
    artifact.metadata_path = "Cam" + serial + "_meta.csv";
    artifact.keyframe_path = "Cam" + serial + "_keyframe.json";
    artifact.frame_count = frame_count;
    artifact.first_recording_frame_id = 1;
    artifact.last_recording_frame_id = frame_count;
    artifact.recording_frame_id_gaps = 0;
    artifact.packet_count = frame_count;
    artifact.packet_count_source = "external_recorder_summary.packets_written";
    return artifact;
}

orange::session::RecordingOutputDescriptor make_external_crop_output(
    const std::string& serial,
    const std::string& scope,
    const int frame_count)
{
    orange::session::RecordingOutputDescriptor output;
    output.camera_serial = serial;
    output.output_kind = "crop";
    output.role = "sidecar";
    output.backend = "external_ipc";
    output.status = "completed";
    output.video_path = "external_crop/Cam" + serial + "_crop_external.mp4";
    output.metadata_path = "Cam" + serial + "_crop_meta.csv";
    output.keyframe_path = "external_crop/Cam" + serial + "_crop_external_keyframe.json";
    output.perf_path = "Cam" + serial + "_crop_perf.csv";
    output.summary_path = "external_crop/Cam" + serial + "_crop_external_summary.json";
    output.frame_count = frame_count;
    output.first_recording_frame_id = 1;
    output.last_recording_frame_id = frame_count;
    output.recording_frame_id_gaps = 0;
    output.packet_count = frame_count;
    output.packet_count_source = "external_crop_recorder_summary.packets_written";
    output.width = 256;
    output.height = 256;
    output.frame_rate = 100;
    output.codec = "hevc";
    output.container = "mp4";
    output.tuning = "lossless";
    output.pixel_source_format = "mono8";
    output.encoded_format = "nv12";
    output.coordinate_space = "full_frame_pixels";
    output.details = {
        {"scope", scope},
        {"stream_id", serial + "_crop"},
        {"video_backend", "external_ipc"},
        {"metadata_backend", "orange_gui_split_crop_csv"}
    };
    return output;
}

void test_single_clip_manifest_preserves_full_and_crop_outputs()
{
    orange::session::SingleClipRecordingSessionManifestOptions options;
    options.producer = "orange_gui_external_ipc";
    options.session_id = "session_single";
    options.recording_folder = "/tmp/orange_session_manifest_single";
    options.status = "completed";
    options.recording_backend = {{"mode", "external_ipc"}, {"status", "completed"}};
    options.cameras.push_back(make_camera_artifact("2010096", 300));
    options.recording_outputs.push_back(
        make_external_crop_output("2010096", "single_clip", 300));

    const nlohmann::json manifest =
        orange::session::build_single_clip_recording_session_manifest(options);
    const nlohmann::json outputs =
        manifest.value("recording_outputs", nlohmann::json::object());
    const nlohmann::json camera_outputs =
        outputs.value("2010096", nlohmann::json::object());

    require(camera_outputs.contains("full"),
            "single-clip manifest should keep generated full output descriptor");
    require(camera_outputs.contains("crop"),
            "single-clip manifest should include explicit crop output descriptor");
    require(camera_outputs["full"].value("backend", std::string()) == "external_ipc",
            "single-clip full output should inherit external backend");
    require(camera_outputs["crop"].value("backend", std::string()) == "external_ipc",
            "single-clip crop output should keep external backend");

    const nlohmann::json clip_outputs =
        manifest["clips"][0].value("recording_outputs", nlohmann::json::object());
    require(clip_outputs.value("2010096", nlohmann::json::object()).contains("full"),
            "single-clip compatibility clip should include full output");
    require(clip_outputs.value("2010096", nlohmann::json::object()).contains("crop"),
            "single-clip compatibility clip should include crop output");
}

void test_rolling_manifest_emits_session_aggregate_and_clip_crop_outputs()
{
    orange::session::RollingRecordingSessionManifestOptions options;
    options.producer = "orange_gui_external_ipc";
    options.session_id = "session_rolling";
    options.recording_folder = "/tmp/orange_session_manifest_rolling";
    options.status = "completed";
    options.recording_control.record_for_seconds = 6;
    options.recording_control.clip_seconds = 2;
    options.recording_backend = {{"mode", "external_ipc"}, {"status", "completed"}};
    options.camera_serials.push_back("2010096");
    options.recording_outputs.push_back(
        make_external_crop_output("2010096", "session_aggregate", 601));

    orange::session::RollingClipManifestOptions clip;
    clip.producer = "orange_gui_external_ipc";
    clip.session_id = "session_rolling";
    clip.clip_index = 0;
    clip.clip_id = "clip_000000";
    clip.recording_folder = options.recording_folder;
    clip.directory = "clips/clip_000000";
    clip.status = "completed";
    clip.first_recording_frame_id = 1;
    clip.last_recording_frame_id = 200;
    clip.cameras.push_back(make_camera_artifact("2010096", 200));
    clip.recording_outputs.push_back(
        make_external_crop_output("2010096", "clip", 200));
    options.clips.push_back(clip);

    const nlohmann::json manifest =
        orange::session::build_rolling_clip_recording_session_manifest(options);
    const nlohmann::json top_outputs =
        manifest.value("recording_outputs", nlohmann::json::object());
    const nlohmann::json top_camera_outputs =
        top_outputs.value("2010096", nlohmann::json::object());
    const nlohmann::json top_crop =
        top_camera_outputs.value("crop", nlohmann::json::object());

    require(top_camera_outputs.contains("crop"),
            "rolling manifest should emit top-level session crop output");
    require(top_crop["details"].value("scope", std::string()) == "session_aggregate",
            "rolling top-level crop output should be session aggregate");
    require(!top_camera_outputs.contains("full"),
            "rolling manifest should not invent a session-wide full output descriptor");

    const nlohmann::json clip_outputs =
        manifest["clips"][0].value("recording_outputs", nlohmann::json::object());
    const nlohmann::json clip_camera_outputs =
        clip_outputs.value("2010096", nlohmann::json::object());
    require(clip_camera_outputs.contains("full"),
            "rolling clip should include clip-scoped full output");
    require(clip_camera_outputs.contains("crop"),
            "rolling clip should include clip-scoped crop output");
    require(clip_camera_outputs["crop"]["details"].value("scope", std::string()) == "clip",
            "rolling clip crop output should remain clip-scoped");
}

}  // namespace

int main()
{
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    const TestCase tests[] = {
        {"single_clip_manifest_preserves_full_and_crop_outputs",
         test_single_clip_manifest_preserves_full_and_crop_outputs},
        {"rolling_manifest_emits_session_aggregate_and_clip_crop_outputs",
         test_rolling_manifest_emits_session_aggregate_and_clip_crop_outputs},
    };

    for (const TestCase& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& ex) {
            std::cerr << "[FAIL] " << test.name << ": " << ex.what() << '\n';
            return 1;
        }
    }
    return 0;
}
