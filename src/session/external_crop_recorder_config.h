#pragma once

#include <string>

namespace orange::session {

int resolve_external_crop_recorder_gpu_id_from_env(const std::string& serial,
                                                   int analytics_gpu_id);

}  // namespace orange::session
