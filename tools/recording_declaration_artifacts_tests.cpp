// session::WriteRecordingDeclarationArtifacts on a temporary folder: the
// declaration files a recording gets at record start, written exactly as the
// GUI and orange_client write them, without a camera.

#include "session/recording_declaration_artifacts.h"
#include "session/recording_start_declaration.h"
#include "outputs/common/digest.h"

#include <cstdio>
#include <functional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace orange::session;

namespace {

int g_failures = 0;
#define EXPECT(cond, msg)                                                            \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "[FAIL] %s:%d %s\n", __FILE__, __LINE__, (msg));     \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

// Stands in for project.cpp's snapshot updater: merges into session{}.
SnapshotSessionUpdate fake_snapshot_update()
{
    return [](const std::string& folder, const json& session) {
        const fs::path path = fs::path(folder) / "recording_snapshot.json";
        json snapshot = fs::exists(path) ? json::parse(read_file(path)) : json{{"session", json::object()}};
        for (auto it = session.begin(); it != session.end(); ++it) snapshot["session"][it.key()] = it.value();
        std::ofstream(path) << snapshot.dump();
        return true;
    };
}

RecordingDeclarationArtifacts base_artifacts()
{
    RecordingDeclarationArtifacts a;
    a.recording_serials = {"2010093", "2010094"};
    a.subject_references = orange::recording::SubjectReferencesConfig::Parse({{"schema_version", 1}});
    return a;
}

void test_spec_declared_run_writes_all_three(const fs::path& dir)
{
    const fs::path folder = dir / "spec_run";
    fs::create_directories(folder);
    RecordingDeclarationArtifacts a = base_artifacts();
    // Deliberately not canonical JSON: the copy must be the bytes, not a re-dump.
    a.spec_bytes = "{\n  \"experiment_id\": \"x\",   \"fixed\": {}\n}\n";
    a.spec_sha256 = orange::outputs::sha256_hex(a.spec_bytes);
    a.spec_copy_relative_path = "run_spec/" + a.spec_sha256 + ".json";
    const auto declaration = ParseRecordingStartDeclaration(
        {{"schema_id", "orange.recording_start_declaration"}, {"schema_version", 1},
         {"declared_by", "delahantyj"}, {"recording_intent", "recording_only"},
         {"subject_references", {{"schema_version", 1}}}});
    a.subject_confirmation = BuildSubjectConfirmation(
        a.subject_references, a.recording_serials, kConfirmedByRunSpec, declaration.ToJson(),
        {{"transport", "run_spec"}, {"spec_sha256", a.spec_sha256},
         {"spec_copy_relative_path", a.spec_copy_relative_path}},
        "2026-11-15T00:00:00Z");

    WriteRecordingDeclarationArtifacts(folder.string(), a, fake_snapshot_update(), "2026-11-15T00:00:00Z");

    const json snapshot = json::parse(read_file(folder / "recording_snapshot.json"));
    const json& refs = snapshot["session"]["subject_references"];
    EXPECT(refs.size() == 2 && refs["2010093"]["status"] == "not_collected",
           "one not_collected subject reference per recording camera");
    const json confirmation = json::parse(read_file(folder / "subject_confirmation.json"));
    EXPECT(confirmation["confirmed_by"] == "run_spec" && confirmation["schema_version"] == 2,
           "the confirmation is the run_spec v2 file");
    EXPECT(read_file(folder / "subject_confirmation.json") == a.subject_confirmation.dump(2) + "\n",
           "the confirmation bytes are the GUI's format (dump(2) + newline)");
    const fs::path copy = folder / a.spec_copy_relative_path;
    EXPECT(fs::exists(copy), "the spec copy exists at run_spec/<sha256>.json");
    EXPECT(read_file(copy) == a.spec_bytes, "the spec copy is the exact bytes, not re-serialized");
    EXPECT(orange::outputs::sha256_hex(read_file(copy)) == a.spec_sha256, "the copy hashes to spec_sha256");
}

void test_gui_start_writes_no_spec_copy(const fs::path& dir)
{
    const fs::path folder = dir / "gui_run";
    fs::create_directories(folder);
    RecordingDeclarationArtifacts a = base_artifacts();
    a.subject_confirmation = BuildSubjectConfirmation(a.subject_references, a.recording_serials,
                                                      kConfirmedByGuiOperator, nullptr,
                                                      {{"transport", "gui_panel"}, {"user", "jeremy"}},
                                                      "2026-11-15T00:00:00Z");
    WriteRecordingDeclarationArtifacts(folder.string(), a, fake_snapshot_update(), "2026-11-15T00:00:00Z");
    EXPECT(fs::exists(folder / "subject_confirmation.json"), "a confirmed GUI start writes its confirmation");
    EXPECT(!fs::exists(folder / "run_spec"), "a GUI start writes no spec copy");
}

void test_undeclared_run_writes_only_the_references(const fs::path& dir)
{
    const fs::path folder = dir / "plain_run";
    fs::create_directories(folder);
    WriteRecordingDeclarationArtifacts(folder.string(), base_artifacts(), fake_snapshot_update(),
                                       "2026-11-15T00:00:00Z");
    EXPECT(fs::exists(folder / "recording_snapshot.json"), "subject references are always frozen");
    EXPECT(!fs::exists(folder / "subject_confirmation.json"), "no declaration, no confirmation file");
}

void test_inconsistent_spec_copy_writes_nothing(const fs::path& dir)
{
    const fs::path folder = dir / "bad_copy";
    fs::create_directories(folder);
    RecordingDeclarationArtifacts a = base_artifacts();
    a.spec_bytes = "{}";
    a.spec_sha256 = std::string(64, 'a');
    a.spec_copy_relative_path = "run_spec/" + a.spec_sha256 + ".json";
    bool refused = false;
    try {
        WriteRecordingDeclarationArtifacts(folder.string(), a, fake_snapshot_update(), "t");
    } catch (const std::runtime_error&) {
        refused = true;
    }
    EXPECT(refused, "bytes that do not hash to spec_sha256 are refused");
    EXPECT(fs::is_empty(folder), "and nothing is written");

    a.spec_sha256 = orange::outputs::sha256_hex(a.spec_bytes);
    a.spec_copy_relative_path = "elsewhere/spec.json";
    refused = false;
    try {
        WriteRecordingDeclarationArtifacts(folder.string(), a, fake_snapshot_update(), "t");
    } catch (const std::runtime_error&) {
        refused = true;
    }
    EXPECT(refused && fs::is_empty(folder), "a copy path other than run_spec/<sha256>.json is refused");
}

json load_fixture_declaration()
{
    const fs::path dir = fs::path(ORANGE_SOURCE_DIR) / "tools/fixtures/declaration_v2/orchard_v2";
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().string().size() > 25 &&
            entry.path().string().rfind(".orange_declaration.json") == entry.path().string().size() - 24) {
            return json::parse(read_file(entry.path()));
        }
    }
    throw std::runtime_error("fixture declaration not found");
}

bool refuses(const std::function<void()>& f)
{
    try {
        f();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

void test_v2_routes_and_coverage()
{
    const auto plan = ParseRecordingStartDeclaration(load_fixture_declaration());
    EXPECT(plan.schema_version == 2 && plan.source_kind() == kSourceKindOrchardRunPlan, "the fixture is a v2 plan");
    EXPECT(plan.subject_references.cameras.at("2010096").blank, "the fixture states a blank camera");
    EXPECT(!refuses([&] { CheckDeclarationRoute(plan, kConfirmedByRunSpec); }), "a plan reaches Orange as a run spec");
    EXPECT(refuses([&] { CheckDeclarationRoute(plan, kConfirmedByLocalControl); }),
           "a plan over the socket is refused in the first build");
    EXPECT(refuses([&] { CheckDeclarationRoute(plan, kConfirmedByGuiOperator); }), "never from the panel");
    EXPECT(!refuses([&] { CheckDeclarationCoversCameras(plan, {"2010093", "2010094", "2010095", "2010096"}); }),
           "all four cameras listed");
    EXPECT(refuses([&] { CheckDeclarationCoversCameras(plan, {"2010093", "2010097"}); }),
           "a recording camera the plan leaves out is refused");
    EXPECT(DeclarationFileCopies(plan).size() == 3, "three run_plan copies");
    EXPECT(plan.ToJson() == load_fixture_declaration(), "v2 round-trips unchanged");

    // A plan session with no fish (e.g. a fish-free session before the real
    // one): recording_only, every camera blank, still plan-sourced.
    json fish_free = load_fixture_declaration();
    fish_free["recording_intent"] = "recording_only";
    for (auto& [serial, entry] : fish_free["subject_references"]["cameras"].items()) entry = {{"blank", true}};
    const auto fish_free_plan = ParseRecordingStartDeclaration(fish_free);
    EXPECT(fish_free_plan.schema_version == 2 && fish_free_plan.source_kind() == kSourceKindOrchardRunPlan &&
               fish_free_plan.recording_intent == "recording_only",
           "a recording_only, all-blank plan declaration is accepted");
    EXPECT(!refuses([&] { CheckDeclarationRoute(fish_free_plan, kConfirmedByRunSpec); }) &&
               !refuses([&] { CheckDeclarationCoversCameras(fish_free_plan, {"2010093", "2010094", "2010095", "2010096"}); }),
           "it reaches Orange as a run spec and covers every camera");
    EXPECT(DeclarationFileCopies(fish_free_plan).size() == 3 && fish_free_plan.ToJson() == fish_free,
           "it names the plan copies and round-trips unchanged");

    json op = load_fixture_declaration();
    op["source"] = {{"kind", "operator"}};
    for (auto& [serial, entry] : op["subject_references"]["cameras"].items()) entry = {{"blank", true}};
    const auto operator_decl = ParseRecordingStartDeclaration(op);
    EXPECT(!refuses([&] { CheckDeclarationRoute(operator_decl, kConfirmedByLocalControl); }), "operator over the socket");
    EXPECT(refuses([&] { CheckDeclarationRoute(operator_decl, kConfirmedByRunSpec); }), "operator is not a run spec");
    EXPECT(DeclarationFileCopies(operator_decl).empty(), "only a plan names copies");
    json op_extra = op;
    op_extra["source"]["repo"] = "a/b";
    EXPECT(refuses([&] { ParseRecordingStartDeclaration(op_extra); }), "operator source has no other keys");
    json v2_no_source = op;
    v2_no_source.erase("source");
    EXPECT(refuses([&] { ParseRecordingStartDeclaration(v2_no_source); }), "v2 requires source");
    json bad_ext = load_fixture_declaration();
    std::string path = bad_ext["source"]["plan_copy_relative_path"];
    bad_ext["source"]["plan_copy_relative_path"] = path.substr(0, path.size() - 5) + ".yml";
    EXPECT(refuses([&] { ParseRecordingStartDeclaration(bad_ext); }), "the plan copy is .yaml, exactly");
    json dotdot = load_fixture_declaration();
    dotdot["source"]["path"] = "specs/../secret.yaml";
    EXPECT(refuses([&] { ParseRecordingStartDeclaration(dotdot); }), "no .. in paths");

    const json v1 = {{"schema_id", "orange.recording_start_declaration"}, {"schema_version", 1},
                     {"declared_by", "delahantyj"}, {"recording_intent", "recording_only"},
                     {"subject_references", {{"schema_version", 1}}}};
    const auto v1_decl = ParseRecordingStartDeclaration(v1);
    EXPECT(v1_decl.ToJson() == v1, "v1 is unchanged byte for byte");
    EXPECT(!refuses([&] { CheckDeclarationRoute(v1_decl, kConfirmedByLocalControl); }) &&
               !refuses([&] { CheckDeclarationRoute(v1_decl, kConfirmedByRunSpec); }),
           "v1 has no source and any route, as before");
}

void test_writer_seals_run_plan_copies(const fs::path& dir)
{
    const fs::path folder = dir / "plan_run";
    fs::create_directories(folder);
    const auto plan = ParseRecordingStartDeclaration(load_fixture_declaration());
    const fs::path fixture_root = fs::path(ORANGE_SOURCE_DIR) / "tools/fixtures/declaration_v2/orchard_v2";
    RecordingDeclarationArtifacts a = base_artifacts();
    for (const auto& copy : DeclarationFileCopies(plan)) {
        a.declaration_files.push_back({copy.relative_path, read_file(fixture_root / copy.relative_path), copy.sha256});
    }
    WriteRecordingDeclarationArtifacts(folder.string(), a, fake_snapshot_update(), "t");
    for (const auto& copy : DeclarationFileCopies(plan)) {
        EXPECT(read_file(folder / copy.relative_path) == read_file(fixture_root / copy.relative_path),
               "each run_plan copy is sealed byte for byte");
    }

    const fs::path bad_folder = dir / "bad_plan_run";
    fs::create_directories(bad_folder);
    RecordingDeclarationArtifacts bad = base_artifacts();
    bad.declaration_files.push_back({"run_plan/" + std::string(64, 'a') + ".json", "{}", std::string(64, 'a')});
    EXPECT(refuses([&] { WriteRecordingDeclarationArtifacts(bad_folder.string(), bad, fake_snapshot_update(), "t"); }) &&
               fs::is_empty(bad_folder),
           "a copy not named by its bytes' sha256 is refused before any write");
}

}  // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / ("orange_declaration_artifacts_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    test_spec_declared_run_writes_all_three(dir);
    test_gui_start_writes_no_spec_copy(dir);
    test_undeclared_run_writes_only_the_references(dir);
    test_inconsistent_spec_copy_writes_nothing(dir);
    test_v2_routes_and_coverage();
    test_writer_seals_run_plan_copies(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures) {
        std::fprintf(stderr, "recording_declaration_artifacts_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("recording_declaration_artifacts_tests: PASS\n");
    return 0;
}
