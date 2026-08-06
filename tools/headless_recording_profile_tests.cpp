#include "headless_recording_profile.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

nlohmann::json valid_profile()
{
    return {
        {"codec", "hevc"},
        {"preset", "p1"},
        {"tuning", "ll"},
        {"rate_control_mode", "vbr"},
        {"quality_value", 20},
        {"gop_length", 30},
        {"aq", "off"},
        {"temporal_aq", "off"},
        {"lookahead", "off"},
        {"lookahead_depth", 0},
        {"target_bitrate_bps", 45000000},
        {"max_bitrate_bps", 60000000},
        {"vbv_buffer_size", 60000000},
        {"importance_map", {{"mode", "off"}}},
    };
}

void parses_and_round_trips()
{
    orange::headless::RecordingProfile profile;
    std::string error;
    require(orange::headless::ParseRecordingProfile(
                valid_profile(), &profile, &error, "fixed.recording_profile"),
            "valid profile should parse: " + error);
    require(profile.codec == "hevc" && profile.preset == "p1",
            "profile identity mismatch");
    require(profile.control.target_bitrate_bps == 45000000 &&
                profile.control.max_bitrate_bps == 60000000 &&
                profile.control.vbv_buffer_size == 60000000,
            "profile rate controls mismatch");

    orange::headless::RecordingProfile round_trip;
    require(orange::headless::ParseRecordingProfile(
                orange::headless::BuildRecordingProfileJson(profile),
                &round_trip,
                &error),
            "built profile should parse: " + error);
    require(round_trip.gop_length == 30 && round_trip.control.aq == 0 &&
                round_trip.control.temporal_aq == 0 &&
                round_trip.control.lookahead == 0,
            "round-trip profile mismatch");
}

void rejects_invalid_profiles()
{
    orange::headless::RecordingProfile profile;
    std::string error;

    nlohmann::json missing = valid_profile();
    missing.erase("target_bitrate_bps");
    require(!orange::headless::ParseRecordingProfile(missing, &profile, &error),
            "missing target bitrate should fail");
    require(error.find("target_bitrate_bps") != std::string::npos,
            "missing-field error should name target bitrate");

    nlohmann::json inverted = valid_profile();
    inverted["max_bitrate_bps"] = 30000000;
    require(!orange::headless::ParseRecordingProfile(inverted, &profile, &error),
            "max below target should fail");
    require(error.find("max_bitrate_bps") != std::string::npos,
            "rate-order error should name maximum bitrate");

    nlohmann::json hidden_analysis = valid_profile();
    hidden_analysis["temporal_aq"] = "on";
    hidden_analysis["typo_bitrate_bps"] = 1;
    require(!orange::headless::ParseRecordingProfile(
                hidden_analysis, &profile, &error),
            "unknown profile keys should fail closed");
    require(error.find("typo_bitrate_bps") != std::string::npos,
            "unknown-key error should name the typo");

    nlohmann::json inherited_controls = valid_profile();
    inherited_controls["aq"] = "auto";
    require(!orange::headless::ParseRecordingProfile(
                inherited_controls, &profile, &error),
            "authoritative profile must reject inherited AQ state");
    require(error.find("explicitly on or off") != std::string::npos,
            "inherited-control error should explain the explicit contract");
}

}  // namespace

int main()
{
    try {
        parses_and_round_trips();
        std::cout << "[PASS] parses_and_round_trips\n";
        rejects_invalid_profiles();
        std::cout << "[PASS] rejects_invalid_profiles\n";
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << "\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
