#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

namespace orange::gui {

enum class GuiStartupOwnershipState {
    kStaging,
    kInstalled,
    kRolledBack,
};

// Owns a fixed set of per-camera runtime slots while stream startup is still
// fallible. Runtime must expose a noexcept Reset() that is safe for empty and
// partially initialized objects. Installation is a vector swap: it performs
// no per-runtime moves and leaves the destination as the sole owner.
template <typename Runtime>
class GuiStartupOwnershipTransaction {
public:
    explicit GuiStartupOwnershipTransaction(const std::size_t runtime_count)
        : runtimes_(runtime_count)
    {
        static_assert(
            std::is_nothrow_destructible_v<Runtime>,
            "startup runtime destruction must not throw");
        static_assert(
            noexcept(std::declval<Runtime&>().Reset()),
            "startup runtime Reset() must be noexcept");
    }

    ~GuiStartupOwnershipTransaction() noexcept
    {
        Rollback();
    }

    GuiStartupOwnershipTransaction(
        const GuiStartupOwnershipTransaction&) = delete;
    GuiStartupOwnershipTransaction& operator=(
        const GuiStartupOwnershipTransaction&) = delete;
    GuiStartupOwnershipTransaction(
        GuiStartupOwnershipTransaction&&) = delete;
    GuiStartupOwnershipTransaction& operator=(
        GuiStartupOwnershipTransaction&&) = delete;

    std::vector<Runtime>& staged_runtimes() noexcept
    {
        return runtimes_;
    }

    const std::vector<Runtime>& staged_runtimes() const noexcept
    {
        return runtimes_;
    }

    GuiStartupOwnershipState state() const noexcept
    {
        return state_;
    }

    bool owns_staged_runtimes() const noexcept
    {
        return state_ == GuiStartupOwnershipState::kStaging;
    }

    // Returns false without changing either vector unless this transaction is
    // still staging and the destination is empty. std::vector::swap preserves
    // pointers/references to the runtime elements, which is required by the
    // compatibility views assembled immediately before installation.
    [[nodiscard]] bool InstallInto(
        std::vector<Runtime>* destination) noexcept
    {
        if (!destination || !destination->empty() ||
            state_ != GuiStartupOwnershipState::kStaging) {
            return false;
        }
        destination->swap(runtimes_);
        state_ = GuiStartupOwnershipState::kInstalled;
        return true;
    }

    // Explicit rollback gives the controller deterministic reverse-camera
    // cleanup at the point a stage fails. The destructor is the fail-safe;
    // repeated calls are intentionally no-ops.
    void Rollback() noexcept
    {
        if (state_ != GuiStartupOwnershipState::kStaging) {
            return;
        }
        for (std::size_t index = runtimes_.size(); index > 0; --index) {
            runtimes_[index - 1].Reset();
        }
        runtimes_.clear();
        state_ = GuiStartupOwnershipState::kRolledBack;
    }

private:
    std::vector<Runtime> runtimes_;
    GuiStartupOwnershipState state_ = GuiStartupOwnershipState::kStaging;
};

}  // namespace orange::gui
