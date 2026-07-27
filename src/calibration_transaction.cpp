#include "calibration_transaction.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace orange::calibration {
namespace {

std::vector<std::string> normalized_camera_scope(
    std::vector<std::string> camera_serials)
{
    camera_serials.erase(
        std::remove_if(
            camera_serials.begin(),
            camera_serials.end(),
            [](const std::string& serial) { return serial.empty(); }),
        camera_serials.end());
    std::sort(camera_serials.begin(), camera_serials.end());
    camera_serials.erase(
        std::unique(camera_serials.begin(), camera_serials.end()),
        camera_serials.end());
    return camera_serials;
}

std::string camera_scope_description(const std::vector<std::string>& cameras)
{
    if (cameras.empty()) {
        return "all cameras";
    }
    std::ostringstream output;
    for (std::size_t index = 0; index < cameras.size(); ++index) {
        if (index > 0) {
            output << ',';
        }
        output << cameras[index];
    }
    return output.str();
}

}  // namespace

const char* workflow_kind_name(const WorkflowKind workflow)
{
    switch (workflow) {
    case WorkflowKind::kRecordingStartReservation:
        return "recording_start_reservation";
    case WorkflowKind::kSpatialGroupedCapture:
        return "spatial_grouped_capture";
    case WorkflowKind::kSpatialDirectCapture:
        return "spatial_direct_capture";
    case WorkflowKind::kManualCameraPreflight:
        return "manual_camera_preflight";
    case WorkflowKind::kDailyRegistration:
        return "daily_registration";
    case WorkflowKind::kGuidedCommissioning:
        return "guided_commissioning";
    case WorkflowKind::kArenaCenteringCommissioning:
        return "arena_centering_commissioning";
    case WorkflowKind::kApertureCharacterization:
        return "aperture_characterization";
    case WorkflowKind::kFovAlignment:
        return "fov_alignment";
    case WorkflowKind::kUsafResolution:
        return "usaf_resolution";
    }
    return "unknown";
}

bool mutation_set_contains(
    const MutationSet mutations,
    const Mutation mutation)
{
    return (mutations & mutation_set(mutation)) != 0;
}

const char* mutation_name(const Mutation mutation)
{
    switch (mutation) {
    case Mutation::kNone:
        return "none";
    case Mutation::kCameraParameters:
        return "camera_parameters";
    case Mutation::kCameraStreamLifecycle:
        return "camera_stream_lifecycle";
    case Mutation::kCameraOpenClose:
        return "camera_open_close";
    case Mutation::kCitrusScene:
        return "citrus_scene";
    case Mutation::kRecordingStart:
        return "recording_start";
    }
    return "unknown";
}

TransactionLease::TransactionLease(
    TransactionCoordinator* coordinator,
    const std::uint64_t generation,
    TransactionRequest request)
    : coordinator_(coordinator),
      generation_(generation),
      request_(std::move(request))
{
}

TransactionLease::~TransactionLease()
{
    Release("scope_destroyed", "transaction lease owner left scope");
}

bool TransactionLease::active() const
{
    return !released_ && coordinator_ != nullptr && coordinator_->owns(this);
}

bool TransactionLease::permits(const Mutation mutation) const
{
    return active() &&
        mutation_set_contains(request_.allowed_owner_mutations, mutation);
}

bool TransactionLease::covers_cameras(
    const std::vector<std::string>& camera_serials) const
{
    if (!active()) {
        return false;
    }
    if (request_.camera_serials.empty()) {
        return true;
    }
    const std::vector<std::string> requested =
        normalized_camera_scope(camera_serials);
    return std::all_of(
        requested.begin(),
        requested.end(),
        [&](const std::string& serial) {
            return std::binary_search(
                request_.camera_serials.begin(),
                request_.camera_serials.end(),
                serial);
        });
}

TransactionSnapshot TransactionLease::snapshot() const
{
    if (!coordinator_) {
        return {};
    }
    return coordinator_->snapshot();
}

void TransactionLease::Release(
    const std::string& terminal_status,
    const std::string& terminal_reason)
{
    if (released_) {
        return;
    }
    released_ = true;
    if (coordinator_) {
        coordinator_->Release(
            generation_, terminal_status, terminal_reason);
    }
}

AcquireResult TransactionCoordinator::TryAcquire(TransactionRequest request)
{
    AcquireResult result;
    if (request.owner_id.empty()) {
        result.error = "Calibration transaction owner_id is required.";
        return result;
    }
    if (request.reason.empty()) {
        result.error = "Calibration transaction reason is required.";
        return result;
    }
    request.camera_serials =
        normalized_camera_scope(std::move(request.camera_serials));

    std::lock_guard<std::mutex> lock(mutex_);
    if (active_.active) {
        result.blocker = active_;
        std::ostringstream error;
        error << "Calibration transaction rejected: "
              << workflow_kind_name(active_.workflow)
              << " owner=" << active_.owner_id
              << " already owns "
              << camera_scope_description(active_.camera_serials)
              << ". Reason: " << active_.reason;
        result.error = error.str();
        return result;
    }

    const std::uint64_t generation = next_generation_++;
    active_.active = true;
    active_.generation = generation;
    active_.lease_id = "calibration_lease_" + std::to_string(generation);
    active_.owner_id = request.owner_id;
    active_.workflow = request.workflow;
    active_.camera_serials = request.camera_serials;
    active_.allowed_owner_mutations = request.allowed_owner_mutations;
    active_.reason = request.reason;
    active_.last_terminal_status = last_terminal_status_;
    active_.last_terminal_reason = last_terminal_reason_;
    result.lease = std::unique_ptr<TransactionLease>(
        new TransactionLease(this, generation, std::move(request)));
    return result;
}

TransactionSnapshot TransactionCoordinator::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    TransactionSnapshot result = active_;
    result.last_terminal_status = last_terminal_status_;
    result.last_terminal_reason = last_terminal_reason_;
    return result;
}

bool TransactionCoordinator::active() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return active_.active;
}

bool TransactionCoordinator::owns(const TransactionLease* lease) const
{
    if (!lease) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return active_.active && active_.generation == lease->generation_;
}

std::string TransactionCoordinator::rejection_message(
    const std::string& requested_action) const
{
    const TransactionSnapshot current = snapshot();
    if (!current.active) {
        return {};
    }
    std::ostringstream message;
    message << (requested_action.empty() ? "Operation" : requested_action)
            << " is blocked by calibration transaction "
            << current.lease_id << " ("
            << workflow_kind_name(current.workflow)
            << ", owner=" << current.owner_id << "). Reason: "
            << current.reason;
    return message.str();
}

void TransactionCoordinator::Release(
    const std::uint64_t generation,
    const std::string& terminal_status,
    const std::string& terminal_reason)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_.active || active_.generation != generation) {
        return;
    }
    last_terminal_status_ = terminal_status.empty()
        ? "released"
        : terminal_status;
    last_terminal_reason_ = terminal_reason;
    active_ = {};
    active_.last_terminal_status = last_terminal_status_;
    active_.last_terminal_reason = last_terminal_reason_;
}

TransactionCoordinator& global_transaction_coordinator()
{
    static TransactionCoordinator coordinator;
    return coordinator;
}

}  // namespace orange::calibration
