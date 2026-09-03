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

// True only when every accepted encoded packet and byte reached the FFmpeg
// mux boundary successfully, no submission was rejected, no contradictory
// error detail remains, and the terminal muxer flush completed without a
// latched writer error.
bool PacketWritesComplete(const Outcome& outcome);

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

// Descriptor-authoritative variant. The held descriptor is never reopened by
// pathname: all reads, the fixed-size metadata patch, verification, and fsync
// operate on the supplied media inode. video_display_label is evidence only
// and is never interpreted as filesystem authority.
bool PatchFullFrameRatePlaybackIntent(int video_fd,
                                      const std::string& video_display_label,
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

// Descriptor-authoritative lifecycle persistence. The media size comes from
// fstat(video_fd), and the JSON document is written in place through the
// explicit sidecar descriptor with ftruncate/write/fsync. Display labels are
// recorded in the document but are never opened or used to derive a filename.
bool Persist(int video_fd,
             const std::string& video_display_label,
             int sidecar_fd,
             const std::string& sidecar_display_label,
             int recording_fps,
             Status status,
             const Outcome& outcome,
             std::string* error);

}  // namespace OrangeVideoContainerFinalization
