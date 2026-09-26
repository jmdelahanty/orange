#include "recording_context.h"
#include <functional>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {
int g_failures = 0;
#define EXPECT(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_failures; } } while (0)
using nlohmann::json;
using orange::recording::RecordingContext;
using orange::recording::RecordingContextsConfig;

bool throws(const std::function<void()>& fn, const char* expect_substring = nullptr)
{
    try { fn(); return false; }
    catch (const std::exception& ex) {
        if (expect_substring && std::string(ex.what()).find(expect_substring) == std::string::npos) {
            std::fprintf(stderr, "  unexpected error text: %s\n", ex.what());
            return false;
        }
        return true;
    }
}

json entry(const char* type = "behavior", const char* subtype = "dish_freeswim", const char* mode = "free",
           const char* intent = "recording_only", const char* origin = "acquired")
{
    return {{"recording_type", type}, {"recording_subtype", subtype}, {"behavior_mode", mode},
            {"recording_intent", intent}, {"data_origin", origin}};
}

struct TempFolder {
    std::filesystem::path path;
    TempFolder() {
        std::string t = (std::filesystem::temp_directory_path() / "orange_recording_context_XXXXXX").string();
        if (!mkdtemp(t.data())) std::abort();
        path = t;
    }
    ~TempFolder() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    void write(const char* name, const json& j) const { std::ofstream out(path / name); out << j.dump(2); }
};

void test_parse_and_emit()
{
    const auto c = RecordingContext::Parse(entry());
    EXPECT(c.recording_type == "behavior" && c.recording_intent == "recording_only");
    const json emitted = c.ToEmittedJson();
    EXPECT(emitted.size() == 7);
    EXPECT(emitted.at("schema_id") == "citrus.parent_recording_context" && emitted.at("schema_version") == 1);
    EXPECT(RecordingContext::ParseEmitted(emitted) == c);
    // round trip through the config form
    EXPECT(RecordingContext::Parse(c.ToJson()) == c);
}

void test_parse_rejections()
{
    EXPECT(throws([] { RecordingContext::Parse(entry("", "x")); }, "must not be empty"));
    EXPECT(throws([] { RecordingContext::Parse(entry(" behavior", "x")); }, "surrounding whitespace"));
    EXPECT(throws([] { RecordingContext::Parse(entry("beha\tvior", "x")); }, "control character"));
    EXPECT(throws([] { RecordingContext::Parse(entry(std::string(1025, 'a').c_str(), "x")); }, "1024"));
    // The budget is 1024 UTF-8 bytes, not characters: 342 three-byte code points = 1026 bytes.
    {
        std::string wide; for (int i = 0; i < 342; ++i) wide += "\xE2\x82\xAC";
        EXPECT(throws([&] { RecordingContext::Parse(entry(wide.c_str(), "x")); }, "1024"));
        std::string fits; for (int i = 0; i < 341; ++i) fits += "\xE2\x82\xAC";
        EXPECT(!throws([&] { RecordingContext::Parse(entry(fits.c_str(), "x")); }));
    }
    // Unicode boundary whitespace (NBSP, ideographic space, BOM) and C1 controls are rejected;
    // interior non-ASCII is fine; malformed UTF-8 is rejected rather than passed through.
    EXPECT(throws([] { RecordingContext::Parse(entry("\xC2\xA0" "behavior", "x")); }, "surrounding whitespace"));
    EXPECT(throws([] { RecordingContext::Parse(entry("behavior\xE3\x80\x80", "x")); }, "surrounding whitespace"));
    EXPECT(throws([] { RecordingContext::Parse(entry("\xEF\xBB\xBF" "behavior", "x")); }, "surrounding whitespace"));
    EXPECT(throws([] { RecordingContext::Parse(entry("beha\xC2\x85vior", "x")); }, "control character"));
    EXPECT(throws([] { RecordingContext::Parse(entry("beha\x7Fvior", "x")); }, "control character"));
    EXPECT(throws([] { RecordingContext::Parse(entry("beha\xFFvior", "x")); }, "not valid UTF-8"));
    EXPECT(throws([] { RecordingContext::Parse(entry("beha\xC0\xAFvior", "x")); }, "not valid UTF-8"));
    EXPECT(!throws([] { RecordingContext::Parse(entry("caf\xC3\xA9 stimulus", "x")); }));
    EXPECT(!throws([] { RecordingContext::Parse(entry("stimulus", "dish_stimulus")); }));
    EXPECT(throws([] { RecordingContext::Parse(entry("b", "s", "sideways")); }, "behavior_mode"));
    EXPECT(throws([] { RecordingContext::Parse(entry("b", "s", "free", "maybe")); }, "recording_intent"));
    EXPECT(throws([] { RecordingContext::Parse(entry("b", "s", "free", "recording_only", "real")); }, "data_origin"));
    json extra = entry(); extra["notes"] = "x";
    EXPECT(throws([&] { RecordingContext::Parse(extra); }, "unknown"));
    json missing = entry(); missing.erase("data_origin");
    EXPECT(throws([&] { RecordingContext::Parse(missing); }, "missing"));
    // emitted form is closed too
    json e = RecordingContext::Parse(entry()).ToEmittedJson(); e["extra"] = 1;
    EXPECT(throws([&] { RecordingContext::ParseEmitted(e); }, "unknown"));
    json wrong_version = RecordingContext::Parse(entry()).ToEmittedJson(); wrong_version["schema_version"] = 3;
    EXPECT(throws([&] { RecordingContext::ParseEmitted(wrong_version); }, "schema_version"));
    // config form must not carry schema fields
    EXPECT(throws([] { RecordingContext::Parse(RecordingContext::Parse(entry()).ToEmittedJson()); }, "unknown"));
}

void test_optional_subtype_contract_revision()
{
    // Version 2: recording_subtype may be omitted (not specified). Omission is
    // explicit: null and "" are refused; nothing is substituted; an entry with
    // a subtype still emits version 1 byte-identically.
    json no_subtype = entry(); no_subtype.erase("recording_subtype");
    const auto c = RecordingContext::Parse(no_subtype);
    EXPECT(!c.recording_subtype.has_value());
    EXPECT(c.emitted_schema_version() == 2);
    const json emitted = c.ToEmittedJson();
    EXPECT(emitted.size() == 6 && !emitted.contains("recording_subtype") && emitted.at("schema_version") == 2);
    EXPECT(RecordingContext::ParseEmitted(emitted) == c);
    EXPECT(!c.ToJson().contains("recording_subtype"));
    EXPECT(RecordingContext::Parse(c.ToJson()) == c);
    // with a subtype: unchanged v1 emission
    const auto with = RecordingContext::Parse(entry());
    EXPECT(with.emitted_schema_version() == 1 && with.ToEmittedJson().at("schema_version") == 1 &&
           with.ToEmittedJson().size() == 7);
    EXPECT(!(with == c));
    // null / "" are not omission
    json null_subtype = entry(); null_subtype["recording_subtype"] = nullptr;
    EXPECT(throws([&] { RecordingContext::Parse(null_subtype); }, "omit the key"));
    json empty_subtype = entry(); empty_subtype["recording_subtype"] = "";
    EXPECT(throws([&] { RecordingContext::Parse(empty_subtype); }, "omit the key"));
    // v1 emitted form still requires the subtype; v2 with a subtype is accepted and preserved
    json v1_missing = with.ToEmittedJson(); v1_missing.erase("recording_subtype");
    EXPECT(throws([&] { RecordingContext::ParseEmitted(v1_missing); }, "schema_version 1"));
    json v2_with = with.ToEmittedJson(); v2_with["schema_version"] = 2;
    EXPECT(RecordingContext::ParseEmitted(v2_with) == with);
    json v2_empty = emitted; v2_empty["recording_subtype"] = "";
    EXPECT(throws([&] { RecordingContext::ParseEmitted(v2_empty); }, "omit the key"));
    // mixed block round trip: one camera with, one without
    std::map<std::string, RecordingContext> block = {{"2010093", with}, {"2010094", c}};
    const json block_json = orange::recording::EmittedRecordingContextsJson(block);
    EXPECT(block_json.at("2010093").at("schema_version") == 1 && block_json.at("2010094").at("schema_version") == 2);
    EXPECT(orange::recording::ParseEmittedRecordingContexts(block_json) == block);
}

void test_config_resolution()
{
    const json cfg = {{"schema_version", 1}, {"default", entry()},
                      {"cameras", {{"2010094", entry("behavior", "dish_stimulus", "free", "stimulus_experiment")}}}};
    const auto c = RecordingContextsConfig::Parse(cfg);
    EXPECT(RecordingContextsConfig::Parse(c.ToJson()).ToJson() == c.ToJson());
    const auto resolved = c.Resolve({"2010093", "2010094"});
    EXPECT(resolved.size() == 2);
    EXPECT(resolved.at("2010093").recording_intent == "recording_only");
    EXPECT(resolved.at("2010094").recording_intent == "stimulus_experiment");
    // configured but not recording serials are ignored; unknown serial without default refuses
    const auto only_cameras = RecordingContextsConfig::Parse({{"schema_version", 1}, {"cameras", {{"2010094", entry()}}}});
    EXPECT(throws([&] { only_cameras.Resolve({"2010093"}); }, "no context entry"));
    EXPECT(only_cameras.Resolve({"2010094"}).size() == 1);
    EXPECT(throws([&] { c.Resolve({}); }, "no recording cameras"));
    EXPECT(throws([&] { c.Resolve({"2010093", "2010093"}); }, "duplicate"));
    EXPECT(throws([] { RecordingContextsConfig::Parse({{"schema_version", 1}}); }, "neither"));
    EXPECT(throws([] { RecordingContextsConfig::Parse({{"schema_version", 2}, {"default", entry()}}); }, "schema_version"));
    EXPECT(throws([] { RecordingContextsConfig::Parse({{"schema_version", 1}, {"default", entry()}, {"mode", "x"}}); }, "unknown"));
    EXPECT(!RecordingContextsConfig{}.configured());
}

void test_gate_copies_frozen_block_and_checks_membership()
{
    TempFolder folder;
    const auto resolved = RecordingContextsConfig::Parse({{"schema_version", 1}, {"default", entry()}}).Resolve({"2010093", "2010094"});
    const json frozen = orange::recording::EmittedRecordingContextsJson(resolved);
    folder.write("recording_snapshot_start.json", {{"session", {{"recording_contexts", frozen}}}});
    folder.write("recording_snapshot.json", {{"session", {{"recording_contexts", json::object()}}}});  // sealed copy wins

    json manifest = {{"schema_id", "orange.recording_session"}, {"cameras", json::array({"2010093", "2010094"})}};
    orange::recording::ApplyRecordingContextsGate(folder.path.string(), &manifest);
    EXPECT(manifest.at("recording_contexts") == frozen);
    // idempotent on a manifest that already carries the same block
    orange::recording::ApplyRecordingContextsGate(folder.path.string(), &manifest);
    EXPECT(manifest.at("recording_contexts") == frozen);
    // differing existing block refused
    json tampered = manifest; tampered["recording_contexts"]["2010093"]["recording_intent"] = "stimulus_experiment";
    EXPECT(throws([&] { orange::recording::ApplyRecordingContextsGate(folder.path.string(), &tampered); }, "differ"));
    // membership mismatch refused (extra camera in manifest, missing camera in manifest)
    json extra = {{"cameras", json::array({"2010093", "2010094", "2010095"})}};
    EXPECT(throws([&] { orange::recording::ApplyRecordingContextsGate(folder.path.string(), &extra); }, "membership"));
    json fewer = {{"cameras", json::array({"2010093"})}};
    EXPECT(throws([&] { orange::recording::ApplyRecordingContextsGate(folder.path.string(), &fewer); }, "membership"));
    // clip manifests (no cameras array) are untouched
    json clip = {{"schema_id", "orange.recording_clip"}, {"clip_index", 0}};
    orange::recording::ApplyRecordingContextsGate(folder.path.string(), &clip);
    EXPECT(!clip.contains("recording_contexts"));
}

void test_gate_without_frozen_block()
{
    TempFolder folder;
    folder.write("recording_snapshot_start.json", {{"session", {{"recording_media_plan", json::object()}}}});
    json manifest = {{"cameras", json::array({"2010093"})}};
    orange::recording::ApplyRecordingContextsGate(folder.path.string(), &manifest);
    EXPECT(!manifest.contains("recording_contexts"));
    json invented = {{"cameras", json::array({"2010093"})}, {"recording_contexts", json::object()}};
    EXPECT(throws([&] { orange::recording::ApplyRecordingContextsGate(folder.path.string(), &invented); }, "froze none"));
    // corrupt frozen block is an error, not silently ignored
    folder.write("recording_snapshot_start.json", {{"session", {{"recording_contexts", {{"2010093", {{"schema_id", "x"}}}}}}}});
    EXPECT(throws([&] { orange::recording::ApplyRecordingContextsGate(folder.path.string(), &manifest); }, "frozen recording_contexts"));
}

void test_intent_binding_mode_coupling()
{
    TempFolder folder;
    auto freeze = [&](const char* intent) {
        const auto r = RecordingContextsConfig::Parse({{"schema_version", 1}, {"default", entry("b", "s", "free", intent)}}).Resolve({"2010093"});
        folder.write("recording_snapshot_start.json", {{"session", {{"recording_contexts", orange::recording::EmittedRecordingContextsJson(r)}}}});
    };
    std::string error;
    // no frozen block: unchanged
    EXPECT(orange::recording::ApplyRecordingIntentToBindingMode(folder.path.string(), "optional", &error) == "optional" && error.empty());
    freeze("recording_only");
    EXPECT(orange::recording::ApplyRecordingIntentToBindingMode(folder.path.string(), "optional", &error) == "not_applicable");
    EXPECT(orange::recording::ApplyRecordingIntentToBindingMode(folder.path.string(), "not_applicable", &error) == "not_applicable");
    error.clear();
    EXPECT(orange::recording::ApplyRecordingIntentToBindingMode(folder.path.string(), "required", &error).empty());
    EXPECT(error.find("recording_only") != std::string::npos);
    freeze("stimulus_experiment");
    error.clear();
    EXPECT(orange::recording::ApplyRecordingIntentToBindingMode(folder.path.string(), "required", &error) == "required" && error.empty());
    EXPECT(orange::recording::ApplyRecordingIntentToBindingMode(folder.path.string(), "optional", &error) == "optional");
}

}  // namespace

int main()
{
    test_parse_and_emit();
    test_parse_rejections();
    test_optional_subtype_contract_revision();
    test_config_resolution();
    test_gate_copies_frozen_block_and_checks_membership();
    test_gate_without_frozen_block();
    test_intent_binding_mode_coupling();
    if (g_failures) { std::fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    std::printf("recording_context_tests: all passed\n");
    return 0;
}
