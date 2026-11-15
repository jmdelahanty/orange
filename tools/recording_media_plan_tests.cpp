#include "gui_recording_evidence.h"
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
using namespace orange::recording;
using json = nlohmann::json;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void refuses(F f) {
    try { f(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid media selection was accepted");
}
RecordingMediaSelection select(const char* mode) {
    return RecordingMediaSelection::Parse({{"schema_version", 1}, {"mode", mode}});
}
}
int main() {
    std::string temporary = "/tmp/orange_media_plan_XXXXXX";
    if (!mkdtemp(temporary.data())) return 1;
    const std::filesystem::path root = temporary;
    int result = 0;
    try {
        const std::vector<RecordingMediaCameraInput> cameras = {{"2010093", true, false}, {"2010094", true, true}, {"unused", false, true}};
        const auto previous = RecordingMediaPlan::Resolve({}, cameras);
        check(previous.cameras.size() == 2 && previous.Find("2010093")->FullFrame() &&
              !previous.Find("2010093")->MovingCrops() && previous.Find("2010094")->MovingCrops(), "existing choices changed");
        const auto full = select("full_frame"), both = select("full_frame_and_moving_crops"),
                   crop = select("registered_context_and_moving_crops");
        RequireCropOnlyRecordingInputs(full, false, false, false, false);
        RequireCropOnlyRecordingInputs(both, false, false, false, false);
        RequireCropOnlyRecordingInputs(crop, true, true, true, true);
        for (int missing = 0; missing < 4; ++missing)
            refuses([&] { RequireCropOnlyRecordingInputs(crop, missing != 0, missing != 1, missing != 2, missing != 3); });
        for (const auto& selection : {full, both, crop}) {
            check(RecordingMediaSelection::Parse(selection.ToJson()).ToJson() == selection.ToJson(), "selection round trip");
            const auto plan = RecordingMediaPlan::Resolve(selection, cameras);
            check(plan.cameras.size() == 2 && !plan.Find("unused"), "session membership changed");
            check(plan.HasFullFrame() == selection.FullFrame() && plan.HasMovingCrops() == selection.MovingCrops(false), "output owners differ from plan");
            check(plan.RequiresContext() == selection.RequiresContext(), "context requirement");
        }
        const auto crop_plan = RecordingMediaPlan::Resolve(crop, cameras);
        check(!crop_plan.HasFullFrame() && crop_plan.HasMovingCrops() && crop_plan.RequiresContext(), "crop-only plans full-frame work");
        check(crop_plan.ToJson().at("cameras").at(0).at("master_frame_record_required") == true, "missing master requirement");
        for (const json& bad : std::vector<json>{json::object(), json::array(), true,
            {{"schema_version", true}, {"mode", "full_frame"}},
            {{"schema_version", 1.0}, {"mode", "full_frame"}},
            {{"schema_version", 2}, {"mode", "full_frame"}},
            {{"schema_version", 1}, {"mode", "crop_only"}},
            {{"schema_version", 1}, {"mode", "full_frame"}, {"disable_full_frame", true}},
            {{"schema_version", 1}, {"mode", "FULL_FRAME"}}})
            refuses([&] { RecordingMediaSelection::Parse(bad); });
        refuses([&] { RecordingMediaPlan::Resolve(full, {{"", true, false}}); });
        refuses([&] { RecordingMediaPlan::Resolve(full, {{"a", true, false}, {"a", true, false}}); });
        check(RecordingMediaPlan::Resolve(full, {}).cameras.empty(), "preview-only membership invented");

        // The camera configuration carries the choice (recording.media_products
        // in each camera JSON); the streaming cameras must agree.
        using Cameras = std::vector<CameraConfigMediaProducts>;
        check(!ResolveCameraConfigMediaSelection({}).mode, "no cameras opted in");
        check(!ResolveCameraConfigMediaSelection(Cameras{{"1", std::nullopt}, {"2", std::nullopt}}).mode,
              "cameras without the key opted in");
        check(ResolveCameraConfigMediaSelection(Cameras{{"1", both.ToJson().dump()}, {"2", both.ToJson().dump()}}).ToJson() ==
              both.ToJson(), "agreeing cameras lost their choice");
        for (const Cameras& bad : std::vector<Cameras>{
                {{"1", both.ToJson().dump()}, {"2", std::nullopt}},
                {{"1", both.ToJson().dump()}, {"2", full.ToJson().dump()}},
                {{"1", "null"}},
                {{"1", json({{"schema_version", 1}, {"mode", "croponly"}}).dump()}},
                {{"1", json({{"schema_version", true}, {"mode", "full_frame"}}).dump()}}})
            refuses([&] { ResolveCameraConfigMediaSelection(bad); });
        try {
            ResolveCameraConfigMediaSelection(Cameras{{"2010093", both.ToJson().dump()}, {"2010094", std::nullopt}});
        } catch (const std::exception& ex) {
            const std::string what = ex.what();
            check(what.find("2010093") != std::string::npos && what.find("2010094") != std::string::npos,
                  "refusal does not name the cameras");
        }

        // The app config no longer carries the choice: the old key is refused.
        const auto path = root / "app.json";
        const json original = {{"unrelated", 17}, {"recording", {{"sink_mode", "external_ipc"},
            {"registered_context_recording", {{"schema_version", 1}, {"enabled", false}}}}}};
        { std::ofstream out(path); out << original; }
        RefuseAppConfigMediaProducts(path);
        SaveGuiRecordingEvidenceConfig(path, {});
        json saved; { std::ifstream in(path); in >> saved; }
        check(saved.at("unrelated") == 17 && saved.at("recording").at("sink_mode") == "external_ipc", "save altered encoder or unrelated config");
        check(!saved.at("recording").contains("media_products"), "context save wrote media_products");
        { std::ofstream out(path); out << json{{"recording", {{"media_products", both.ToJson()}}}}; }
        refuses([&] { RefuseAppConfigMediaProducts(path); });
        { std::ofstream out(path); out << json{{"recording", {{"media_products", nullptr}}}}; }
        refuses([&] { RefuseAppConfigMediaProducts(path); });
        const auto link = root / "linked.json";
        std::filesystem::create_symlink(path, link);
        refuses([&] { SaveGuiRecordingEvidenceConfig(link, {}); });
        check(std::filesystem::is_symlink(link), "save replaced user symlink");
        std::cout << "Recording media plan, construction selection, admission and persistence tests passed\n";
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; result = 1; }
    std::error_code error;
    std::filesystem::remove_all(root, error); // unique test-owned directory
    return result;
}
