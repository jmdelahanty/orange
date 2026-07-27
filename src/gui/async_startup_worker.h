#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace orange::gui {

struct GuiAsyncStartupWorkResult {
    bool success = false;
    bool canceled = false;
    std::string error;

    static GuiAsyncStartupWorkResult Succeeded();
    static GuiAsyncStartupWorkResult Failed(std::string error);
    static GuiAsyncStartupWorkResult Canceled(std::string reason = {});
};

struct GuiAsyncStartupCompletion {
    std::string operation;
    GuiAsyncStartupWorkResult result;
};

// One bounded, joinable startup worker. It deliberately has no camera or GUI
// dependencies so its cancellation and exception lifecycle can be tested
// without hardware. Completion is consumed from the GUI thread through Poll;
// no worker is detached.
class GuiAsyncStartupWorker {
public:
    using Work = std::function<GuiAsyncStartupWorkResult(
        const std::atomic<bool>& cancel_requested)>;

    GuiAsyncStartupWorker() = default;
    ~GuiAsyncStartupWorker();

    GuiAsyncStartupWorker(const GuiAsyncStartupWorker&) = delete;
    GuiAsyncStartupWorker& operator=(const GuiAsyncStartupWorker&) = delete;

    bool Start(std::string operation, Work work, std::string* error_out = nullptr);
    void RequestCancel() noexcept;
    std::optional<GuiAsyncStartupCompletion> Poll();
    void Shutdown() noexcept;

    bool running() const noexcept;
    bool cancel_requested() const noexcept;
    std::string operation() const;

private:
    void JoinFinishedWorker();

    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> worker_finished_{false};
    bool running_ = false;
    std::string operation_;
    GuiAsyncStartupWorkResult result_;
};

}  // namespace orange::gui
