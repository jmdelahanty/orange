// Tests for the GUI's post-finalize validator scheduling
// (src/gui/post_finalize_validation.h): the validator waits for Citrus's
// receipts, or the receipt wait limit, before it starts (2026-11-22: in
// keep-open mode it started before the receipts and reported a
// stimulus_experiment recording without a finalized Citrus binding).
// The child-process start is replaced, so no validator runs.

#include "gui/post_finalize_validation.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::string>& started()
{
    static std::vector<std::string> folders;
    return folders;
}

void install_fake_starter()
{
    started().clear();
    orange::gui::SetPostFinalizeValidationStarterForTesting(
        [](const std::string& folder) { started().push_back(folder); });
}

void test_may_start_rule()
{
    using orange::gui::PostFinalizeValidationMayStart;
    using orange::gui::PostFinalizeValidationReceiptWait;
    PostFinalizeValidationReceiptWait none;
    require(PostFinalizeValidationMayStart(none, std::chrono::seconds(0)),
            "no receipts due: start at once");
    PostFinalizeValidationReceiptWait due;
    due.expected = true;
    due.limit = std::chrono::seconds(600);
    bool stored = false;
    due.finalized = [&stored]() { return stored; };
    require(!PostFinalizeValidationMayStart(due, std::chrono::seconds(5)),
            "receipts due and not stored: wait");
    stored = true;
    require(PostFinalizeValidationMayStart(due, std::chrono::seconds(5)),
            "receipts stored: start");
    stored = false;
    require(PostFinalizeValidationMayStart(due, std::chrono::seconds(600)),
            "limit passed: start anyway");
}

void test_no_receipts_due_starts_immediately()
{
    install_fake_starter();
    orange::gui::SchedulePostFinalizeValidation("/tmp/rec_only", {});
    require(started().size() == 1 && started()[0] == "/tmp/rec_only",
            "a recording_only folder validates at once");
}

void test_waits_for_receipts_then_starts()
{
    install_fake_starter();
    bool stored = false;
    orange::gui::PostFinalizeValidationReceiptWait wait;
    wait.expected = true;
    wait.limit = std::chrono::seconds(1254);
    wait.finalized = [&stored]() { return stored; };
    orange::gui::SchedulePostFinalizeValidation("/tmp/bound", wait);
    require(started().empty(), "a bound recording must not validate before its receipts");
    require(orange::gui::PostFinalizeValidationStatus().status == "waiting_for_receipts",
            "the panel shows the receipt wait");
    for (int i = 0; i < 3; ++i) {
        orange::gui::PollPostFinalizeValidation();
    }
    require(started().empty(), "polling without receipts keeps waiting");
    stored = true;
    orange::gui::PollPostFinalizeValidation();
    require(started().size() == 1 && started()[0] == "/tmp/bound",
            "the validator starts on the poll after the receipts are stored");
    orange::gui::PollPostFinalizeValidation();
    require(started().size() == 1, "it starts exactly once");
}

void test_limit_releases_the_wait()
{
    install_fake_starter();
    orange::gui::PostFinalizeValidationReceiptWait wait;
    wait.expected = true;
    wait.limit = std::chrono::seconds(1);
    wait.finalized = []() { return false; };
    orange::gui::SchedulePostFinalizeValidation("/tmp/bound_late", wait);
    orange::gui::PollPostFinalizeValidation();
    require(started().empty(), "before the limit it waits");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    orange::gui::PollPostFinalizeValidation();
    require(started().size() == 1 && started()[0] == "/tmp/bound_late",
            "past the receipt wait limit it validates anyway");
}

void test_receipts_already_stored_start_at_once()
{
    install_fake_starter();
    orange::gui::PostFinalizeValidationReceiptWait wait;
    wait.expected = true;
    wait.limit = std::chrono::seconds(600);
    wait.finalized = []() { return true; };
    orange::gui::SchedulePostFinalizeValidation("/tmp/bound_ready", wait);
    require(started().size() == 1, "receipts already stored: start at schedule");
}

void test_cancel_and_exit_flush()
{
    install_fake_starter();
    orange::gui::PostFinalizeValidationReceiptWait wait;
    wait.expected = true;
    wait.limit = std::chrono::seconds(600);
    wait.finalized = []() { return false; };
    orange::gui::SchedulePostFinalizeValidation("/tmp/bound_cancel", wait);
    orange::gui::CancelPostFinalizeValidation("a recording started before it finished");
    require(orange::gui::PostFinalizeValidationStatus().status == "cancelled",
            "a recording start drops a validation waiting for receipts");
    orange::gui::PollPostFinalizeValidation();
    orange::gui::StartScheduledPostFinalizeValidationNow("GUI exiting");
    require(started().empty(), "a cancelled validation never starts");

    orange::gui::SchedulePostFinalizeValidation("/tmp/bound_exit", wait);
    orange::gui::StartScheduledPostFinalizeValidationNow("GUI exiting");
    require(started().size() == 1 && started()[0] == "/tmp/bound_exit",
            "the GUI exit starts a validation still waiting (it outlives the GUI)");
    orange::gui::StartScheduledPostFinalizeValidationNow("GUI exiting");
    require(started().size() == 1, "the exit flush is idempotent");
}

}  // namespace

int main()
{
    struct NamedTest {
        const char* name;
        void (*fn)();
    };
    const NamedTest tests[] = {
        {"may_start_rule", &test_may_start_rule},
        {"no_receipts_due_starts_immediately", &test_no_receipts_due_starts_immediately},
        {"waits_for_receipts_then_starts", &test_waits_for_receipts_then_starts},
        {"limit_releases_the_wait", &test_limit_releases_the_wait},
        {"receipts_already_stored_start_at_once", &test_receipts_already_stored_start_at_once},
        {"cancel_and_exit_flush", &test_cancel_and_exit_flush},
    };
    for (const auto& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << test.name << ": " << e.what() << "\n";
            orange::gui::SetPostFinalizeValidationStarterForTesting(nullptr);
            return EXIT_FAILURE;
        }
    }
    orange::gui::SetPostFinalizeValidationStarterForTesting(nullptr);
    std::cout << "post_finalize_validation_tests passed\n";
    return EXIT_SUCCESS;
}
