#include "gui/bounded_startup_tasks.h"
#include "gui/startup_ownership_transaction.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using orange::gui::GuiAsyncStartupWorkResult;
using orange::gui::GuiBoundedStartupTaskState;
using orange::gui::GuiStartupCancellation;
using orange::gui::GuiStartupOwnershipState;
using orange::gui::GuiStartupOwnershipTransaction;
using orange::gui::RunGuiBoundedStartupTasks;

void require(const bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void require_equal(
    const std::vector<int>& actual,
    const std::vector<int>& expected,
    const std::string& message)
{
    require(actual == expected, message);
}

struct RuntimeAudit {
    explicit RuntimeAudit(const std::size_t count)
        : reset_calls(count, 0)
    {
    }

    std::vector<int> reset_order;
    std::vector<int> release_order;
    std::vector<int> reset_calls;
};

struct FakeCameraRuntime {
    void Bind(const int camera_index, RuntimeAudit* runtime_audit) noexcept
    {
        index = camera_index;
        audit = runtime_audit;
    }

    void AcquirePartialResources() noexcept
    {
        owns_resources = true;
    }

    void Reset() noexcept
    {
        if (!audit || index < 0) return;
        audit->reset_order.push_back(index);
        ++audit->reset_calls[static_cast<std::size_t>(index)];
        if (owns_resources) {
            audit->release_order.push_back(index);
            owns_resources = false;
        }
    }

    int index = -1;
    RuntimeAudit* audit = nullptr;
    bool owns_resources = false;
};

void bind_runtimes(
    GuiStartupOwnershipTransaction<FakeCameraRuntime>* transaction,
    RuntimeAudit* audit)
{
    auto& runtimes = transaction->staged_runtimes();
    for (std::size_t index = 0; index < runtimes.size(); ++index) {
        runtimes[index].Bind(static_cast<int>(index), audit);
    }
}

void require_each_reset_once(const RuntimeAudit& audit)
{
    for (const int reset_calls : audit.reset_calls) {
        require(reset_calls == 1, "each staged runtime must reset exactly once");
    }
}

void test_partial_stage_failure_rolls_back_once_in_reverse_order()
{
    RuntimeAudit audit(4);
    GuiStartupOwnershipTransaction<FakeCameraRuntime> transaction(4);
    bind_runtimes(&transaction, &audit);
    std::atomic<bool> external_cancel{false};

    const auto result = RunGuiBoundedStartupTasks(
        4,
        1,
        external_cancel,
        [&](const std::size_t index, const GuiStartupCancellation&) {
            transaction.staged_runtimes()[index].AcquirePartialResources();
            if (index == 2) {
                return GuiAsyncStartupWorkResult::Failed(
                    "injected_buffer_allocation_failure");
            }
            return GuiAsyncStartupWorkResult::Succeeded();
        });

    require(result.any_failed(), "injected stage failure must fail the group");
    require(result.started_task_count == 3, "failure must stop pending work");
    require(
        result.task_states[3] == GuiBoundedStartupTaskState::kNotStarted,
        "camera after the injected failure must remain unstarted");

    transaction.Rollback();
    require(
        transaction.state() == GuiStartupOwnershipState::kRolledBack,
        "failed transaction must enter rolled-back state");
    require_equal(
        audit.reset_order,
        {3, 2, 1, 0},
        "all runtime slots must reset in reverse camera order");
    require_equal(
        audit.release_order,
        {2, 1, 0},
        "only partially acquired resources must be released");
    require_each_reset_once(audit);

    transaction.Rollback();
    require_equal(
        audit.reset_order,
        {3, 2, 1, 0},
        "rollback must be idempotent");
}

void test_canceled_stage_uses_the_same_cleanup_contract()
{
    RuntimeAudit audit(4);
    GuiStartupOwnershipTransaction<FakeCameraRuntime> transaction(4);
    bind_runtimes(&transaction, &audit);
    std::atomic<bool> external_cancel{false};

    const auto result = RunGuiBoundedStartupTasks(
        4,
        1,
        external_cancel,
        [&](const std::size_t index, const GuiStartupCancellation&) {
            transaction.staged_runtimes()[index].AcquirePartialResources();
            if (index == 1) {
                return GuiAsyncStartupWorkResult::Canceled(
                    "injected_operator_cancel");
            }
            return GuiAsyncStartupWorkResult::Succeeded();
        });

    require(!result.all_succeeded(), "canceled stage cannot commit");
    require(
        result.task_states[1] == GuiBoundedStartupTaskState::kCanceled,
        "canceled camera slot must retain its result");
    transaction.Rollback();
    require_equal(
        audit.reset_order,
        {3, 2, 1, 0},
        "cancellation must reset every staged runtime in reverse order");
    require_equal(
        audit.release_order,
        {1, 0},
        "cancellation must release resources acquired before it was observed");
    require_each_reset_once(audit);
}

void test_destructor_is_the_partial_construction_fail_safe()
{
    RuntimeAudit audit(3);
    {
        GuiStartupOwnershipTransaction<FakeCameraRuntime> transaction(3);
        bind_runtimes(&transaction, &audit);
        transaction.staged_runtimes()[0].AcquirePartialResources();
        transaction.staged_runtimes()[2].AcquirePartialResources();
        // Deliberately omit explicit rollback, as an unexpected exception
        // would. The transaction destructor must unwind the staged owners.
    }

    require_equal(
        audit.reset_order,
        {2, 1, 0},
        "destructor rollback must preserve reverse camera order");
    require_equal(
        audit.release_order,
        {2, 0},
        "destructor rollback must release every acquired partial resource");
    require_each_reset_once(audit);
}

void test_install_transfers_sole_ownership_across_activation_failure()
{
    RuntimeAudit audit(3);
    std::vector<FakeCameraRuntime> installed;
    FakeCameraRuntime* staged_address = nullptr;
    {
        GuiStartupOwnershipTransaction<FakeCameraRuntime> transaction(3);
        bind_runtimes(&transaction, &audit);
        for (FakeCameraRuntime& runtime : transaction.staged_runtimes()) {
            runtime.AcquirePartialResources();
        }
        staged_address = transaction.staged_runtimes().data();
        require(
            transaction.InstallInto(&installed),
            "empty GUI destination must accept staged ownership");
        require(
            transaction.state() == GuiStartupOwnershipState::kInstalled,
            "successful handoff must enter installed state");
        require(
            installed.data() == staged_address,
            "installation must preserve runtime addresses");

        // Model an exception in pipeline/thread activation after installation.
        // The staging destructor must not release objects now owned by main.
        transaction.Rollback();
    }

    require(audit.reset_order.empty(), "staging must not reset installed owners");
    require(installed.size() == 3, "GUI must retain every installed owner");

    // This is the ownership portion of main's activation-failure teardown.
    for (std::size_t index = installed.size(); index > 0; --index) {
        installed[index - 1].Reset();
    }
    installed.clear();
    require_equal(
        audit.reset_order,
        {2, 1, 0},
        "installed owner must perform activation-failure rollback once");
    require_equal(
        audit.release_order,
        {2, 1, 0},
        "activation-failure rollback must release every installed resource");
    require_each_reset_once(audit);
}

void test_install_rejection_leaves_staging_ownership_intact()
{
    RuntimeAudit audit(2);
    std::vector<FakeCameraRuntime> occupied_destination(1);
    {
        GuiStartupOwnershipTransaction<FakeCameraRuntime> transaction(2);
        bind_runtimes(&transaction, &audit);
        transaction.staged_runtimes()[0].AcquirePartialResources();
        require(
            !transaction.InstallInto(&occupied_destination),
            "nonempty destination must reject installation");
        require(
            transaction.owns_staged_runtimes(),
            "rejected installation must retain staging ownership");
    }
    require_equal(
        audit.reset_order,
        {1, 0},
        "rejected handoff must roll staged runtimes back");
    require_equal(
        audit.release_order,
        {0},
        "rejected handoff must release acquired resources");
    require_each_reset_once(audit);
}

}  // namespace

int main()
{
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"partial_stage_failure_rolls_back_once_in_reverse_order",
         test_partial_stage_failure_rolls_back_once_in_reverse_order},
        {"canceled_stage_uses_the_same_cleanup_contract",
         test_canceled_stage_uses_the_same_cleanup_contract},
        {"destructor_is_the_partial_construction_fail_safe",
         test_destructor_is_the_partial_construction_fail_safe},
        {"install_transfers_sole_ownership_across_activation_failure",
         test_install_transfers_sole_ownership_across_activation_failure},
        {"install_rejection_leaves_staging_ownership_intact",
         test_install_rejection_leaves_staging_ownership_intact},
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
