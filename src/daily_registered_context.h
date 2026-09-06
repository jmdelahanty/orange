#pragma once
#include "recording_registered_context.h"

namespace orange::calibration {
struct DailyContextPlan {
    std::filesystem::path output_root, registration_path;
    std::string capture_id, requested_at_utc, registration_sha256;
    int housekeeping_cpu = -1;
    nlohmann::json cameras = nlohmann::json::object();
    nlohmann::json declaration, geometry, runtime_before, runtime_after;
};
struct DailyContextFrame {
    recording::RegisteredContextFrame source;
    std::string source_storage;
};
// CPU-only persistence. No camera calls, image conversion or record-start side
// effects. The caller supplies the copied pixels and selected-runtime evidence.
// New daily context artifacts are explicitly unbound to any recording.
nlohmann::json WriteDailyRegisteredContext(const DailyContextPlan& plan,
                                         const std::vector<DailyContextFrame>& frames);
void ValidateDailyContextSelection(const nlohmann::json& status,
                                  const std::filesystem::path& registration,
                                  const std::string& sha256,
                                  const nlohmann::json& cameras);
}
