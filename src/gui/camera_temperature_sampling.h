#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

struct GuiCameraTemperatureSamplingState {
    bool enabled = false;
    std::chrono::steady_clock::time_point next_sample_at{};
    uint64_t sample_count = 0;
    uint64_t skipped_count = 0;
};

inline bool gui_camera_temperature_sample_due(
    GuiCameraTemperatureSamplingState* state,
    const bool enabled,
    const std::chrono::steady_clock::time_point now,
    const std::chrono::milliseconds interval = std::chrono::milliseconds(1000))
{
    if (!state) {
        return enabled;
    }
    if (!enabled) {
        state->enabled = false;
        state->next_sample_at = {};
        return false;
    }

    const bool first_sample = !state->enabled;
    state->enabled = true;
    if (!first_sample && now < state->next_sample_at) {
        ++state->skipped_count;
        return false;
    }

    ++state->sample_count;
    state->next_sample_at =
        now + std::max(interval, std::chrono::milliseconds(0));
    return true;
}
