// Unit tests for recording_subject_reference: config parsing, the builder
// against a fake HTTP function (collected, not declared, not configured,
// transport failure, 404 with detail.error, schema/identity mismatches, fish
// lookup outcomes), the emitted-block rules, the frozen-block gate, and (when
// a URL is given as argv[1]) the real HttpGet against a local server.
#include "recording_subject_reference.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unistd.h>

namespace {
int g_failures = 0;
#define EXPECT(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_failures; } } while (0)
using nlohmann::json;
using namespace orange::recording;

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

const ZebrobotLookupConfig kZb = ZebrobotLookupConfig::Parse({{"base_url", "http://zb.test/"}, {"timeout_ms", 500}});
const std::string kNow = "2026-10-05T20:00:00Z";

HttpGetResult ok(const json& body) { HttpGetResult r; r.transport_ok = true; r.status = 200; r.body = body.dump(); return r; }
HttpGetResult http(int status, const json& body) { HttpGetResult r; r.transport_ok = true; r.status = status; r.body = body.dump(); return r; }
HttpGetResult down(const std::string& why) { HttpGetResult r; r.transport_ok = false; r.error = why; return r; }

json served_dish(const char* id = "19220_1")
{
    return {{"schema_version", 2}, {"dish_id", id}, {"dish_uuid", "28c29cc6-a1ef-4382-9dc5-414fbb445d92"},
            {"revision", 1}, {"updated_at", "2026-10-02 16:28:20"}, {"genotype", "wt"}, {"dof", "2026-09-20"},
            {"fish_count", 12}, {"species", "zebrafish"}};
}
json served_fish(int n)
{
    json items = json::array();
    for (int i = 0; i < n; ++i)
        items.push_back({{"fish_id", "19220_1_f" + std::to_string(i + 1)}, {"dish_id", "19220_1"},
                         {"dish_uuid", "28c29cc6-a1ef-4382-9dc5-414fbb445d92"}, {"subject_label", "f" + std::to_string(i + 1)},
                         {"current_unit_id", nullptr}, {"revision", 3}, {"updated_at", "2026-10-03 09:00:00"}});
    return {{"items", items}};
}

// A fake endpoint map: url suffix -> result.
HttpGetFn fake(const std::map<std::string, HttpGetResult>& routes, std::vector<std::string>* calls = nullptr)
{
    return [routes, calls](const std::string& url, int timeout_ms) {
        EXPECT(timeout_ms == 500);
        if (calls) calls->push_back(url);
        for (const auto& [suffix, result] : routes)
            if (url.size() >= suffix.size() && url.compare(url.size() - suffix.size(), suffix.size(), suffix) == 0) return result;
        return down("no route for " + url);
    };
}

void test_configs()
{
    const auto zb = ZebrobotLookupConfig::Parse({{"base_url", "http://host:8000///"}});
    EXPECT(zb.base_url == "http://host:8000" && zb.timeout_ms == 2000 && zb.configured());
    EXPECT(!ZebrobotLookupConfig::Parse(json::object()).configured());
    EXPECT(throws([] { ZebrobotLookupConfig::Parse({{"base_url", "https://x"}}); }, "http://"));
    EXPECT(throws([] { ZebrobotLookupConfig::Parse({{"base_url", "http://x"}, {"timeout_ms", 50}}); }, "timeout_ms"));
    EXPECT(throws([] { ZebrobotLookupConfig::Parse({{"url", "http://x"}}); }, "unknown"));

    const auto cfg = SubjectReferencesConfig::Parse({{"schema_version", 1}, {"default", {{"dish_id", "19220_1"}, {"subject_count", 3}}},
                                                     {"cameras", {{"2010094", {{"dish_id", "19220_2"}}}, {"2010095", {{"subject_count", 1}}}}}});
    EXPECT(cfg.configured());
    const auto resolved = cfg.Resolve({"2010093", "2010094", "2010095", "2010096"});
    EXPECT(resolved.at("2010093").dish_id == "19220_1" && resolved.at("2010093").subject_count == 3);
    EXPECT(resolved.at("2010094").dish_id == "19220_2" && resolved.at("2010094").subject_count == 3);  // field-wise override
    EXPECT(resolved.at("2010095").dish_id == "19220_1" && resolved.at("2010095").subject_count == 1);
    EXPECT(resolved.at("2010096").dish_id == "19220_1" && resolved.at("2010096").subject_count == 3);
    EXPECT(SubjectReferencesConfig::Parse(cfg.ToJson()).ToJson() == cfg.ToJson());
    const auto only_cameras = SubjectReferencesConfig::Parse({{"schema_version", 1}, {"cameras", {{"2010094", {{"dish_id", "x"}}}}}});
    EXPECT(only_cameras.Resolve({"2010093"}).at("2010093").empty());
    EXPECT(throws([] { SubjectReferencesConfig::Parse({{"schema_version", 1}, {"default", {{"subject_count", 0}}}}); }, "subject_count"));
    EXPECT(throws([] { SubjectReferencesConfig::Parse({{"schema_version", 1}, {"default", json::object()}}); }, "must declare"));
    EXPECT(!SubjectReferencesConfig::Parse({{"schema_version", 1}}).configured());
    EXPECT(throws([] { SubjectReferencesConfig::Parse({{"schema_version", 1}, {"default", {{"dish_id", ""}}}}); }, "empty"));
    EXPECT(throws([] { SubjectReferencesConfig::Parse({{"schema_version", 1}, {"default", {{"dish_id", "a b"}}}}); }, "whitespace"));
    EXPECT(throws([] { SubjectReferencesConfig::Parse({{"schema_version", 1}, {"default", {{"dish_id", "a/b"}}}}); }, "delimiter"));
    EXPECT(throws([] { SubjectReferencesConfig::Parse({{"schema_version", 1}, {"default", nullptr}}); }, "null"));
    EXPECT(throws([] { SubjectReferencesConfig::Parse({{"schema_version", 2}}); }, "schema_version"));
}

void test_builder_outcomes()
{
    const SubjectDeclaration none;
    SubjectDeclaration dish; dish.dish_id = "19220_1";
    SubjectDeclaration dish_counted = dish; dish_counted.subject_count = 2;
    SubjectDeclaration count_only; count_only.subject_count = 4;
    SubjectDeclaration nope; nope.dish_id = "nope";
    // Not declared.
    const json nd = BuildSubjectReference(none, kZb, kNow, fake({}));
    EXPECT(nd.at("status") == "not_collected" && nd.at("reason") == "no_dish_declared" && nd.at("zebrobot").is_null() &&
           nd.at("dish").is_null() && nd.at("dish_fish").empty() && nd.at("dish_fish_lookup").is_null() && nd.at("subject_count").is_null());
    // A count without a dish is still a declaration (independent of status).
    const json co = BuildSubjectReference(count_only, kZb, kNow, fake({}));
    EXPECT(co.at("status") == "not_collected" && co.at("subject_count") == 4);
    // Declared but no base_url: declared failure, never a refusal.
    const json nc = BuildSubjectReference(dish, ZebrobotLookupConfig{}, kNow, fake({}));
    EXPECT(nc.at("status") == "lookup_failed" && nc.at("reason") == "zebrobot_not_configured" &&
           nc.at("zebrobot").at("error").at("kind") == "transport" && nc.at("zebrobot").at("http_status").is_null());
    // Collected with two registered fish; biological fields are not copied.
    std::vector<std::string> calls;
    const json c = BuildSubjectReference(dish_counted, kZb, kNow,
                                         fake({{"/dishes/19220_1/citrus-snapshot", ok(served_dish())},
                                               {"/dishes/19220_1/fish", ok(served_fish(2))}}, &calls));
    EXPECT(c.at("subject_count") == 2);  // declared, never derived from fish_count (12) or dish_fish (2)
    EXPECT(calls.size() == 2 && calls[0] == "http://zb.test/dishes/19220_1/citrus-snapshot" && calls[1] == "http://zb.test/dishes/19220_1/fish");
    EXPECT(c.at("status") == "collected" && c.at("reason") == "" && c.at("zebrobot").at("http_status") == 200 &&
           c.at("zebrobot").at("error").is_null());
    EXPECT(c.at("zebrobot").at("base_url") == "http://zb.test" && c.at("zebrobot").at("endpoint") == "/dishes/19220_1/citrus-snapshot" &&
           c.at("zebrobot").at("fish_endpoint") == "/dishes/19220_1/fish" && c.at("zebrobot").at("api_schema_version") == 2 &&
           c.at("zebrobot").at("queried_at_utc") == kNow);
    EXPECT(c.at("dish") == json({{"dish_id", "19220_1"}, {"dish_uuid", "28c29cc6-a1ef-4382-9dc5-414fbb445d92"}, {"revision", 1},
                                 {"updated_at", "2026-10-02 16:28:20"}}));
    EXPECT(!c.at("dish").contains("genotype") && c.dump().find("genotype") == std::string::npos && c.dump().find("dof") == std::string::npos);
    EXPECT(c.at("dish_fish").size() == 2 && c.at("dish_fish")[0] == json({{"fish_id", "19220_1_f1"}, {"revision", 3}, {"updated_at", "2026-10-03 09:00:00"}}));
    EXPECT(c.at("dish_fish_lookup") == json({{"status", "complete"}, {"http_status", 200}, {"error", nullptr}}));
    // Collected, empty registered-fish list is a completed lookup, not a failure.
    const json e = BuildSubjectReference(dish, kZb, kNow,
                                         fake({{"/citrus-snapshot", ok(served_dish())}, {"/fish", ok(served_fish(0))}}));
    EXPECT(e.at("status") == "collected" && e.at("dish_fish").empty() && e.at("dish_fish_lookup").at("status") == "complete");
    // Collected, fish lookup failed: never read as "no individuals".
    const json ff = BuildSubjectReference(dish, kZb, kNow,
                                          fake({{"/citrus-snapshot", ok(served_dish())}, {"/fish", http(503, {{"detail", {{"error", "database_error"}}}})}}));
    EXPECT(ff.at("status") == "collected" && ff.at("dish_fish").empty() && ff.at("dish_fish_lookup").at("status") == "failed" &&
           ff.at("dish_fish_lookup").at("http_status") == 503 && ff.at("dish_fish_lookup").at("error").at("detail_error") == "database_error");
    const json ft = BuildSubjectReference(dish, kZb, kNow,
                                          fake({{"/citrus-snapshot", ok(served_dish())}, {"/fish", down("receive timed out")}}));
    EXPECT(ft.at("status") == "collected" && ft.at("dish_fish_lookup").at("status") == "failed" &&
           ft.at("dish_fish_lookup").at("error").at("kind") == "transport" && ft.at("dish_fish_lookup").at("http_status").is_null());
    // Dish 404 with the structured body; fish never attempted.
    const json nf = BuildSubjectReference(nope, kZb, kNow,
                                          fake({{"/dishes/nope/citrus-snapshot", http(404, {{"detail", {{"error", "dish_not_found"}, {"dish_id", "nope"}}}})}}));
    EXPECT(nf.at("status") == "lookup_failed" && nf.at("reason") == "dish_lookup_http_404" && nf.at("zebrobot").at("http_status") == 404 &&
           nf.at("zebrobot").at("error") == json({{"kind", "http"}, {"detail_error", "dish_not_found"}, {"message", "HTTP 404: dish_not_found"}}) &&
           nf.at("dish").is_null() && nf.at("dish_fish_lookup").at("status") == "not_attempted");
    // Transport failure on the dish lookup.
    const json tf = BuildSubjectReference(dish, kZb, kNow, fake({{"/citrus-snapshot", down("connect timed out")}}));
    EXPECT(tf.at("status") == "lookup_failed" && tf.at("reason") == "dish_lookup_transport_failure" &&
           tf.at("zebrobot").at("http_status").is_null() && tf.at("zebrobot").at("error").at("message") == "connect timed out");
    // API schema mismatch, missing field, and identity mismatch are declared failures, never substitutions.
    json v1 = served_dish(); v1["schema_version"] = 1;
    EXPECT(BuildSubjectReference(dish, kZb, kNow, fake({{"/citrus-snapshot", ok(v1)}})).at("reason") == "dish_lookup_api_schema_mismatch");
    json nouuid = served_dish(); nouuid.erase("dish_uuid");
    EXPECT(BuildSubjectReference(dish, kZb, kNow, fake({{"/citrus-snapshot", ok(nouuid)}})).at("reason") == "dish_lookup_missing_dish_uuid");
    EXPECT(BuildSubjectReference(dish, kZb, kNow, fake({{"/citrus-snapshot", ok(served_dish("other"))}})).at("reason") == "dish_lookup_identity_mismatch");
    // Every outcome validates under the emitted rules.
    for (const json& entry : {nd, co, nc, c, e, ff, ft, nf, tf}) EXPECT(!throws([&] { ValidateEmittedSubjectReferences({{"2010093", entry}}); }));
}

void test_block_rules_and_gate()
{
    SubjectDeclaration dish; dish.dish_id = "19220_1";
    const json block = BuildSubjectReferences({{"2010093", dish}, {"2010094", SubjectDeclaration{}}}, kZb, kNow,
                                              fake({{"/citrus-snapshot", ok(served_dish())}, {"/fish", ok(served_fish(1))}}));
    EXPECT(block.size() == 2 && block.at("2010093").at("status") == "collected" && block.at("2010094").at("status") == "not_collected");
    // Rule violations.
    json bad = block; bad["2010094"]["dish"] = block["2010093"]["dish"];
    EXPECT(throws([&] { ValidateEmittedSubjectReferences(bad); }, "not_collected must carry no"));
    bad = block; bad["2010093"]["reason"] = "x";
    EXPECT(throws([&] { ValidateEmittedSubjectReferences(bad); }, "collected must have an empty reason"));
    bad = block; bad["2010093"]["dish"]["genotype"] = "wt";
    EXPECT(throws([&] { ValidateEmittedSubjectReferences(bad); }, "unknown"));
    bad = block; bad["2010093"]["dish_fish_lookup"]["status"] = "failed"; bad["2010093"]["dish_fish_lookup"]["error"] = nullptr;
    EXPECT(throws([&] { ValidateEmittedSubjectReferences(bad); }));
    bad = block; bad["2010093"]["dish_fish_lookup"] = {{"status", "not_attempted"}, {"http_status", nullptr}, {"error", nullptr}};
    EXPECT(throws([&] { ValidateEmittedSubjectReferences(bad); }, "dish_fish must be empty"));
    bad = block; bad["2010093"]["subject_count"] = 0;
    EXPECT(throws([&] { ValidateEmittedSubjectReferences(bad); }, "subject_count"));
    bad = block; bad["2010093"].erase("subject_count");
    EXPECT(throws([&] { ValidateEmittedSubjectReferences(bad); }, "missing subject_count"));
    bad = block; bad[""] = block["2010094"];
    EXPECT(throws([&] { ValidateEmittedSubjectReferences(bad); }, "empty camera serial"));
    EXPECT(throws([] { ValidateEmittedSubjectReferences(json::object()); }, "non-empty"));
    EXPECT(throws([] { NotCollectedSubjectReference(""); }, "reason"));

    // Gate: frozen block copied verbatim; membership and difference refusals; absent block leaves the manifest alone.
    std::string temp = (std::filesystem::temp_directory_path() / "orange_subject_refs_XXXXXX").string();
    EXPECT(mkdtemp(temp.data()) != nullptr);
    const std::filesystem::path folder(temp);
    { std::ofstream out(folder / "recording_snapshot_start.json"); out << json{{"session", {{"subject_references", block}}}}.dump(); }
    json manifest = {{"cameras", {"2010093", "2010094"}}};
    ApplySubjectReferencesGate(folder.string(), &manifest);
    EXPECT(manifest.at("subject_references") == block);
    ApplySubjectReferencesGate(folder.string(), &manifest);  // refresh keeps it identical
    EXPECT(manifest.at("subject_references") == block);
    json differing = manifest; differing["subject_references"]["2010094"]["reason"] = "other";
    EXPECT(throws([&] { ApplySubjectReferencesGate(folder.string(), &differing); }, "differs"));
    json wrong_cameras = {{"cameras", {"2010093"}}};
    EXPECT(throws([&] { ApplySubjectReferencesGate(folder.string(), &wrong_cameras); }, "membership"));
    json clip = {{"schema_id", "orange.recording_clip"}};
    ApplySubjectReferencesGate(folder.string(), &clip);
    EXPECT(!clip.contains("subject_references"));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    { std::ofstream out(folder / "recording_snapshot_start.json"); out << json{{"session", json::object()}}.dump(); }
    json none = {{"cameras", {"2010093"}}};
    ApplySubjectReferencesGate(folder.string(), &none);
    EXPECT(!none.contains("subject_references"));
    json stale = {{"cameras", {"2010093"}}, {"subject_references", block}};
    EXPECT(throws([&] { ApplySubjectReferencesGate(folder.string(), &stale); }, "froze none"));
    std::filesystem::remove_all(folder);
}

// Real client against a local server (tools/recording_subject_reference_tests.py starts it):
// <url>/dishes/19220_1/citrus-snapshot -> 200 JSON, <url>/dishes/nope/citrus-snapshot -> 404 JSON,
// <url>/slow -> no response within the timeout.
void test_real_http(const std::string& base_url)
{
    const auto zb = ZebrobotLookupConfig::Parse({{"base_url", base_url}, {"timeout_ms", 1000}});
    SubjectDeclaration dish; dish.dish_id = "19220_1";
    SubjectDeclaration nope; nope.dish_id = "nope";
    const json c = BuildSubjectReference(dish, zb, kNow);
    EXPECT(c.at("status") == "collected" && c.at("dish").at("dish_uuid") == "28c29cc6-a1ef-4382-9dc5-414fbb445d92" &&
           c.at("dish_fish").size() == 1 && c.at("dish_fish_lookup").at("status") == "complete");
    const json nf = BuildSubjectReference(nope, zb, kNow);
    EXPECT(nf.at("status") == "lookup_failed" && nf.at("zebrobot").at("http_status") == 404 &&
           nf.at("zebrobot").at("error").at("detail_error") == "dish_not_found");
    const HttpGetResult slow = HttpGet(base_url + "/slow", 300);
    EXPECT(!slow.transport_ok && slow.error.find("timed out") != std::string::npos);
    const HttpGetResult unreachable = HttpGet("http://127.0.0.1:1/x", 300);
    EXPECT(!unreachable.transport_ok);
    const HttpGetResult https = HttpGet("https://example.invalid/", 300);
    EXPECT(!https.transport_ok && https.error.find("http://") != std::string::npos);
}
}  // namespace

int main(int argc, char** argv)
{
    test_configs();
    test_builder_outcomes();
    test_block_rules_and_gate();
    if (argc > 1) test_real_http(argv[1]);
    if (g_failures) { std::fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    std::puts(argc > 1 ? "recording_subject_reference_tests: all passed (incl. real HTTP)" : "recording_subject_reference_tests: all passed");
    return 0;
}
