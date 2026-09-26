// recording_observation_binding_cli: run Orange's observation-binding pre-arm
// and post-close finalization on an existing recording folder, with the same
// production functions the GUI and headless client call, so a cross-repository
// fixture pipeline (Citrus H5 generation on an Orange synthetic bundle) can
// complete the Orange side without compiling ad-hoc clients.
//
//   recording_observation_binding_cli prearm --folder <recording>
//       [--binding-mode required|optional|not_applicable] [--decided-at <utc>]
//       [--socket <af_unix path>] [--timeout-ms N] [--request-version 1|2]
//     Materializes (or verifies) the sealed requests, sends the binding batch
//     to Citrus's local-control socket, persists the acceptances and the
//     create-once pre-arm decision. Idempotent: a repeat replays the existing
//     decision and never contacts Citrus again.
//
//   recording_observation_binding_cli finalize --folder <recording>
//       --receipts <receipts.json>
//     <receipts.json> is {"experiment_id": "...", "receipts": [<sealed receipt v1>, ...]}
//     (the local-control finalization params). Validates every receipt against
//     the immutable request/acceptance chain, verifies the closed H5 bytes,
//     materializes create-once receipt artifacts and finalized_collection.json,
//     and refreshes recording_session.json's binding projection when a manifest
//     exists. Safe to retry with byte-identical evidence.
//
// No camera, recorder or GUI is involved. See docs/synthetic_recording_bundle.md.
#include "json.hpp"
#include "session/recording_observation_finalization.h"
#include "session/recording_observation_prearm.h"
#include "session/recording_observation_request_artifacts.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using json = nlohmann::json;

[[noreturn]] void usage(const std::string& error = "")
{
    if (!error.empty()) std::cerr << "error: " << error << "\n\n";
    std::cerr <<
        "usage: recording_observation_binding_cli prearm --folder <recording>\n"
        "           [--binding-mode required|optional|not_applicable] [--decided-at <utc>]\n"
        "           [--socket <path>] [--timeout-ms N] [--request-version 1|2]\n"
        "       recording_observation_binding_cli finalize --folder <recording> --receipts <params.json>\n"
        "Prints a JSON result on stdout; exit 0 only when the step succeeded.\n";
    std::exit(2);
}

std::string now_utc()
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buffer[32];
    std::tm tm{};
    gmtime_r(&now, &tm);
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

json rejections_json(const orange::session::RecordingObservationPreArmResult& result)
{
    json rows = json::array();
    for (const auto& rejection : result.context_rejections) {
        rows.push_back({{"observation_context_id", rejection.observation_context_id},
                        {"reason", rejection.reason}});
    }
    return rows;
}

int run_prearm(int argc, char** argv)
{
    std::string folder, binding_mode = "optional", decided_at, socket_path, timeout_ms, request_version;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&]() -> std::string { if (i + 1 >= argc) usage(a + " needs a value"); return argv[++i]; };
        if (a == "--folder") folder = need();
        else if (a == "--binding-mode") binding_mode = need();
        else if (a == "--decided-at") decided_at = need();
        else if (a == "--socket") socket_path = need();
        else if (a == "--timeout-ms") timeout_ms = need();
        else if (a == "--request-version") request_version = need();
        else usage("unknown prearm argument " + a);
    }
    if (folder.empty()) usage("--folder is required");
    if (!socket_path.empty()) setenv("ORANGE_CITRUS_OBSERVATION_BINDING_SOCKET", socket_path.c_str(), 1);
    if (!timeout_ms.empty()) setenv("ORANGE_CITRUS_OBSERVATION_BINDING_TIMEOUT_MS", timeout_ms.c_str(), 1);
    if (!request_version.empty()) setenv("ORANGE_CITRUS_BINDING_REQUEST_VERSION", request_version.c_str(), 1);
    if (decided_at.empty()) decided_at = now_utc();

    orange::session::RecordingObservationBindingRequestMaterialization requests;
    orange::session::RecordingObservationPreArmResult result;
    std::string error;
    const bool ok = orange::session::prepare_recording_observation_pre_arm(
        folder, binding_mode, decided_at, &requests, &result, &error);
    json out = {
        {"step", "prearm"}, {"ok", ok}, {"error", error},
        {"binding_mode", result.binding_mode}, {"lifecycle_status", result.lifecycle_status},
        {"reason", result.reason}, {"arm_allowed", result.arm_allowed},
        {"transport_attempted", result.transport_attempted},
        {"decision_relative_path", result.decision_relative_path},
        {"decision_sha256", result.decision_sha256},
        {"requests", {{"status", requests.status}, {"reason", requests.reason},
                      {"collection_relative_path", requests.collection_relative_path},
                      {"count", requests.artifacts.size()}}},
        {"acceptance_count", result.acceptances.size()},
        {"context_rejections", rejections_json(result)},
    };
    std::cout << out.dump(2) << std::endl;
    return ok ? 0 : 1;
}

int run_finalize(int argc, char** argv)
{
    std::string folder, receipts_path;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&]() -> std::string { if (i + 1 >= argc) usage(a + " needs a value"); return argv[++i]; };
        if (a == "--folder") folder = need();
        else if (a == "--receipts") receipts_path = need();
        else usage("unknown finalize argument " + a);
    }
    if (folder.empty() || receipts_path.empty()) usage("--folder and --receipts are required");
    std::ifstream input(receipts_path);
    if (!input) { std::cerr << "cannot read " << receipts_path << std::endl; return 1; }
    const json params = json::parse(input, nullptr, false);
    if (params.is_discarded() || !params.is_object() || !params.contains("receipts")) {
        std::cerr << "receipts file must be a JSON object with an experiment_id and a receipts array" << std::endl;
        return 1;
    }
    const auto result = orange::session::finalize_recording_observation_bindings(folder, params);
    std::string refresh_error;
    bool refreshed = false;
    if (result.ok) {
        refreshed = orange::session::refresh_recording_session_observation_bindings(folder, &refresh_error);
    }
    json out = {
        {"step", "finalize"}, {"ok", result.ok && refreshed}, {"error", result.error},
        {"collection_reference", result.collection_reference},
        {"collection_status", result.collection.value("status", "")},
        {"binding_status", result.collection.value("binding_status", "")},
        {"manifest_refreshed", refreshed}, {"refresh_error", refresh_error},
    };
    std::cout << out.dump(2) << std::endl;
    return (result.ok && refreshed) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) usage();
    const std::string command = argv[1];
    try {
        if (command == "prearm") return run_prearm(argc, argv);
        if (command == "finalize") return run_finalize(argc, argv);
        if (command == "-h" || command == "--help") usage();
        usage("unknown command " + command);
    } catch (const std::exception& ex) {
        std::cerr << "recording_observation_binding_cli: " << ex.what() << std::endl;
        return 1;
    }
}
