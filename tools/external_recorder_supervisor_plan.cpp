#include "external_recorder_supervisor.h"

#include "json.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void usage(const char* argv0)
{
    std::cerr
        << "Usage: " << argv0 << " (--spec <experiment_spec.json> | --contract <contract.json>) [options]\n\n"
        << "Options:\n"
        << "  --recorder-tool <path>       Recorder executable path for generated argv.\n"
        << "  --encode-queue-depth <int>   Default queue depth when a stream omits it.\n"
        << "  --prewarm-slots <int>        Default prewarm slots when a stream omits it.\n"
        << "  --prewarm-bytes <int>        Default prewarm bytes when a stream omits it.\n"
        << "  --no-prewarm-peer-copy       Default peer-copy prewarm off when a stream omits it.\n"
        << "  --socket-timeout-ms <int>    Timeout for socket readiness in --start-and-stop.\n"
        << "  --shutdown-timeout-ms <int>  Natural-exit wait before SIGTERM in --start-and-stop.\n"
        << "  --print-json                 Print the supervisor plan JSON. This is the default.\n"
        << "  --check                      Validate only; print a short summary.\n"
        << "  --start-and-stop             Launch recorders, wait for sockets, then stop them.\n"
        << "  --allow-regular-ready-for-tests  Treat regular files as socket-ready markers.\n"
        << "  --help\n";
}

int parse_int(const std::string& value, const char* name)
{
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < 0) {
        throw std::runtime_error(std::string("invalid ") + name + ": " + value);
    }
    return static_cast<int>(parsed);
}

uint64_t parse_u64(const std::string& value, const char* name)
{
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        throw std::runtime_error(std::string("invalid ") + name + ": " + value);
    }
    return static_cast<uint64_t>(parsed);
}

nlohmann::json read_json_file(const std::string& path)
{
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open " + path);
    }
    nlohmann::json json;
    in >> json;
    return json;
}

}  // namespace

int main(int argc, char** argv)
{
    std::string spec_path;
    std::string contract_path;
    bool check_only = false;
    bool print_json = true;
    bool start_and_stop = false;
    orange::external_recorder::SupervisorPlanOptions options;
    orange::external_recorder::SupervisorProcessOptions process_options;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto require_value = [&](const char* flag) -> std::string {
                if (i + 1 >= argc) {
                    throw std::runtime_error(std::string(flag) + " requires a value");
                }
                return argv[++i];
            };
            if (arg == "--help" || arg == "-h") {
                usage(argv[0]);
                return 0;
            } else if (arg == "--spec") {
                spec_path = require_value("--spec");
            } else if (arg == "--contract") {
                contract_path = require_value("--contract");
            } else if (arg == "--recorder-tool") {
                options.recorder_tool_path = require_value("--recorder-tool");
            } else if (arg == "--encode-queue-depth") {
                options.default_encode_queue_depth =
                    parse_int(require_value("--encode-queue-depth"), "--encode-queue-depth");
            } else if (arg == "--prewarm-slots") {
                options.default_prewarm_slots =
                    parse_int(require_value("--prewarm-slots"), "--prewarm-slots");
            } else if (arg == "--prewarm-bytes") {
                options.default_prewarm_bytes =
                    parse_u64(require_value("--prewarm-bytes"), "--prewarm-bytes");
            } else if (arg == "--no-prewarm-peer-copy") {
                options.default_prewarm_peer_copy = false;
            } else if (arg == "--socket-timeout-ms") {
                process_options.socket_ready_timeout_ms =
                    parse_int(require_value("--socket-timeout-ms"), "--socket-timeout-ms");
            } else if (arg == "--shutdown-timeout-ms") {
                process_options.graceful_shutdown_timeout_ms =
                    parse_int(require_value("--shutdown-timeout-ms"), "--shutdown-timeout-ms");
            } else if (arg == "--print-json") {
                print_json = true;
                check_only = false;
            } else if (arg == "--check") {
                check_only = true;
                print_json = false;
            } else if (arg == "--start-and-stop") {
                start_and_stop = true;
                check_only = false;
            } else if (arg == "--allow-regular-ready-for-tests") {
                process_options.allow_regular_file_socket_ready_for_tests = true;
            } else {
                throw std::runtime_error("unknown argument: " + arg);
            }
        }

        if (spec_path.empty() == contract_path.empty()) {
            throw std::runtime_error("provide exactly one of --spec or --contract");
        }

        orange::external_recorder::SupervisorPlan plan;
        std::string error;
        if (!spec_path.empty()) {
            const nlohmann::json spec = read_json_file(spec_path);
            if (!orange::external_recorder::BuildSupervisorPlanFromExperimentSpec(
                    spec, options, &plan, &error)) {
                throw std::runtime_error(error);
            }
            plan.source_path = spec_path;
        } else {
            const nlohmann::json contract = read_json_file(contract_path);
            if (!orange::external_recorder::BuildSupervisorPlanFromContract(
                    contract, options, &plan, &error)) {
                throw std::runtime_error(error);
            }
            plan.source_path = contract_path;
        }

        if (start_and_stop) {
            orange::external_recorder::SupervisorRuntimeState runtime;
            if (!orange::external_recorder::StartSupervisorProcesses(
                    plan, process_options, &runtime, &error)) {
                std::cerr << orange::external_recorder::SupervisorRuntimeStateToJson(runtime).dump(2)
                          << "\n";
                throw std::runtime_error(error);
            }
            const bool stopped = orange::external_recorder::StopSupervisorProcesses(
                &runtime, process_options, &error);
            std::cout << orange::external_recorder::SupervisorRuntimeStateToJson(runtime).dump(2)
                      << "\n";
            if (!stopped) {
                throw std::runtime_error(error.empty()
                                             ? "external recorder supervisor stop failed"
                                             : error);
            }
        } else if (print_json) {
            std::cout << orange::external_recorder::SupervisorPlanToJson(plan).dump(2)
                      << "\n";
        } else if (check_only) {
            std::cout << "external recorder supervisor plan OK: streams="
                      << plan.streams.size() << " session_id=" << plan.session_id
                      << " artifact_root=" << plan.artifact_root << "\n";
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "external_recorder_supervisor_plan: " << ex.what() << "\n";
        return 2;
    }
}
