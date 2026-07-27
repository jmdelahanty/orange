#include "gui/async_startup_worker.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using orange::gui::GuiAsyncStartupCompletion;
using orange::gui::GuiAsyncStartupWorkResult;
using orange::gui::GuiAsyncStartupWorker;

void require(const bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

GuiAsyncStartupCompletion wait_for_completion(GuiAsyncStartupWorker* worker)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        auto completion = worker->Poll();
        if (completion) return std::move(*completion);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("timed out waiting for startup completion");
}

void test_start_is_nonblocking_and_success_is_polled()
{
    GuiAsyncStartupWorker worker;
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::string error;
    require(
        worker.Start(
            "open",
            [&](const std::atomic<bool>&) {
                entered.store(true, std::memory_order_release);
                while (!release.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                return GuiAsyncStartupWorkResult::Succeeded();
            },
            &error),
        "worker starts");
    require(error.empty(), "start has no error");
    require(worker.running(), "worker reports running");
    require(!worker.Poll().has_value(), "unfinished work is not joined by Poll");

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    require(entered.load(std::memory_order_acquire), "work entered asynchronously");
    release.store(true, std::memory_order_release);
    const GuiAsyncStartupCompletion completion = wait_for_completion(&worker);
    require(completion.operation == "open", "operation identity preserved");
    require(completion.result.success, "success result preserved");
    require(!worker.running(), "worker returns to idle after Poll");
}

void test_exception_becomes_failure()
{
    GuiAsyncStartupWorker worker;
    require(
        worker.Start(
            "throwing",
            [](const std::atomic<bool>&) -> GuiAsyncStartupWorkResult {
                throw std::runtime_error("fixture failure");
            }),
        "throwing work starts");
    const GuiAsyncStartupCompletion completion = wait_for_completion(&worker);
    require(!completion.result.success, "exception is not success");
    require(!completion.result.canceled, "exception is not cancellation");
    require(
        completion.result.error == "fixture failure",
        "exception text is retained");
}

void test_active_work_rejects_overlap_and_worker_is_reusable()
{
    GuiAsyncStartupWorker worker;
    std::atomic<bool> release{false};
    require(
        worker.Start(
            "first",
            [&](const std::atomic<bool>&) {
                while (!release.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                return GuiAsyncStartupWorkResult::Succeeded();
            }),
        "first work starts");
    std::string overlap_error;
    require(
        !worker.Start(
            "overlap",
            [](const std::atomic<bool>&) {
                return GuiAsyncStartupWorkResult::Succeeded();
            },
            &overlap_error),
        "overlapping work is rejected");
    require(!overlap_error.empty(), "overlap reports a reason");

    release.store(true, std::memory_order_release);
    require(
        wait_for_completion(&worker).result.success,
        "first work completes");
    require(
        worker.Start(
            "second",
            [](const std::atomic<bool>&) {
                return GuiAsyncStartupWorkResult::Succeeded();
            }),
        "worker can be reused after completion is consumed");
    const GuiAsyncStartupCompletion second = wait_for_completion(&worker);
    require(second.operation == "second", "reuse has fresh operation identity");
    require(second.result.success, "reused worker completes");
}

void test_cancel_is_observed_and_joined()
{
    GuiAsyncStartupWorker worker;
    std::atomic<bool> entered{false};
    require(
        worker.Start(
            "cancel",
            [&](const std::atomic<bool>& cancel_requested) {
                entered.store(true, std::memory_order_release);
                while (!cancel_requested.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                return GuiAsyncStartupWorkResult::Canceled("test_cancel");
            }),
        "cancelable work starts");
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    worker.RequestCancel();
    const GuiAsyncStartupCompletion completion = wait_for_completion(&worker);
    require(completion.result.canceled, "cancellation result retained");
    require(completion.result.error == "test_cancel", "cancel reason retained");
    require(!worker.running(), "canceled worker is joined");
}

void test_shutdown_requests_cancel_and_joins()
{
    GuiAsyncStartupWorker worker;
    std::atomic<bool> exited{false};
    require(
        worker.Start(
            "shutdown",
            [&](const std::atomic<bool>& cancel_requested) {
                while (!cancel_requested.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                exited.store(true, std::memory_order_release);
                return GuiAsyncStartupWorkResult::Canceled("shutdown");
            }),
        "shutdown work starts");
    worker.Shutdown();
    require(exited.load(std::memory_order_acquire), "shutdown waits for worker exit");
    require(!worker.running(), "shutdown leaves worker idle");
}

}  // namespace

int main()
{
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"start_is_nonblocking_and_success_is_polled",
         test_start_is_nonblocking_and_success_is_polled},
        {"exception_becomes_failure", test_exception_becomes_failure},
        {"active_work_rejects_overlap_and_worker_is_reusable",
         test_active_work_rejects_overlap_and_worker_is_reusable},
        {"cancel_is_observed_and_joined", test_cancel_is_observed_and_joined},
        {"shutdown_requests_cancel_and_joins",
         test_shutdown_requests_cancel_and_joins},
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
