#pragma once

#include "gui/async_startup_worker.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace orange::gui {

// Read-only cancellation view passed to one bounded startup task. The caller's
// cancellation request and a peer-task failure are both visible. Tasks should
// check it between non-interruptible SDK calls; RunGuiBoundedStartupTasks()
// always joins tasks that are already inside such a call.
class GuiStartupCancellation {
public:
    bool requested() const noexcept;
    bool external_requested() const noexcept;
    bool peer_stop_requested() const noexcept;

private:
    friend struct GuiStartupCancellationFactory;
    const std::atomic<bool>* external_ = nullptr;
    const std::atomic<bool>* peer_stop_ = nullptr;
};

enum class GuiBoundedStartupTaskState : std::uint8_t {
    kNotStarted,
    kRunning,
    kSucceeded,
    kFailed,
    kCanceled,
};

struct GuiBoundedStartupTaskGroupResult {
    std::vector<GuiAsyncStartupWorkResult> task_results;
    std::vector<GuiBoundedStartupTaskState> task_states;
    std::size_t requested_concurrency = 1;
    std::size_t effective_concurrency = 0;
    std::size_t peak_concurrency = 0;
    std::size_t started_task_count = 0;
    std::size_t completed_task_count = 0;
    bool external_cancel_observed = false;
    bool launch_failed = false;
    std::string launch_error;

    bool all_succeeded() const noexcept;
    bool any_failed() const noexcept;
};

using GuiBoundedStartupTask = std::function<GuiAsyncStartupWorkResult(
    std::size_t task_index,
    const GuiStartupCancellation& cancellation)>;

// Supported experimental camera-open widths. Invalid values deliberately fall
// back to one worker rather than silently enabling an untested topology.
std::size_t SanitizeGuiCameraOpenConcurrency(
    int requested_concurrency,
    std::size_t task_count,
    bool* requested_value_supported = nullptr) noexcept;

// Runs task_count indexed tasks with bounded concurrency and deterministic
// result slots. A task failure stops new work from being scheduled, but every
// already-started task is joined before this function returns.
GuiBoundedStartupTaskGroupResult RunGuiBoundedStartupTasks(
    std::size_t task_count,
    std::size_t max_concurrency,
    const std::atomic<bool>& external_cancel_requested,
    GuiBoundedStartupTask task);

const char* GuiBoundedStartupTaskStateName(
    GuiBoundedStartupTaskState state) noexcept;

}  // namespace orange::gui
