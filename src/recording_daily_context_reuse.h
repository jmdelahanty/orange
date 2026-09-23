#pragma once
#include "recording_registered_context.h"

namespace orange::recording {
nlohmann::json PrepareDailyContextReuse(const RegisteredContextConfig& config,
    const std::filesystem::path& root, const std::vector<RegisteredContextCamera>& cameras,
    const nlohmann::json& geometry);
void ApplyDailyContextReuseGate(const std::filesystem::path& root,
    const nlohmann::json& immutable_snapshot, nlohmann::json* parent);
}
