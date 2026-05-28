#pragma once

#include <string>

namespace orange::session {

int resolve_external_crop_recorder_gpu_id_from_env(const std::string& serial,
                                                   int analytics_gpu_id);
bool external_crop_recorder_require_separate_gpu_from_env();
bool external_crop_recorder_same_gpu_disallowed(int analytics_gpu_id,
                                                int recorder_gpu_id);

}  // namespace orange::session
