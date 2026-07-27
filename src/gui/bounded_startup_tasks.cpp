#include "gui/bounded_startup_tasks.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace orange::gui {
namespace {

void update_peak(
    std::atomic<std::size_t>* peak,
    const std::size_t candidate) noexcept
{
    std::size_t observed = peak->load(std::memory_order_relaxed);
    while (observed < candidate &&
           !peak->compare_exchange_weak(
               observed,
               candidate,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

GuiAsyncStartupWorkResult exception_result()
{
    try {
        throw;
    } catch (const std::exception& error) {
        return GuiAsyncStartupWorkResult::Failed(error.what());
    } catch (...) {
        return GuiAsyncStartupWorkResult::Failed(
            "bounded startup task threw a non-standard exception");
    }
}

}  // namespace

struct GuiStartupCancellationFactory {
    static GuiStartupCancellation Make(
        const std::atomic<bool>* external,
        const std::atomic<bool>* peer_stop) noexcept
    {
        GuiStartupCancellation cancellation;
        cancellation.external_ = external;
        cancellation.peer_stop_ = peer_stop;
        return cancellation;
    }
};

bool GuiStartupCancellation::requested() const noexcept
{
    return external_requested() || peer_stop_requested();
}

bool GuiStartupCancellation::external_requested() const noexcept
{
    return external_ && external_->load(std::memory_order_acquire);
}

bool GuiStartupCancellation::peer_stop_requested() const noexcept
{
    return peer_stop_ && peer_stop_->load(std::memory_order_acquire);
}

bool GuiBoundedStartupTaskGroupResult::all_succeeded() const noexcept
{
    return !launch_failed && completed_task_count == task_states.size() &&
        std::all_of(
            task_states.begin(),
            task_states.end(),
            [](const GuiBoundedStartupTaskState state) {
                return state == GuiBoundedStartupTaskState::kSucceeded;
            });
}

bool GuiBoundedStartupTaskGroupResult::any_failed() const noexcept
{
    return launch_failed || std::any_of(
        task_states.begin(),
        task_states.end(),
        [](const GuiBoundedStartupTaskState state) {
            return state == GuiBoundedStartupTaskState::kFailed;
        });
}

std::size_t SanitizeGuiCameraOpenConcurrency(
    const int requested_concurrency,
    const std::size_t task_count,
    bool* requested_value_supported) noexcept
{
    const bool supported = requested_concurrency == 1 ||
        requested_concurrency == 2 || requested_concurrency == 4;
    if (requested_value_supported) {
        *requested_value_supported = supported;
    }
    if (task_count == 0) {
        return 0;
    }
    const std::size_t accepted = supported
        ? static_cast<std::size_t>(requested_concurrency)
        : std::size_t{1};
    return std::min(accepted, task_count);
}

GuiBoundedStartupTaskGroupResult RunGuiBoundedStartupTasks(
    const std::size_t task_count,
    const std::size_t max_concurrency,
    const std::atomic<bool>& external_cancel_requested,
    GuiBoundedStartupTask task)
{
    GuiBoundedStartupTaskGroupResult result;
    result.requested_concurrency = std::max<std::size_t>(1, max_concurrency);
    result.effective_concurrency = task_count == 0
        ? 0
        : std::min(result.requested_concurrency, task_count);
    result.task_results.resize(task_count);
    result.task_states.assign(
        task_count, GuiBoundedStartupTaskState::kNotStarted);

    if (task_count == 0) {
        return result;
    }
    if (!task) {
        result.launch_failed = true;
        result.launch_error = "bounded startup task callback is empty";
        return result;
    }

    std::atomic<std::size_t> next_task{0};
    std::atomic<std::size_t> active_tasks{0};
    std::atomic<std::size_t> peak_tasks{0};
    std::atomic<std::size_t> started_tasks{0};
    std::atomic<std::size_t> completed_tasks{0};
    std::atomic<bool> peer_stop_requested{false};
    const GuiStartupCancellation cancellation =
        GuiStartupCancellationFactory::Make(
            &external_cancel_requested, &peer_stop_requested);

    auto worker = [&]() {
        while (!cancellation.requested()) {
            const std::size_t index =
                next_task.fetch_add(1, std::memory_order_relaxed);
            if (index >= task_count) {
                return;
            }
            // A peer can fail between the queue claim and task entry. Leave
            // that deterministic slot as not-started rather than beginning
            // more SDK work after the transaction has already failed.
            if (cancellation.requested()) {
                return;
            }

            result.task_states[index] = GuiBoundedStartupTaskState::kRunning;
            started_tasks.fetch_add(1, std::memory_order_relaxed);
            const std::size_t active =
                active_tasks.fetch_add(1, std::memory_order_acq_rel) + 1;
            update_peak(&peak_tasks, active);

            GuiAsyncStartupWorkResult outcome;
            try {
                outcome = task(index, cancellation);
            } catch (...) {
                outcome = exception_result();
            }
            result.task_results[index] = outcome;
            if (outcome.success) {
                result.task_states[index] =
                    GuiBoundedStartupTaskState::kSucceeded;
            } else if (outcome.canceled) {
                result.task_states[index] =
                    GuiBoundedStartupTaskState::kCanceled;
            } else {
                result.task_states[index] =
                    GuiBoundedStartupTaskState::kFailed;
            }
            active_tasks.fetch_sub(1, std::memory_order_acq_rel);
            completed_tasks.fetch_add(1, std::memory_order_relaxed);

            if (!outcome.success) {
                peer_stop_requested.store(true, std::memory_order_release);
                return;
            }
        }
    };

    std::vector<std::thread> workers;
    if (result.effective_concurrency == 1) {
        // Preserve the original execution shape for the default/fallback:
        // the outer joinable startup worker performs serial SDK calls itself.
        worker();
    } else {
        try {
            workers.reserve(result.effective_concurrency);
            for (std::size_t i = 0; i < result.effective_concurrency; ++i) {
                workers.emplace_back(worker);
            }
        } catch (const std::exception& error) {
            result.launch_failed = true;
            result.launch_error = error.what();
            peer_stop_requested.store(true, std::memory_order_release);
        } catch (...) {
            result.launch_failed = true;
            result.launch_error =
                "bounded startup worker launch threw a non-standard exception";
            peer_stop_requested.store(true, std::memory_order_release);
        }
    }

    for (std::thread& thread : workers) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    result.peak_concurrency = peak_tasks.load(std::memory_order_relaxed);
    result.started_task_count = started_tasks.load(std::memory_order_relaxed);
    result.completed_task_count = completed_tasks.load(std::memory_order_relaxed);
    result.external_cancel_observed =
        external_cancel_requested.load(std::memory_order_acquire);
    return result;
}

const char* GuiBoundedStartupTaskStateName(
    const GuiBoundedStartupTaskState state) noexcept
{
    switch (state) {
        case GuiBoundedStartupTaskState::kNotStarted: return "not_started";
        case GuiBoundedStartupTaskState::kRunning: return "running";
        case GuiBoundedStartupTaskState::kSucceeded: return "succeeded";
        case GuiBoundedStartupTaskState::kFailed: return "failed";
        case GuiBoundedStartupTaskState::kCanceled: return "canceled";
    }
    return "unknown";
}

}  // namespace orange::gui
