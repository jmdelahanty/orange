#include "spatial_roi_recorder_frame_journal_bridge.h"

#include <exception>
#include <string>
#include <utility>

namespace orange::spatial_roi::recording {
namespace {

void set_error_noexcept(std::string* error_out, const char* value) noexcept
{
    if (!error_out) {
        return;
    }
    try {
        error_out->assign(value ? value : "");
    } catch (...) {
        // The bridge is noexcept and diagnostics are advisory.
    }
}

void clear_error_noexcept(std::string* error_out) noexcept
{
    if (!error_out) {
        return;
    }
    try {
        error_out->clear();
    } catch (...) {
        // The bridge is noexcept and diagnostics are advisory.
    }
}

}  // namespace

bool make_spatial_roi_recorder_frame_transport_outcome(
    const ipc::SpatialRoiRecorderIpcFrameOutcome& session_outcome,
    SpatialRoiRecorderFrameTransportOutcome* outcome_out,
    std::string* error_out) noexcept
{
    try {
        if (!outcome_out) {
            set_error_noexcept(error_out, "journal bridge output is null");
            return false;
        }
        SpatialRoiRecorderFrameTransportOutcome converted;
        converted.descriptor = session_outcome.descriptor;
        converted.detach_status = session_outcome.dispatch.detach_status;
        converted.source_release_safe =
            session_outcome.dispatch.source_release_safe;
        converted.dispatch_admitted = session_outcome.dispatch.accepted();
        converted.dispatch_reason = session_outcome.dispatch.reason;
        converted.ack_attempted = session_outcome.ack_attempted;
        converted.ack_sent = session_outcome.ack_sent;
        converted.ack_accepted = session_outcome.ack_accepted;
        converted.ack_reason = session_outcome.ack_reason;
        converted.ack_error = session_outcome.ack_error;
        converted.release_attempted = session_outcome.release_attempted;
        converted.release_sent = session_outcome.release_sent;
        converted.release_reason = session_outcome.release_reason;
        converted.release_error = session_outcome.release_error;
        *outcome_out = std::move(converted);
        clear_error_noexcept(error_out);
        return true;
    } catch (const std::exception& exception) {
        set_error_noexcept(error_out, exception.what());
        return false;
    } catch (...) {
        set_error_noexcept(error_out, "journal bridge conversion failed");
        return false;
    }
}

}  // namespace orange::spatial_roi::recording
