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
        LoadRegisteredContextRecordingSettings(path);
        check(!RecordingMediaSelectionForStream().mode, "old GUI configuration opted into a product plan");
        check(!RegisteredContextRecordingConfigForArm().enabled, "missing config is not opt-in");
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
        orange::recording::SaveGuiRecordingMediaSelection(path, media);
        orange::recording::SaveGuiRecordingEvidenceConfig(path, config);
        ConsumeRegisteredContextRecordingConfirmation();
        refuses([&] { RegisteredContextRecordingConfigForArm(); });
        LoadRegisteredContextRecordingSettings(path);
        check(RecordingMediaSelectionForStream().ToJson() == media.ToJson(), "GUI did not restore explicit media choice");
        orange::recording::SaveGuiRecordingMediaSelection(path, {});
        LoadRegisteredContextRecordingSettings(path, false);
        check(RecordingMediaSelectionForStream().ToJson() == media.ToJson(), "context reload changed frozen media choice");
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
        std::cout << "GUI context selection, saved reload and per-arm confirmation tests passed\n";
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; result = 1; }
    std::error_code error; std::filesystem::remove_all(root, error);
    return result;
}
