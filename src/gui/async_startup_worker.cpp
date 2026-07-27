#include "gui/async_startup_worker.h"

#include <exception>
#include <utility>

namespace orange::gui {

GuiAsyncStartupWorkResult GuiAsyncStartupWorkResult::Succeeded()
{
    GuiAsyncStartupWorkResult result;
    result.success = true;
    return result;
}

GuiAsyncStartupWorkResult GuiAsyncStartupWorkResult::Failed(std::string error)
{
    GuiAsyncStartupWorkResult result;
    result.error = std::move(error);
    return result;
}

GuiAsyncStartupWorkResult GuiAsyncStartupWorkResult::Canceled(std::string reason)
{
    GuiAsyncStartupWorkResult result;
    result.canceled = true;
    result.error = std::move(reason);
    return result;
}

GuiAsyncStartupWorker::~GuiAsyncStartupWorker()
{
    Shutdown();
}

bool GuiAsyncStartupWorker::Start(
    std::string operation,
    Work work,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!work) {
        if (error_out) *error_out = "startup work callback is empty";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_ || worker_.joinable()) {
            if (error_out) *error_out = "another startup operation is active";
            return false;
        }
        operation_ = std::move(operation);
        result_ = {};
        cancel_requested_.store(false, std::memory_order_release);
        worker_finished_.store(false, std::memory_order_release);
        running_ = true;
    }

    try {
        worker_ = std::thread([this, work = std::move(work)]() mutable {
            GuiAsyncStartupWorkResult result;
            try {
                result = work(cancel_requested_);
            } catch (const std::exception& error) {
                result = GuiAsyncStartupWorkResult::Failed(error.what());
            } catch (...) {
                result = GuiAsyncStartupWorkResult::Failed(
                    "startup worker threw a non-standard exception");
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                result_ = std::move(result);
            }
            worker_finished_.store(true, std::memory_order_release);
        });
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        operation_.clear();
        if (error_out) *error_out = error.what();
        return false;
    }
    return true;
}

void GuiAsyncStartupWorker::RequestCancel() noexcept
{
    cancel_requested_.store(true, std::memory_order_release);
}

void GuiAsyncStartupWorker::JoinFinishedWorker()
{
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::optional<GuiAsyncStartupCompletion> GuiAsyncStartupWorker::Poll()
{
    if (!worker_finished_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }

    JoinFinishedWorker();
    GuiAsyncStartupCompletion completion;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        completion.operation = operation_;
        completion.result = std::move(result_);
        result_ = {};
        operation_.clear();
        running_ = false;
        worker_finished_.store(false, std::memory_order_release);
    }
    return completion;
}

void GuiAsyncStartupWorker::Shutdown() noexcept
{
    RequestCancel();
    try {
        JoinFinishedWorker();
    } catch (...) {
    }
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    operation_.clear();
    result_ = {};
    worker_finished_.store(false, std::memory_order_release);
}

bool GuiAsyncStartupWorker::running() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

bool GuiAsyncStartupWorker::cancel_requested() const noexcept
{
    return cancel_requested_.load(std::memory_order_acquire);
}

std::string GuiAsyncStartupWorker::operation() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return operation_;
}

}  // namespace orange::gui
