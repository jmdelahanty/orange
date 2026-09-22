#pragma once

#include <cstdint>
#include <string>

// RecordingStartupAudit (2026-09-21, diagnostic): timestamped milestones of
// the record-start transition, per recording session, camera and encoder
// shard, so the question "what initialization is still happening after we
// begin accepting recording frames?" can be answered from one file.
//
// Records are buffered in memory (bounded; overflow is counted and reported)
// and written by the GUI/session thread at recording stop, never by an
// acquisition or GPU-submission thread. Mark() is a mutex-protected push of a
// small struct and is safe to call from any thread; per-frame marks are only
// taken inside the diagnostic window (the first WindowSeconds() after
// recording was enabled). Timestamps are CLOCK_MONOTONIC-equivalent
// (std::steady_clock) nanoseconds; the session record carries the matching
// CLOCK_REALTIME value so rows can be joined with Cam*_crop_meta.csv.
namespace orange {

class RecordingStartupAudit {
public:
    static RecordingStartupAudit& Instance();

    // Arms a new session (flushes any unwritten previous session first).
    void BeginSession(const std::string& session_id, const std::string& recording_folder);
    // Milestone from any thread. `camera` may be empty for session-wide
    // events; `frame_id` is the recording frame id when known.
    void Mark(const std::string& camera, const char* milestone,
              const std::string& detail = std::string(), uint64_t frame_id = 0);
    // Marks "recording_enabled" and opens the per-frame window.
    void MarkRecordingEnabled(const std::string& detail);
    // True while per-frame records should be taken (before recording was
    // enabled, and for WindowSeconds() after it).
    bool InWindow() const;
    // Writes <folder>/recording_startup_audit.jsonl and
    // <folder>/recording_startup_audit_summary.json. Idempotent.
    void EndSession();

    static double WindowSeconds();
    uint64_t overflow() const;

private:
    RecordingStartupAudit() = default;
    struct Impl;
    Impl* impl();
};

}  // namespace orange
