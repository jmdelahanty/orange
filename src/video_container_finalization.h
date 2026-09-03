#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace OrangeVideoContainerFinalization {

enum class Status {
    RecordingOpen,
    Finalizing,
    Complete,
    DegradedPlaybackIntentUnpatched,
    ContainerFinalizationFailed,
};

struct Outcome {
    bool header_written = false;
    bool trailer_attempted = false;
    bool trailer_written = false;
    bool output_close_attempted = false;
    bool output_closed = false;
    bool playback_intent_patch_attempted = false;
    bool playback_intent_patch_applied = false;
    bool writer_error_latched = false;
    bool muxer_flush_attempted = false;
    bool muxer_flush_succeeded = false;

    std::uint64_t packet_submissions_accepted = 0;
    std::uint64_t packet_submission_bytes_accepted = 0;
    std::uint64_t packet_submissions_rejected = 0;
    std::uint64_t packet_write_attempts = 0;
    std::uint64_t packets_written = 0;
    std::uint64_t packet_bytes_written = 0;
    std::uint64_t packet_write_failures = 0;
    std::optional<int> first_packet_write_error_code;
    std::optional<int> muxer_flush_error_code;
    std::string muxer_flush_error;

    std::optional<int> trailer_error_code;
    std::string trailer_error;
    std::optional<int> output_close_error_code;
    std::string output_close_error;
    std::string playback_intent_patch_error;
};

inline constexpr char kFullFrameRatePlaybackIntentKey[] =
    "com.apple.quicktime.full-frame-rate-playback-intent";

Status ClassifyTerminalStatus(const Outcome& outcome);
const char* StatusName(Status status);
bool IsTerminal(Status status);

std::filesystem::path SidecarPathFor(
    const std::filesystem::path& video_path);

// FFmpeg 4.x writes arbitrary QuickTime mdta values as UTF-8. After a clean
// close, replace this key's same-sized UTF-8 "1" representation with Apple's
// required UInt8 representation and verify the exact bytes in place.
bool PatchFullFrameRatePlaybackIntent(const std::filesystem::path& video_path,
                                      std::string* error);

// Persist a small adjacent lifecycle document. This never reads or hashes the
// media payload; it stats the file and atomically replaces the JSON sidecar in
// the video's directory under Orange's invoking-user filesystem identity.
bool Persist(const std::filesystem::path& video_path,
             int recording_fps,
             Status status,
             const Outcome& outcome,
             std::filesystem::path* sidecar_path,
             std::string* error);

}  // namespace OrangeVideoContainerFinalization
