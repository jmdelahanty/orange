#pragma once

// Crop video quality for the external crop recorder, from
// ORANGE_CROP_EXTERNAL_QP (the app config's recording.crop.external_ipc.qp
// sets it): unset or empty = lossless; 1..51 = constant QP, all-intra.
// Header-only so the snapshot and finalizer code that reports the crop tuning
// does not need the recording session engine linked in.

#include <cctype>
#include <cstdlib>
#include <string>

namespace orange::session {

struct ExternalCropEncodeQuality {
    std::string tuning = "lossless";
    std::string rate_control_mode = "cqp";
    int quality_value = 0;
    std::string error;
};

inline ExternalCropEncodeQuality resolve_external_crop_encode_quality()
{
    ExternalCropEncodeQuality quality;
    const char* raw = std::getenv("ORANGE_CROP_EXTERNAL_QP");
    std::string text = raw ? raw : "";
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    if (text.empty()) {
        return quality;
    }
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || (end && *end) || value < 1 || value > 51) {
        quality.error = "ORANGE_CROP_EXTERNAL_QP='" + std::string(raw) +
            "' must be an integer QP within [1, 51] (unset for lossless crops)";
        return quality;
    }
    quality.tuning = "hq";
    quality.quality_value = static_cast<int>(value);
    return quality;
}

}  // namespace orange::session
