#include "gui/registered_context_recording.h"
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
using json = nlohmann::json;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class F> void refuses(F&& f) {
    bool failed = false;
    try { f(); } catch (const std::exception&) { failed = true; }
    check(failed, "unsafe GUI context arm accepted");
}
}
int main() {
    std::string temporary = "/tmp/orange_gui_context_controls_XXXXXX";
    if (!mkdtemp(temporary.data())) return 1;
    const std::filesystem::path root = temporary, path = root / "app.json";
    int result = 0;
    try {
        using namespace orange::gui;
        using Cameras = std::vector<orange::recording::CameraConfigMediaProducts>;
        auto cameras = [](const nlohmann::json& media) {
            return Cameras{{"2010093", media.dump()}, {"2010094", media.dump()}};
        };
        LoadRegisteredContextRecordingSettings(path);
        check(!RecordingMediaSelectionForStream({}).mode, "old GUI configuration opted into a product plan");
        check(!RecordingMediaSelectionForStream(Cameras{{"2010093", std::nullopt}}).mode,
              "camera configs without media_products opted into a product plan");
        check(!RegisteredContextRecordingConfigForArm().enabled, "missing config is not opt-in");
        const auto crop_only = orange::recording::RecordingMediaSelection::Parse(
            {{"schema_version", 1}, {"mode", "registered_context_and_moving_crops"}});
        // The camera configuration decides; crop-only needs the registered context.
        check(RecordingMediaSelectionForStream(cameras(crop_only.ToJson())).RequiresContext(), "camera crop-only lost");
        refuses([&] { RegisteredContextRecordingConfigForArm(); });
        // Cameras that disagree, or set it on some cameras only, refuse the stream start.
        refuses([&] { RecordingMediaSelectionForStream(Cameras{{"2010093", crop_only.ToJson().dump()},
            {"2010094", json({{"schema_version", 1}, {"mode", "full_frame"}}).dump()}}); });
        refuses([&] { RecordingMediaSelectionForStream(Cameras{{"2010093", crop_only.ToJson().dump()},
            {"2010094", std::nullopt}}); });
        refuses([&] { RecordingMediaSelectionForStream(Cameras{{"2010093", "null"}}); });
        refuses([&] { RecordingMediaSelectionForStream(Cameras{{"2010093", json({{"schema_version", 1}, {"mode", "croponly"}}).dump()}}); });
        // The app config's old key is refused, not read.
        { std::ofstream out(path); out << json{{"recording", {{"media_products", crop_only.ToJson()}}}}; }
        LoadRegisteredContextRecordingSettings(path);
        refuses([&] { RecordingMediaSelectionForStream(cameras(crop_only.ToJson())); });
        refuses([&] { RecordingMediaSelectionForStream({}); });
        std::filesystem::remove(path); // unique fixture preference file
        LoadRegisteredContextRecordingSettings(path);
        const json context = {
            {"schema_version", 2}, {"enabled", true}, {"worker_cpu_ids", {0}},
            {"source", {{"kind", "daily_registration"}, {"descriptor_path", (root / "context.json").string()},
                {"size_bytes", 10}, {"sha256", "sha256:" + std::string(64, 'a')}, {"scene_unchanged_since_capture", true}}},
            {"declaration", {{"schema_id", "orange.recording.registered_scene_context.capture_declaration"},
                {"schema_version", 1}, {"registration_authority_status", "accepted_for_experiment"},
                {"subject_presence", "present"}, {"dish_setup_complete", true}, {"nir_illumination_fixed", true},
                {"camera_configuration_fixed", true}, {"rig_fixed", true}}}};
        // These tests exercise controls only; no artifact import or CPU pinning.
        SelectDailyContextForGuiRecording(context);
        const auto config = RegisteredContextRecordingConfigForArm();
        check(config.enabled && config.master.enabled && config.master.writer_cpu_ids == std::vector<int>{0},
            "daily selection did not configure journal/context together");
        check(!std::filesystem::exists(path), "selecting context silently saved global config");
        const auto media = orange::recording::RecordingMediaSelection::Parse(
            {{"schema_version", 1}, {"mode", "full_frame_and_moving_crops"}});
        orange::recording::SaveGuiRecordingEvidenceConfig(path, config);
        ConsumeRegisteredContextRecordingConfirmation();
        refuses([&] { RegisteredContextRecordingConfigForArm(); });
        LoadRegisteredContextRecordingSettings(path);
        check(RecordingMediaSelectionForStream(cameras(media.ToJson())).ToJson() == media.ToJson(),
              "GUI did not take the cameras' media choice");
        LoadRegisteredContextRecordingSettings(path, false);
        check(RecordingMediaSelectionForStream(cameras(media.ToJson())).ToJson() == media.ToJson(),
              "context reload changed the cameras' media choice");
        refuses([&] { RegisteredContextRecordingConfigForArm(); });
        SelectDailyContextForGuiRecording(context);
        check(RegisteredContextRecordingConfigForArm().enabled, "new confirmation did not allow another arm");
        ConsumeRegisteredContextRecordingConfirmation();
        refuses([&] { RegisteredContextRecordingConfigForArm(); });
        auto bad = config.ToJson(); bad["enabled"] = "true";
        { std::ofstream out(path); out << json{{"recording", {{"registered_context_recording", bad}}}}; }
        LoadRegisteredContextRecordingSettings(path);
        refuses([&] { RegisteredContextRecordingConfigForArm(); });
        // A deliberate valid operator selection replaces the bad in-memory option.
        SelectDailyContextForGuiRecording(context);
        check(RegisteredContextRecordingConfigForArm().enabled, "operator cannot repair invalid selection");

        // A remote start declares its own subjects and intent: the saved table
        // and intent are not used, and no panel Confirm is needed.
        {
            std::ofstream out(path);
            out << json{{"recording",
                         {{"contexts", {{"schema_version", 1},
                                        {"default", {{"behavior_mode", "free"}, {"data_origin", "acquired"},
                                                     {"recording_intent", "stimulus_experiment"},
                                                     {"recording_type", "behavior"}}}}},
                          {"subject_references", {{"schema_version", 1},
                                                  {"default", {{"dish_id", "saved-dish"}, {"subject_count", 1}}}}}}}};
        }
        LoadRegisteredContextRecordingSettings(path);
        const std::vector<std::string> serials = {"2010093", "2010094"};
        refuses([&] { SubjectReferencesForRecordStart(nullptr, serials); });  // no Confirm, no declaration
        const RemoteRecordStart remote{
            orange::session::ParseRecordingStartDeclaration(
                {{"schema_id", "orange.recording_start_declaration"}, {"schema_version", 1},
                 {"declared_by", "delahantyj"}, {"recording_intent", "recording_only"},
                 {"subject_references", {{"schema_version", 1}}}}),
            {{"transport", "local_control_socket"}, {"peer", {{"uid", 1000}, {"user", "jeremy"}}}}};
        check(RecordingContextsForStream().default_context->recording_intent == "stimulus_experiment",
              "the saved intent is unchanged");
        check(RecordingContextsForRecordStart(&remote).default_context->recording_intent == "recording_only",
              "a remote start uses its declared intent");
        check(RecordingContextsForRecordStart(nullptr).default_context->recording_intent == "stimulus_experiment",
              "a panel start keeps the saved intent");
        json confirmation;
        const auto declared = SubjectReferencesForRecordStart(&confirmation, serials, &remote);
        check(!declared.configured(), "the blank declaration, not the saved dish, is used");
        check(confirmation["schema_version"] == 2 && confirmation["confirmed_by"] == "local_control",
              "a remote confirmation says it came over local control");
        check(confirmation["cameras"]["2010093"]["dish_id"].is_null() &&
                  confirmation["cameras"]["2010094"]["subject_count"].is_null(),
              "every camera is declared blank");
        check(confirmation["declaration"]["declared_by"] == "delahantyj" &&
                  confirmation["requester"]["peer"]["user"] == "jeremy",
              "the claimed declarer and the actual requester are both kept");
        std::cout << "GUI context selection, saved reload and per-arm confirmation tests passed\n";
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; result = 1; }
    std::error_code error; std::filesystem::remove_all(root, error);
    return result;
}
