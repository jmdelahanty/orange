#include "gui/bounded_startup_tasks.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using orange::gui::GuiAsyncStartupWorkResult;
using orange::gui::GuiBoundedStartupTaskGroupResult;
using orange::gui::GuiBoundedStartupTaskState;
using orange::gui::GuiStartupCancellation;
using orange::gui::RunGuiBoundedStartupTasks;
using orange::gui::SanitizeGuiStartupConcurrency;

void require(const bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

template <typename Predicate>
void wait_until(Predicate predicate, const std::string& description)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    require(predicate(), "timed out waiting for " + description);
}

void test_startup_concurrency_sanitization()
{
    bool supported = false;
    require(
        SanitizeGuiStartupConcurrency(1, 4, &supported) == 1 && supported,
        "one worker is supported");
    require(
        SanitizeGuiStartupConcurrency(2, 4, &supported) == 2 && supported,
        "two workers are supported");
    require(
        SanitizeGuiStartupConcurrency(4, 3, &supported) == 3 && supported,
        "four workers clamp to selected camera count");
    require(
        SanitizeGuiStartupConcurrency(3, 4, &supported) == 1 && !supported,
        "untested widths fall back to serial");
    require(
        SanitizeGuiStartupConcurrency(4, 0, &supported) == 0 && supported,
        "an empty task set has no workers");
}

void test_bounded_parallelism_and_complete_results()
{
    std::atomic<bool> cancel{false};
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    std::atomic<int> first_wave_entered{0};
    std::vector<int> observations(8, -1);
    const GuiBoundedStartupTaskGroupResult result =
        RunGuiBoundedStartupTasks(
            observations.size(),
            2,
            cancel,
            [&](const std::size_t index, const GuiStartupCancellation&) {
                const int now_active = active.fetch_add(1) + 1;
                int observed_peak = peak.load();
                while (observed_peak < now_active &&
                       !peak.compare_exchange_weak(observed_peak, now_active)) {
                }
                if (index < 2) {
                    first_wave_entered.fetch_add(1);
                    wait_until(
                        [&]() { return first_wave_entered.load() == 2; },
                        "both first-wave tasks");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                observations[index] = static_cast<int>(index);
                active.fetch_sub(1);
                return GuiAsyncStartupWorkResult::Succeeded();
            });

    require(result.all_succeeded(), "all bounded tasks succeed");
    require(result.effective_concurrency == 2, "effective width retained");
    require(result.peak_concurrency == 2, "group records bounded overlap");
    require(peak.load() == 2, "fixture observed two simultaneous tasks");
    require(result.started_task_count == observations.size(), "all tasks started");
    require(
        result.completed_task_count == observations.size(),
        "all tasks completed");
    for (std::size_t i = 0; i < observations.size(); ++i) {
        require(observations[i] == static_cast<int>(i), "result index is stable");
        require(
            result.task_states[i] == GuiBoundedStartupTaskState::kSucceeded,
            "task state is stored in input order");
    }
}

void test_peer_failure_stops_pending_work_and_joins_running_work()
{
    std::atomic<bool> cancel{false};
    std::atomic<int> entered{0};
    std::atomic<bool> peer_observed_stop{false};
    const GuiBoundedStartupTaskGroupResult result =
        RunGuiBoundedStartupTasks(
            6,
            2,
            cancel,
            [&](const std::size_t index,
                const GuiStartupCancellation& cancellation) {
                entered.fetch_add(1);
                if (index == 0) {
                    wait_until(
                        [&]() { return entered.load() >= 2; },
                        "second peer to enter");
                    return GuiAsyncStartupWorkResult::Failed("camera_0_failed");
                }
                wait_until(
                    [&]() { return cancellation.requested(); },
                    "peer failure cancellation");
                peer_observed_stop.store(
                    cancellation.peer_stop_requested(),
                    std::memory_order_release);
                return GuiAsyncStartupWorkResult::Canceled("peer_failed");
            });

    require(result.any_failed(), "peer failure is retained");
    require(result.started_task_count == 2, "pending tasks were not started");
    require(result.completed_task_count == 2, "both running tasks were joined");
    require(peer_observed_stop.load(), "running peer observed group stop");
    require(
        result.task_states[0] == GuiBoundedStartupTaskState::kFailed,
        "failing task is identified");
    require(
        result.task_states[1] == GuiBoundedStartupTaskState::kCanceled,
        "already-running peer exits as canceled");
    for (std::size_t i = 2; i < result.task_states.size(); ++i) {
        require(
            result.task_states[i] == GuiBoundedStartupTaskState::kNotStarted,
            "unclaimed work stays not-started");
    }
}

void test_external_cancel_is_observed_and_all_threads_join()
{
    std::atomic<bool> cancel{false};
    std::atomic<int> entered{0};
    GuiBoundedStartupTaskGroupResult result;
    std::thread runner([&]() {
        result = RunGuiBoundedStartupTasks(
            5,
            2,
            cancel,
            [&](const std::size_t,
                const GuiStartupCancellation& cancellation) {
                entered.fetch_add(1);
                wait_until(
                    [&]() { return cancellation.external_requested(); },
                    "external cancellation");
                return GuiAsyncStartupWorkResult::Canceled("external_cancel");
            });
    });
    wait_until([&]() { return entered.load() == 2; }, "two active tasks");
    cancel.store(true, std::memory_order_release);
    runner.join();

    require(result.external_cancel_observed, "external cancel is recorded");
    require(result.started_task_count == 2, "cancel prevents pending starts");
    require(result.completed_task_count == 2, "active tasks are joined");
    require(result.peak_concurrency == 2, "cancel path retained overlap metric");
}

void test_task_exception_becomes_indexed_failure()
{
    std::atomic<bool> cancel{false};
    const GuiBoundedStartupTaskGroupResult result =
        RunGuiBoundedStartupTasks(
            1,
            1,
            cancel,
            [](const std::size_t, const GuiStartupCancellation&)
                -> GuiAsyncStartupWorkResult {
                throw std::runtime_error("sdk fixture exception");
            });
    require(result.any_failed(), "task exception fails the group");
    require(
        result.task_states[0] == GuiBoundedStartupTaskState::kFailed,
        "exception has failed task state");
    require(
        result.task_results[0].error == "sdk fixture exception",
        "exception text remains attached to its task index");
}

}  // namespace

int main()
{
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"startup_concurrency_sanitization",
         test_startup_concurrency_sanitization},
        {"bounded_parallelism_and_complete_results",
         test_bounded_parallelism_and_complete_results},
        {"peer_failure_stops_pending_work_and_joins_running_work",
         test_peer_failure_stops_pending_work_and_joins_running_work},
        {"external_cancel_is_observed_and_all_threads_join",
         test_external_cancel_is_observed_and_all_threads_join},
        {"task_exception_becomes_indexed_failure",
         test_task_exception_becomes_indexed_failure},
    };
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
