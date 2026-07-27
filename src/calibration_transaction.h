#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace orange::calibration {

enum class WorkflowKind {
    kRecordingStartReservation,
    kSpatialGroupedCapture,
    kSpatialDirectCapture,
    kManualCameraPreflight,
    kDailyRegistration,
    kGuidedCommissioning,
    kArenaCenteringCommissioning,
    kApertureCharacterization,
    kFovAlignment,
    kUsafResolution,
};

const char* workflow_kind_name(WorkflowKind workflow);

enum class Mutation : std::uint32_t {
    kNone = 0,
    kCameraParameters = 1u << 0u,
    kCameraStreamLifecycle = 1u << 1u,
    kCameraOpenClose = 1u << 2u,
    kCitrusScene = 1u << 3u,
    kRecordingStart = 1u << 4u,
};

using MutationSet = std::uint32_t;

constexpr MutationSet mutation_set(const Mutation mutation)
{
    return static_cast<MutationSet>(mutation);
}

constexpr MutationSet operator|(const Mutation left, const Mutation right)
{
    return mutation_set(left) | mutation_set(right);
}

constexpr MutationSet operator|(const MutationSet left, const Mutation right)
{
    return left | mutation_set(right);
}

bool mutation_set_contains(MutationSet mutations, Mutation mutation);
const char* mutation_name(Mutation mutation);

struct TransactionRequest {
    std::string owner_id;
    WorkflowKind workflow = WorkflowKind::kSpatialGroupedCapture;
    std::vector<std::string> camera_serials;
    MutationSet allowed_owner_mutations = mutation_set(Mutation::kNone);
    std::string reason;
};

struct TransactionSnapshot {
    bool active = false;
    std::uint64_t generation = 0;
    std::string lease_id;
    std::string owner_id;
    WorkflowKind workflow = WorkflowKind::kSpatialGroupedCapture;
    std::vector<std::string> camera_serials;
    MutationSet allowed_owner_mutations = mutation_set(Mutation::kNone);
    std::string reason;
    std::string last_terminal_status;
    std::string last_terminal_reason;
};

class TransactionCoordinator;

class TransactionLease final {
public:
    ~TransactionLease();

    TransactionLease(const TransactionLease&) = delete;
    TransactionLease& operator=(const TransactionLease&) = delete;
    TransactionLease(TransactionLease&&) = delete;
    TransactionLease& operator=(TransactionLease&&) = delete;

    bool active() const;
    bool permits(Mutation mutation) const;
    bool covers_cameras(const std::vector<std::string>& camera_serials) const;
    TransactionSnapshot snapshot() const;

    void Release(
        const std::string& terminal_status = "released",
        const std::string& terminal_reason = std::string());

private:
    friend class TransactionCoordinator;

    TransactionLease(
        TransactionCoordinator* coordinator,
        std::uint64_t generation,
        TransactionRequest request);

    TransactionCoordinator* coordinator_ = nullptr;
    std::uint64_t generation_ = 0;
    TransactionRequest request_;
    bool released_ = false;
};

struct AcquireResult {
    std::unique_ptr<TransactionLease> lease;
    TransactionSnapshot blocker;
    std::string error;

    bool ok() const { return lease != nullptr; }
};

class TransactionCoordinator final {
public:
    AcquireResult TryAcquire(TransactionRequest request);

    TransactionSnapshot snapshot() const;
    bool active() const;
    bool owns(const TransactionLease* lease) const;

    std::string rejection_message(const std::string& requested_action) const;

private:
    friend class TransactionLease;

    void Release(
        std::uint64_t generation,
        const std::string& terminal_status,
        const std::string& terminal_reason);

    mutable std::mutex mutex_;
    std::uint64_t next_generation_ = 1;
    TransactionSnapshot active_;
    std::string last_terminal_status_;
    std::string last_terminal_reason_;
};

TransactionCoordinator& global_transaction_coordinator();

}  // namespace orange::calibration
