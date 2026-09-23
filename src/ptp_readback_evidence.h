#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include "json.hpp"

namespace orange {
// Bookkeeping only: no camera calls, clock reads or synchronization. Call at the
// existing bounded PTP readback cadence. A range groups equal sampled states;
// it never asserts what happened between those observations.
class PtpReadbackEvidence {
public:
    static constexpr size_t kMaxStateRuns = 1024;

    void reset() { *this = PtpReadbackEvidence{}; }
    void recorded_frame(uint64_t frame) {
        if (!closed_ && frame != 0) last_recording_frame_id_ = frame;
    }
    uint64_t last_recording_frame_id() const { return last_recording_frame_id_; }
    bool closed() const { return closed_; }
    bool finish_if_stopped(bool recording_active, bool preserve_session) {
        if (closed_ || last_recording_frame_id_ == 0 || recording_active || preserve_session)
            return false;
        closed_ = true;
        return true;
    }

    void observe(const std::optional<std::string>& mode,
                 const std::optional<std::string>& status,
                 uint64_t local_frame, const std::string& utc, uint64_t steady_ns) {
        if (closed_) return;
        ++attempts_;
        if (!mode) ++mode_failures_;
        if (!status) ++status_failures_;
        if (last_sample_ns_ != 0 && steady_ns >= last_sample_ns_)
            max_gap_ns_ = std::max(max_gap_ns_, steady_ns - last_sample_ns_);
        last_sample_ns_ = steady_ns;
        const nlohmann::json m = mode ? nlohmann::json(*mode) : nlohmann::json(nullptr);
        const nlohmann::json s = status ? nlohmann::json(*status) : nlohmann::json(nullptr);
        // Do not merge across omitted runs: doing so would hide state changes.
        if (omitted_ == 0 && !observations_.empty() &&
            observations_.back().at("ptp_mode") == m &&
            observations_.back().at("ptp_status") == s) {
            auto& row = observations_.back();
            row["sampled_at_utc"] = utc;
            row["last_sampled_at_utc"] = utc;
            row["last_sampled_steady_ns"] = steady_ns;
            row["last_local_frame_id"] = local_frame;
            row["last_recording_frame_id"] = last_recording_frame_id_;
            row["samples"] = row.at("samples").get<uint64_t>() + 1;
        } else if (observations_.size() < kMaxStateRuns) {
            observations_.push_back({
                {"sampled_at_utc", utc}, {"first_sampled_at_utc", utc},
                {"last_sampled_at_utc", utc},
                {"first_sampled_steady_ns", steady_ns}, {"last_sampled_steady_ns", steady_ns},
                {"local_frame_id", local_frame}, {"first_local_frame_id", local_frame},
                {"last_local_frame_id", local_frame},
                {"recording_frame_id", last_recording_frame_id_},
                {"first_recording_frame_id", last_recording_frame_id_},
                {"last_recording_frame_id", last_recording_frame_id_},
                {"samples", uint64_t{1}}, {"ptp_mode", m}, {"ptp_status", s}});
        } else {
            ++omitted_;
        }
    }

    const nlohmann::json& observations() const { return observations_; }
    nlohmann::json coverage() const {
        return {{"schema_id", "orange.ptp.sampled_readback_coverage"}, {"schema_version", 1},
            {"semantics", "sampled_states_not_continuous_lock"},
            {"steady_clock", "std_chrono_steady_clock_implementation_epoch"},
            {"recording_frame_semantics", "last_assigned_in_this_recording_zero_before_first"},
            {"attempts", attempts_}, {"mode_read_failures", mode_failures_},
            {"status_read_failures", status_failures_}, {"max_sample_gap_ns", max_gap_ns_},
            {"omitted_observations", omitted_}, {"state_run_capacity", kMaxStateRuns}};
    }

private:
    bool closed_ = false;
    uint64_t last_recording_frame_id_ = 0;
    uint64_t attempts_ = 0, mode_failures_ = 0, status_failures_ = 0;
    uint64_t last_sample_ns_ = 0, max_gap_ns_ = 0, omitted_ = 0;
    nlohmann::json observations_ = nlohmann::json::array();
};
}  // namespace orange
