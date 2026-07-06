// Shadow-mode incremental clip finalization worker (stage 4). See
// gui/incremental_clip_shadow.h for the design and threading model.

#include "gui/incremental_clip_shadow.h"

#include "gui/env_util.h"

#include "external_recorder_lifecycle.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace {

// Bounded retry budget for kIncomplete clips. The crop root CSVs are
// flushed per row, so a completed clip's rows are normally readable within
// one retry; the budget only has to outlast transient scheduling delays.
// 60 retries with a 100 ms -> 2 s doubling backoff spans roughly two
// minutes before the chain parks.
constexpr int kGuiShadowMaxIncompleteRetries = 60;
constexpr std::chrono::milliseconds kGuiShadowRetryInitialBackoff{100};
constexpr std::chrono::milliseconds kGuiShadowRetryMaxBackoff{2000};

std::chrono::milliseconds gui_shadow_retry_backoff(const int retry_count)
{
    std::chrono::milliseconds backoff = kGuiShadowRetryInitialBackoff;
    for (int i = 1; i < retry_count && backoff < kGuiShadowRetryMaxBackoff; ++i) {
        backoff *= 2;
    }
    return std::min(backoff, kGuiShadowRetryMaxBackoff);
}

std::string gui_shadow_utc_now()
{
    const std::time_t seconds =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm_utc{};
    gmtime_r(&seconds, &tm_utc);
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        return {};
    }
    return buffer;
}

std::string gui_shadow_clip_id(const int clip_index)
{
    // Matches the recorder's clip directory naming (clips/clip_%06d) and
    // gui_external_recorder_clip_id in gui/recording_finalizer.cpp.
    std::ostringstream out;
    out << "clip_" << std::setw(6) << std::setfill('0') << clip_index;
    return out.str();
}

std::string gui_shadow_strip_crop_suffix(std::string serial)
{
    const std::string suffix = "_crop";
    if (serial.size() > suffix.size() &&
        serial.compare(serial.size() - suffix.size(), suffix.size(), suffix) == 0) {
        serial.resize(serial.size() - suffix.size());
    }
    return serial;
}

bool gui_shadow_stop_requested(GuiIncrementalClipShadowState* state)
{
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->stop_requested;
}

void gui_shadow_append_index_line_or_warn(
    GuiIncrementalClipShadowState* state,
    const nlohmann::json& line)
{
    std::string append_error;
    if (!gui_shadow_append_index_line(state->index_path, line, &append_error)) {
        std::cerr << "[GUI][clip_shadow] WARNING: failed to append to shadow"
                     " index " << state->index_path << ": " << append_error
                  << std::endl;
    }
}

// Parks a chain: records the parked state durably in the shadow index and
// logs loudly. The chain's cursor stops advancing (later clips of this
// camera cannot be split without violating the sequential-cursor gap
// policy) but other cameras, the recording, and the finalize continue
// untouched.
void gui_shadow_park_chain(
    GuiIncrementalClipShadowState* state,
    GuiShadowCameraChainState* chain,
    const int clip_index,
    const std::string& reason)
{
    chain->parked = true;
    chain->parked_clip_index = clip_index;
    chain->parked_reason = reason;
    state->chains_parked.fetch_add(1, std::memory_order_relaxed);
    std::cerr << "[GUI][clip_shadow] ERROR: shadow chain parked for Cam"
              << chain->camera_serial << " at clip " << clip_index << ": "
              << reason
              << " (shadow mode observes only; recording and finalize are"
                 " unaffected)"
              << std::endl;
    gui_shadow_append_index_line_or_warn(
        state,
        gui_shadow_chain_parked_index_line(
            chain->camera_serial,
            clip_index,
            reason,
            gui_shadow_utc_now()));
}

void gui_incremental_clip_shadow_worker(GuiIncrementalClipShadowState* state)
{
    // Thread boundary (docs/error_handling_convention.md): an exception
    // escaping a raw std::thread would std::terminate the whole process.
    // Catch, report loudly, and publish done so the join never hangs.
    try {
        {
            nlohmann::json cameras = nlohmann::json::array();
            for (const GuiShadowCameraChainState& chain : state->chains) {
                cameras.push_back(chain.camera_serial);
            }
            gui_shadow_append_index_line_or_warn(
                state,
                {
                    {"event", "shadow_started"},
                    {"recording_folder", state->recording_folder},
                    {"cameras", std::move(cameras)},
                    {"wall_clock_utc", gui_shadow_utc_now()},
                });
        }

        uint64_t seen_sequence = 0;
        std::vector<GuiShadowClipAnnouncement> captured(state->chains.size());
        bool stop = false;
        while (!stop) {
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                if (state->stop_requested) {
                    break;
                }
                seen_sequence = state->push_sequence;
                for (size_t i = 0;
                     i < state->slots.size() && i < captured.size();
                     ++i) {
                    captured[i] = state->slots[i].latest;
                }
            }

            // Fold the captured watermarks into the per-camera chains.
            for (size_t i = 0; i < state->chains.size(); ++i) {
                GuiShadowCameraChainState* chain = &state->chains[i];
                if (chain->parked) {
                    continue;
                }
                const int previous_accepted = chain->last_accepted_clip_index;
                if (gui_shadow_intake_announcement(chain, captured[i]) ==
                    GuiShadowAnnouncementIntake::kGap) {
                    gui_shadow_park_chain(
                        state,
                        chain,
                        captured[i].clip_index,
                        "clip completion announcements skipped from index " +
                            std::to_string(previous_accepted) + " to " +
                            std::to_string(captured[i].clip_index) +
                            "; the skipped clips' frame ranges are unknown");
                }
            }

            // Process every chain that is ready (not parked, has pending
            // clips, and is not backing off between retries).
            const auto now = std::chrono::steady_clock::now();
            for (GuiShadowCameraChainState& chain : state->chains) {
                if (chain.parked || chain.pending.empty() ||
                    now < chain.next_retry_at) {
                    continue;
                }
                while (!chain.parked && !chain.pending.empty()) {
                    if (gui_shadow_stop_requested(state)) {
                        stop = true;
                        break;
                    }
                    const GuiShadowClipAnnouncement clip = chain.pending.front();
                    const GuiShadowClipAttemptResult attempt =
                        gui_shadow_process_clip_attempt(&chain, clip);
                    if (attempt.status == GuiShadowClipAttemptStatus::kCompleted) {
                        gui_shadow_append_index_line_or_warn(
                            state,
                            gui_shadow_clip_completed_index_line(
                                chain.camera_serial,
                                clip,
                                attempt.meta_rows,
                                attempt.perf_rows,
                                attempt.meta_shadow_path,
                                attempt.perf_shadow_path,
                                gui_shadow_utc_now()));
                        chain.pending.pop_front();
                        chain.incomplete_retry_count = 0;
                        chain.next_retry_at = {};
                        ++chain.clips_completed;
                        state->clips_completed.fetch_add(1, std::memory_order_relaxed);
                        std::cout << "[GUI][clip_shadow] Cam" << chain.camera_serial
                                  << " clip " << clip.clip_index
                                  << " shadow-split: meta_rows=" << attempt.meta_rows
                                  << " perf_rows=" << attempt.perf_rows
                                  << std::endl;
                    } else if (attempt.status ==
                               GuiShadowClipAttemptStatus::kIncomplete) {
                        ++chain.incomplete_retry_count;
                        if (chain.incomplete_retry_count >
                            kGuiShadowMaxIncompleteRetries) {
                            gui_shadow_park_chain(
                                state,
                                &chain,
                                clip.clip_index,
                                "clip rows still incomplete after " +
                                    std::to_string(kGuiShadowMaxIncompleteRetries) +
                                    " retries" +
                                    (attempt.error.empty()
                                         ? std::string()
                                         : (": " + attempt.error)));
                        } else {
                            chain.next_retry_at =
                                std::chrono::steady_clock::now() +
                                gui_shadow_retry_backoff(
                                    chain.incomplete_retry_count);
                        }
                        break;
                    } else {
                        gui_shadow_park_chain(
                            state,
                            &chain,
                            clip.clip_index,
                            attempt.error.empty()
                                ? "shadow clip split failed"
                                : attempt.error);
                        break;
                    }
                }
                if (stop) {
                    break;
                }
            }
            if (stop) {
                break;
            }

            // Sleep until the stop flag, a new watermark, or the earliest
            // pending retry deadline (condition-variable wake, never a
            // busy-spin).
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                if (state->stop_requested) {
                    break;
                }
                if (state->push_sequence != seen_sequence) {
                    continue;  // A push landed while processing.
                }
                bool has_deadline = false;
                bool has_ready = false;
                std::chrono::steady_clock::time_point deadline{};
                for (const GuiShadowCameraChainState& chain : state->chains) {
                    if (chain.parked || chain.pending.empty()) {
                        continue;
                    }
                    if (chain.next_retry_at ==
                        std::chrono::steady_clock::time_point{}) {
                        has_ready = true;
                        break;
                    }
                    if (!has_deadline || chain.next_retry_at < deadline) {
                        has_deadline = true;
                        deadline = chain.next_retry_at;
                    }
                }
                if (has_ready) {
                    continue;
                }
                const auto woken = [&]() {
                    return state->stop_requested ||
                           state->push_sequence != seen_sequence;
                };
                if (has_deadline) {
                    state->cv.wait_until(lock, deadline, woken);
                } else {
                    state->cv.wait(lock, woken);
                }
                if (state->stop_requested) {
                    break;
                }
            }
        }

        gui_shadow_append_index_line_or_warn(
            state,
            {
                {"event", "shadow_stopped"},
                {"clips_completed",
                 state->clips_completed.load(std::memory_order_relaxed)},
                {"chains_parked",
                 state->chains_parked.load(std::memory_order_relaxed)},
                {"wall_clock_utc", gui_shadow_utc_now()},
            });
    } catch (const std::exception& ex) {
        std::cerr << "[GUI][clip_shadow] ERROR: shadow worker threw: "
                  << ex.what() << std::endl;
    } catch (...) {
        std::cerr << "[GUI][clip_shadow] ERROR: shadow worker threw a non-std"
                     " exception" << std::endl;
    }
    state->done.store(true, std::memory_order_release);
}

}  // namespace

bool gui_incremental_clip_shadow_enabled()
{
    return gui_env_flag_enabled(kGuiIncrementalClipShadowEnvFlag, false);
}

GuiShadowAnnouncementIntake gui_shadow_classify_announcement(
    const int last_accepted_clip_index,
    const GuiShadowClipAnnouncement& announcement)
{
    if (!announcement.valid()) {
        return GuiShadowAnnouncementIntake::kIgnored;
    }
    if (announcement.clip_index <= last_accepted_clip_index) {
        return GuiShadowAnnouncementIntake::kIgnored;
    }
    if (announcement.clip_index == last_accepted_clip_index + 1) {
        return GuiShadowAnnouncementIntake::kAccepted;
    }
    return GuiShadowAnnouncementIntake::kGap;
}

GuiShadowAnnouncementIntake gui_shadow_intake_announcement(
    GuiShadowCameraChainState* chain,
    const GuiShadowClipAnnouncement& announcement)
{
    if (!chain) {
        return GuiShadowAnnouncementIntake::kIgnored;
    }
    const GuiShadowAnnouncementIntake intake =
        gui_shadow_classify_announcement(
            chain->last_accepted_clip_index,
            announcement);
    if (intake == GuiShadowAnnouncementIntake::kAccepted) {
        chain->pending.push_back(announcement);
        chain->last_accepted_clip_index = announcement.clip_index;
    }
    return intake;
}

GuiShadowClipAttemptResult gui_shadow_process_clip_attempt(
    GuiShadowCameraChainState* chain,
    const GuiShadowClipAnnouncement& clip)
{
    GuiShadowClipAttemptResult result;
    if (!chain || !clip.valid()) {
        result.error = "invalid shadow clip attempt inputs";
        return result;
    }
    result.meta_shadow_path = gui_shadow_meta_csv_path(
        chain->clips_root, clip.clip_index, chain->camera_serial);
    result.perf_shadow_path = gui_shadow_perf_csv_path(
        chain->clips_root, clip.clip_index, chain->camera_serial);

    // The root CSVs are created when the crop worker accepts its first
    // frame; a completed clip implies they exist, but a benign scheduling
    // race is retryable rather than an error.
    std::error_code exists_error;
    if (!std::filesystem::exists(chain->root_meta_csv_path, exists_error) ||
        !std::filesystem::exists(chain->root_perf_csv_path, exists_error)) {
        result.status = GuiShadowClipAttemptStatus::kIncomplete;
        result.error = "crop root CSVs not created yet";
        return result;
    }

    const orange::session::RecordingFrameCsvRange range{
        clip.first_recording_frame_id,
        clip.last_recording_frame_id,
        std::string(),
        0,
    };

    // Crop metadata rows exist only for frames accepted for encoding, so an
    // orphan row is a hole in the recorded data: fail loudly (kFail), same
    // policy as the authoritative finalize split.
    const orange::session::RecordingFrameCsvSplitResult meta_result =
        orange::session::append_clip_rows_from_root_csv(
            chain->root_meta_csv_path,
            &chain->meta_cursor,
            range,
            result.meta_shadow_path,
            orange::session::RecordingFrameCsvOrphanRowPolicy::kFail);
    result.meta_rows = meta_result.clip_rows_written;
    if (meta_result.status ==
        orange::session::RecordingFrameCsvClipSplitStatus::kFailed) {
        result.status = GuiShadowClipAttemptStatus::kFailed;
        result.error = "crop meta shadow split failed: " + meta_result.error;
        return result;
    }

    // Crop perf rows are also written for frames dropped before external
    // submission, so head/tail orphans are legitimate: count and skip
    // (kCountAndSkip), same policy as the authoritative finalize split.
    const orange::session::RecordingFrameCsvSplitResult perf_result =
        orange::session::append_clip_rows_from_root_csv(
            chain->root_perf_csv_path,
            &chain->perf_cursor,
            range,
            result.perf_shadow_path,
            orange::session::RecordingFrameCsvOrphanRowPolicy::kCountAndSkip);
    result.perf_rows = perf_result.clip_rows_written;
    if (perf_result.status ==
        orange::session::RecordingFrameCsvClipSplitStatus::kFailed) {
        result.status = GuiShadowClipAttemptStatus::kFailed;
        result.error = "crop perf shadow split failed: " + perf_result.error;
        return result;
    }

    const bool completed =
        meta_result.status ==
            orange::session::RecordingFrameCsvClipSplitStatus::kCompleted &&
        perf_result.status ==
            orange::session::RecordingFrameCsvClipSplitStatus::kCompleted;
    result.status = completed
        ? GuiShadowClipAttemptStatus::kCompleted
        : GuiShadowClipAttemptStatus::kIncomplete;
    return result;
}

std::string gui_shadow_index_path(const std::string& recording_folder)
{
    return (std::filesystem::path(recording_folder) /
            "recording_clip_index.shadow.jsonl").string();
}

std::string gui_shadow_clip_directory(const std::string& clips_root,
                                      const int clip_index)
{
    return (std::filesystem::path(clips_root) /
            gui_shadow_clip_id(clip_index)).string();
}

std::string gui_shadow_meta_csv_path(const std::string& clips_root,
                                     const int clip_index,
                                     const std::string& camera_serial)
{
    return (std::filesystem::path(
                gui_shadow_clip_directory(clips_root, clip_index)) /
            ("Cam" + camera_serial + "_crop_meta.shadow.csv")).string();
}

std::string gui_shadow_perf_csv_path(const std::string& clips_root,
                                     const int clip_index,
                                     const std::string& camera_serial)
{
    return (std::filesystem::path(
                gui_shadow_clip_directory(clips_root, clip_index)) /
            ("Cam" + camera_serial + "_crop_perf.shadow.csv")).string();
}

std::string gui_shadow_sibling_csv_path(const std::string& authoritative_csv_path)
{
    const std::string suffix = ".csv";
    if (authoritative_csv_path.size() <= suffix.size() ||
        authoritative_csv_path.compare(
            authoritative_csv_path.size() - suffix.size(),
            suffix.size(),
            suffix) != 0) {
        return {};
    }
    return authoritative_csv_path.substr(
               0, authoritative_csv_path.size() - suffix.size()) +
           ".shadow.csv";
}

nlohmann::json gui_shadow_clip_completed_index_line(
    const std::string& camera_serial,
    const GuiShadowClipAnnouncement& clip,
    const uint64_t meta_rows,
    const uint64_t perf_rows,
    const std::string& meta_shadow_path,
    const std::string& perf_shadow_path,
    const std::string& wall_clock_utc)
{
    return {
        {"event", "clip_completed"},
        {"clip_index", clip.clip_index},
        {"camera_serial", camera_serial},
        {"first_recording_frame_id", clip.first_recording_frame_id},
        {"last_recording_frame_id", clip.last_recording_frame_id},
        {"meta_rows", meta_rows},
        {"perf_rows", perf_rows},
        {"files", nlohmann::json::array({meta_shadow_path, perf_shadow_path})},
        {"wall_clock_utc", wall_clock_utc},
    };
}

nlohmann::json gui_shadow_chain_parked_index_line(
    const std::string& camera_serial,
    const int clip_index,
    const std::string& reason,
    const std::string& wall_clock_utc)
{
    return {
        {"event", "chain_parked"},
        {"camera_serial", camera_serial},
        {"clip_index", clip_index},
        {"reason", reason},
        {"wall_clock_utc", wall_clock_utc},
    };
}

bool gui_shadow_append_index_line(const std::string& index_path,
                                  const nlohmann::json& line,
                                  std::string* error_out)
{
    if (index_path.empty()) {
        if (error_out) {
            *error_out = "shadow index path is empty";
        }
        return false;
    }
    std::ofstream output(index_path, std::ios::out | std::ios::app);
    if (!output) {
        if (error_out) {
            *error_out = "failed to open shadow index: " + index_path;
        }
        return false;
    }
    output << line.dump() << '\n';
    output.flush();
    if (!output) {
        if (error_out) {
            *error_out = "failed while writing shadow index: " + index_path;
        }
        return false;
    }
    return true;
}

namespace {

bool gui_shadow_files_identical(const std::string& lhs_path,
                                const std::string& rhs_path)
{
    std::ifstream lhs(lhs_path, std::ios::in | std::ios::binary);
    std::ifstream rhs(rhs_path, std::ios::in | std::ios::binary);
    if (!lhs || !rhs) {
        return false;
    }
    char lhs_chunk[64 * 1024];
    char rhs_chunk[64 * 1024];
    for (;;) {
        lhs.read(lhs_chunk, sizeof(lhs_chunk));
        rhs.read(rhs_chunk, sizeof(rhs_chunk));
        const std::streamsize lhs_got = lhs.gcount();
        const std::streamsize rhs_got = rhs.gcount();
        if (lhs_got != rhs_got) {
            return false;
        }
        if (lhs_got == 0) {
            return true;
        }
        if (std::memcmp(lhs_chunk, rhs_chunk, static_cast<size_t>(lhs_got)) != 0) {
            return false;
        }
    }
}

}  // namespace

GuiShadowCrossCheckOutcome gui_cross_check_incremental_clip_shadow(
    const std::string& shadow_index_path,
    const std::vector<std::string>& authoritative_csv_paths)
{
    GuiShadowCrossCheckOutcome outcome;
    nlohmann::json mismatched_files = nlohmann::json::array();
    nlohmann::json missing_files = nlohmann::json::array();
    for (const std::string& authoritative_path : authoritative_csv_paths) {
        const std::string shadow_path =
            gui_shadow_sibling_csv_path(authoritative_path);
        if (shadow_path.empty()) {
            continue;
        }
        std::error_code exists_error;
        if (!std::filesystem::exists(authoritative_path, exists_error)) {
            // The authoritative split did not produce this file; there is
            // nothing to judge the shadow output against.
            continue;
        }
        ++outcome.total;
        if (!std::filesystem::exists(shadow_path, exists_error)) {
            ++outcome.missing;
            missing_files.push_back(shadow_path);
            continue;
        }
        if (gui_shadow_files_identical(authoritative_path, shadow_path)) {
            ++outcome.identical;
        } else {
            ++outcome.mismatched;
            mismatched_files.push_back(shadow_path);
        }
    }
    outcome.summary =
        "shadow cross-check: " + std::to_string(outcome.identical) + "/" +
        std::to_string(outcome.total) + " clip CSVs identical, " +
        std::to_string(outcome.mismatched) + " mismatched, " +
        std::to_string(outcome.missing) + " missing";

    std::string append_error;
    if (!gui_shadow_append_index_line(
            shadow_index_path,
            {
                {"event", "cross_check"},
                {"total", outcome.total},
                {"identical", outcome.identical},
                {"mismatched", outcome.mismatched},
                {"missing", outcome.missing},
                {"mismatched_files", std::move(mismatched_files)},
                {"missing_files", std::move(missing_files)},
                {"summary", outcome.summary},
                {"wall_clock_utc", gui_shadow_utc_now()},
            },
            &append_error)) {
        std::cerr << "[GUI][clip_shadow] WARNING: failed to record cross-check"
                     " in shadow index " << shadow_index_path << ": "
                  << append_error << std::endl;
    }
    return outcome;
}

GuiIncrementalClipShadowState::~GuiIncrementalClipShadowState()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        stop_requested = true;
    }
    cv.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
}

bool gui_start_incremental_clip_shadow(
    GuiIncrementalClipShadowState* state,
    std::vector<GuiShadowCameraChainState> chains,
    const std::string& recording_folder)
{
    if (!state || state->active || chains.empty() || recording_folder.empty()) {
        return false;
    }
    if (state->worker.joinable()) {
        state->worker.join();  // Defensive; joins run at finalize/teardown.
    }
    state->recording_folder = recording_folder;
    state->index_path = gui_shadow_index_path(recording_folder);
    state->chains = std::move(chains);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stop_requested = false;
        state->push_sequence = 0;
        state->slots.clear();
        state->slots.reserve(state->chains.size());
        for (const GuiShadowCameraChainState& chain : state->chains) {
            state->slots.push_back({chain.camera_serial, {}});
        }
    }
    state->clips_completed.store(0, std::memory_order_relaxed);
    state->chains_parked.store(0, std::memory_order_relaxed);
    state->done.store(false, std::memory_order_release);
    state->active = true;
    GuiIncrementalClipShadowState* worker_state = state;
    state->worker = std::thread([worker_state]() {
        gui_incremental_clip_shadow_worker(worker_state);
    });
    std::cout << "[GUI][clip_shadow] Shadow-mode incremental clip splitter"
                 " started (" << kGuiIncrementalClipShadowEnvFlag << ")"
              << " cameras=" << state->chains.size()
              << " index=" << state->index_path
              << std::endl;
    return true;
}

void gui_push_incremental_clip_shadow_announcement(
    GuiIncrementalClipShadowState* state,
    const std::string& camera_serial,
    const GuiShadowClipAnnouncement& announcement)
{
    if (!state || !state->active || !announcement.valid()) {
        return;
    }
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (GuiShadowFeedSlot& slot : state->slots) {
            if (slot.camera_serial == camera_serial &&
                announcement.clip_index > slot.latest.clip_index) {
                slot.latest = announcement;
                ++state->push_sequence;
                changed = true;
                break;
            }
        }
    }
    if (changed) {
        state->cv.notify_one();
    }
}

bool gui_maybe_start_incremental_clip_shadow(
    GuiIncrementalClipShadowState* state,
    const orange::external_recorder::SupervisedRecorderLifecycleState& crop_lifecycle,
    const std::string& recording_folder,
    const bool recording_active)
{
    if (!state || state->active || !recording_active || recording_folder.empty()) {
        return false;
    }
    if (state->last_recording_folder == recording_folder) {
        return false;  // Start decision already made for this run.
    }
    if (!gui_incremental_clip_shadow_enabled()) {
        return false;
    }
    if (!crop_lifecycle.started) {
        return false;
    }

    // The crop-CSV clip ranges are driven by the CROP recorder's completed
    // clips (the authoritative finalize builds its split ranges from the
    // crop recorder's summary); mirror exactly that here by watching only
    // the crop lifecycle's rolling streams.
    std::vector<GuiShadowCameraChainState> chains;
    for (const orange::external_recorder::RecorderStreamPlan& stream :
         crop_lifecycle.plan.streams) {
        if (stream.stream_kind != "crop" && stream.output_kind != "crop") {
            continue;
        }
        if (stream.clip_seconds <= 0) {
            continue;  // Not a rolling stream: nothing to shadow-split.
        }
        const std::string serial = gui_shadow_strip_crop_suffix(
            !stream.camera_serial.empty() ? stream.camera_serial
                                          : stream.stream_id);
        if (serial.empty()) {
            continue;
        }
        GuiShadowCameraChainState chain;
        chain.camera_serial = serial;
        chain.root_meta_csv_path =
            (std::filesystem::path(recording_folder) /
             ("Cam" + serial + "_crop_meta.csv")).string();
        chain.root_perf_csv_path =
            (std::filesystem::path(recording_folder) /
             ("Cam" + serial + "_crop_perf.csv")).string();
        // The recorder derives clip directories from its mp4 path
        // (dirname(mp4)/clips/clip_NNNNNN); mirror that so the shadow CSVs
        // land next to the authoritative per-clip outputs.
        chain.clips_root = stream.mp4.empty()
            ? (std::filesystem::path(recording_folder) / "clips").string()
            : (std::filesystem::path(stream.mp4).parent_path() / "clips").string();
        chains.push_back(std::move(chain));
    }

    // Record the decision for this run either way so the per-frame hook
    // does not rescan the plan every frame of a non-rolling recording.
    state->last_recording_folder = recording_folder;
    if (chains.empty()) {
        return false;  // Flag on but the recording is not external rolling.
    }
    return gui_start_incremental_clip_shadow(
        state, std::move(chains), recording_folder);
}

void gui_push_incremental_clip_shadow_watermarks(
    GuiIncrementalClipShadowState* state,
    const orange::external_recorder::SupervisedRecorderLifecycleState& crop_lifecycle)
{
    if (!state || !state->active) {
        return;
    }
    for (const orange::external_recorder::RecorderProcessState& process :
         crop_lifecycle.runtime.processes) {
        const orange::external_recorder::RecorderStatusSnapshot& status =
            process.recorder_status;
        if (!status.valid || !status.rolling_enabled ||
            status.rolling_last_completed_clip_index < 0) {
            continue;
        }
        GuiShadowClipAnnouncement announcement;
        announcement.clip_index = status.rolling_last_completed_clip_index;
        announcement.first_recording_frame_id =
            status.rolling_last_completed_clip_first_recording_frame_id;
        announcement.last_recording_frame_id =
            status.rolling_last_completed_clip_last_recording_frame_id;
        gui_push_incremental_clip_shadow_announcement(
            state,
            gui_shadow_strip_crop_suffix(
                !process.camera_serial.empty() ? process.camera_serial
                                               : process.stream_id),
            announcement);
    }
}

void gui_join_incremental_clip_shadow(GuiIncrementalClipShadowState* state)
{
    if (!state) {
        return;
    }
    if (!state->active && !state->worker.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stop_requested = true;
    }
    state->cv.notify_all();
    if (state->worker.joinable()) {
        state->worker.join();
    }
    if (state->active) {
        std::cout << "[GUI][clip_shadow] Shadow worker joined:"
                  << " clips_completed="
                  << state->clips_completed.load(std::memory_order_relaxed)
                  << " chains_parked="
                  << state->chains_parked.load(std::memory_order_relaxed)
                  << " index=" << state->index_path
                  << std::endl;
    }
    state->active = false;
}
