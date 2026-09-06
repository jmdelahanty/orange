#include "gui/spatial_layout/daily_native_context_capture.h"
#include "daily_registered_context.h"
#include "citrus_recording_geometry.h"
#include "gui/spatial_layout/calibration_transaction_bridge.h"
#include "gui/spatial_layout/projection_snapshot_client.h"
#include "project.h"
#include "scoped_housekeeping_cpu.h"
#include "imgui.h"
#include <chrono>
#include <future>
#include <map>
#include <stdexcept>
#include <unistd.h>

namespace orange::gui::spatial_layout {
using json = nlohmann::json;
namespace {
constexpr const char* kOwner = "daily_native_context";
double now_seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int camera_index(const CameraParams* cameras, int count, const std::string& serial) {
    for (int i = 0; cameras && i < count; ++i) if (cameras[i].camera_serial == serial) return i;
    return -1;
}
json camera_settings(const CameraParams& camera) {
    return {{"camera_id", camera.camera_id}, {"width", camera.width}, {"height", camera.height},
        {"pixel_format", camera.pixel_format}, {"frame_rate_hz", camera.frame_rate}, {"exposure_us", camera.exposure},
        {"gain", camera.gain}, {"focus", camera.focus}, {"iris", camera.iris}};
}
void require(bool value, const std::string& error) { if (!value) throw std::runtime_error(error); }
}
class DailyNativeContextCapture {
public:
    int cpu = -1, subject_presence = 2;
    bool dish_setup = false, nir_fixed = false, camera_fixed = false, rig_fixed = false;
    bool active = false, saving = false;
    std::string status = "No native daily context captured.", error, saved_path, canvas_path;
    double deadline = 0;
    calibration::DailyContextPlan plan;
    std::map<std::string, uint64_t> pending;
    std::vector<calibration::DailyContextFrame> frames;
    std::future<json> write;
};

void poll_daily_native_context_capture(SpatialLayoutUiState* ui, const CameraParams* cameras, int count,
                                      SpatialSnapshotWorker* const* workers, bool recording_locked) {
    if (!ui || !ui->daily_native_context_capture || !ui->daily_native_context_capture->active) return;
    auto& capture = *ui->daily_native_context_capture;
    const auto finish = [&](const std::string& failure) {
        capture.active = false;
        capture.saving = false;
        capture.error = failure;
        capture.frames.clear();
        capture.status = failure.empty() ? "Native daily context saved; no recording started." : "Native daily context failed; registration was not changed.";
        if (spatial_calibration_transaction_owned_by(*ui, kOwner))
            release_spatial_calibration_transaction(ui, failure.empty() ? "complete" : "failed", capture.status);
    };
    if (capture.saving) {
        if (capture.write.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        try {
            const auto receipt = capture.write.get();
            capture.saved_path = (capture.plan.output_root / receipt.at("relative_path").get<std::string>()).string();
            finish("");
        } catch (const std::exception& ex) { finish(ex.what()); }
        return;
    }
    // Consume only this operation's result, even when the registration panel
    // is collapsed/closed. Do not steal another calibration tool's snapshot.
    for (auto it = capture.pending.begin(); it != capture.pending.end();) {
        const auto current = it++;
        const int index = camera_index(cameras, count, current->first);
        auto* worker = index >= 0 && workers ? workers[index] : nullptr;
        SpatialSnapshotResult result;
        if (worker && worker->PopCompletedSnapshotForRequest(current->second, capture.plan.capture_id, &result))
            consume_daily_native_context_snapshot(ui, &result);
    }
    if (recording_locked) capture.error = "Recording began while a pre-recording context request was pending.";
    if (now_seconds() > capture.deadline && capture.error.empty()) capture.error = "Native context capture timed out.";
    for (auto it = capture.pending.begin(); it != capture.pending.end();) {
        const int index = camera_index(cameras, count, it->first);
        auto* worker = index >= 0 && workers ? workers[index] : nullptr;
        if (!worker || worker->HasFatalError()) {
            capture.error = "Snapshot worker disappeared or failed for Cam" + it->first;
            it = capture.pending.erase(it);
        } else if (!capture.error.empty() && worker->CancelUnclaimedRequest(it->second)) it = capture.pending.erase(it);
        else ++it;
    }
    if (!capture.pending.empty()) {
        if (!capture.error.empty()) capture.status = "Capture failed; waiting for claimed source copies to release safely.";
        return;
    }
    if (!capture.error.empty()) { finish(capture.error); return; }
    try {
        require(spatial_calibration_transaction_owned_by(*ui, kOwner), "Native context calibration lease was lost.");
        for (const auto& item : capture.plan.cameras.items()) {
            const int index = camera_index(cameras, count, item.key());
            require(index >= 0 && camera_settings(cameras[index]) == item.value(), "Camera configuration changed during native capture.");
        }
        const auto after = query_citrus_daily_registration_status("native_context_post_capture");
        require(after.ok, "Cannot verify selected registration after native context capture: " + after.reason);
        calibration::ValidateDailyContextSelection(after.daily_registration, capture.plan.registration_path,
            capture.plan.registration_sha256, capture.plan.cameras);
        capture.plan.runtime_after = after.daily_registration;
        auto plan = capture.plan;
        const auto canvas = capture.canvas_path;
        capture.write = std::async(std::launch::async, [plan = std::move(plan), canvas, frames = std::move(capture.frames)]() mutable {
            orange::ScopedHousekeepingCpu affinity(plan.housekeeping_cpu);
            recording_geometry::CitrusGeometryResolveRequest request;
            request.selected_canvas_config_path = canvas; request.selection_source = "daily_native_context";
            request.captured_at_utc = plan.requested_at_utc;
            for (const auto& item : plan.cameras.items()) request.camera_serials.push_back(item.key());
            plan.geometry = recording_geometry::resolve_citrus_recording_geometry(request).contract;
            auto receipt = calibration::WriteDailyRegisteredContext(plan, frames);
            affinity.Restore();
            return receipt;
        });
        capture.saving = true;
        capture.status = "Saving native pixels and verifying registration/geometry evidence on a housekeeping worker.";
    } catch (const std::exception& ex) { finish(ex.what()); }
}

bool consume_daily_native_context_snapshot(SpatialLayoutUiState* ui, SpatialSnapshotResult* result) {
    if (!ui || !result || !ui->daily_native_context_capture) return false;
    auto& capture = *ui->daily_native_context_capture;
    const auto it = capture.pending.find(result->camera_serial);
    if (!capture.active || it == capture.pending.end() || it->second != result->request_id || result->operation_id != capture.plan.capture_id)
        return false;
    capture.pending.erase(it);
    if (!result->ok || result->capture_representation != "native_bytes" || result->pixel_format != GVSP_PIX_MONO8 ||
        result->requested_frame_count != 1 || result->completed_frame_count != 1 || result->recording_frame_id != 0) {
        capture.error = "Cam" + result->camera_serial + " native snapshot rejected: " + result->error;
        return true;
    }
    calibration::DailyContextFrame frame;
    frame.source.camera_serial = result->camera_serial; frame.source.width = result->width; frame.source.height = result->height;
    frame.source.local_frame_id = result->local_frame_id; frame.source.camera_frame_id = result->camera_frame_id;
    frame.source.recording_frame_id = result->recording_frame_id;
    frame.source.camera_timestamp_ns = result->camera_timestamp_ns; frame.source.timestamp_sys_ns = result->timestamp_sys_ns;
    frame.source.mono8 = std::move(result->native_bytes); frame.source_storage = result->native_source_storage;
    capture.frames.push_back(std::move(frame));
    return true;
}

void render_daily_native_context_capture(SpatialLayoutUiState* ui, const CameraParams* cameras, int count,
                                        SpatialSnapshotWorker* const* workers, bool recording_locked) {
    if (!ui) return;
    const auto& workflow = ui->daily_registration_workflow;
    if (workflow.stage != "complete" && !ui->daily_native_context_capture) return;
    if (!ui->daily_native_context_capture) ui->daily_native_context_capture = std::make_shared<DailyNativeContextCapture>();
    auto& capture = *ui->daily_native_context_capture;
    ImGui::SeparatorText("Native Experiment Context (Optional)");
    ImGui::TextWrapped("After accepting daily registration, capture one unaltered full-camera Mono8 image per registered camera. "
        "Streaming must be on; detection may be off. This does not start recording or replace calibration images. Fish may be present.");
    ImGui::BeginDisabled(capture.active);
    ImGui::InputInt("Context housekeeping CPU", &capture.cpu);
    ImGui::TextDisabled("Choose a housekeeping CPU from the rig isolation plan; never an acquisition/render CPU.");
    const char* presence[] = {"Absent", "Present", "Unknown"};
    ImGui::Combo("Subjects in context", &capture.subject_presence, presence, 3);
    ImGui::Checkbox("Dish setup complete for this context", &capture.dish_setup);
    ImGui::Checkbox("Experiment NIR illumination is fixed", &capture.nir_fixed);
    ImGui::Checkbox("Runtime optical path restored and camera settings fixed", &capture.camera_fixed);
    ImGui::Checkbox("Rig and dishes will remain fixed", &capture.rig_fixed);
    ImGui::BeginDisabled(workflow.stage != "complete" || recording_locked || capture.cpu < 0 || capture.cpu >= CPU_SETSIZE ||
        !capture.dish_setup || !capture.nir_fixed || !capture.camera_fixed || !capture.rig_fixed ||
        (ui->calibration_transaction_lease && ui->calibration_transaction_lease->active()));
    if (ImGui::Button("Capture Native Context (All Registered Cameras)")) {
        try {
            capture.error.clear(); capture.saved_path.clear(); capture.pending.clear(); capture.frames.clear();
            calibration::DailyContextPlan plan;
            const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
            plan.capture_id = "native_context_" + std::to_string(getpid()) + "_" + std::to_string(ticks);
            plan.output_root = std::filesystem::path(workflow.transaction_dir) / plan.capture_id;
            plan.registration_path = workflow.accepted_registration_path;
            plan.registration_sha256 = workflow.accepted_registration_sha256;
            plan.requested_at_utc = get_current_utc_timestamp(); plan.housekeeping_cpu = capture.cpu;
            const char* presence_values[] = {"absent", "present", "unknown"};
            plan.declaration = {{"schema_id", "orange.recording.registered_scene_context.capture_declaration"}, {"schema_version", 1},
                {"registration_authority_status", "accepted_for_experiment"}, {"subject_presence", presence_values[capture.subject_presence]},
                {"dish_setup_complete", true}, {"nir_illumination_fixed", true}, {"camera_configuration_fixed", true}, {"rig_fixed", true}};
            std::vector<std::string> serials;
            uint64_t total = 0;
            for (const auto& target : workflow.targets) {
                const int index = camera_index(cameras, count, target.camera_serial);
                require(index >= 0 && workers && workers[index] && !workers[index]->HasFatalError(), "Start streaming every registered camera first.");
                const auto& camera = cameras[index];
                const uint64_t bytes = static_cast<uint64_t>(camera.width) * camera.height;
                require(camera.pixel_format == "Mono8" && bytes > 0 && bytes <= 64 * 1024 * 1024 &&
                    (total += bytes) <= 256 * 1024 * 1024 && !plan.cameras.contains(target.camera_serial), "Unsupported native raster/camera set or capture budget.");
                plan.cameras[target.camera_serial] = camera_settings(camera); serials.push_back(target.camera_serial);
            }
            require(!serials.empty() && serials.size() <= 64, "No unambiguous registered cameras selected.");
            const auto before = query_citrus_daily_registration_status("native_context_pre_capture");
            require(before.ok, "Cannot verify selected daily registration: " + before.reason);
            calibration::ValidateDailyContextSelection(before.daily_registration, plan.registration_path, plan.registration_sha256, plan.cameras);
            plan.runtime_before = before.daily_registration;
            std::string error;
            require(acquire_spatial_calibration_transaction(ui, kOwner, plan.capture_id, calibration::WorkflowKind::kDailyRegistration,
                serials, calibration::mutation_set(calibration::Mutation::kNone), "Capture native daily context without changing camera/recording state.", &error), error);
            capture.plan = std::move(plan); capture.canvas_path = ui->citrus_canvas_config_path;
            capture.active = true; capture.saving = false; capture.deadline = now_seconds() + 10.0;
            for (const auto& serial : serials) {
                auto slot = capture.pending.emplace(serial, 0).first;
                if (!workers[camera_index(cameras, count, serial)]->RequestNativeSnapshot(capture.plan.capture_id, &slot->second, &error,
                    NativeSnapshotOptions{capture.cpu, true})) {
                    capture.pending.erase(slot);
                    throw std::runtime_error(error);
                }
            }
            capture.status = "Waiting for native source frames; recording and calibration mutations are locked.";
        } catch (const std::exception& ex) {
            capture.error = ex.what();
            // Poll cancels unclaimed requests and drains claimed source copies
            // before releasing the transaction; never abandon a camera lease.
            if (!capture.active && spatial_calibration_transaction_owned_by(*ui, kOwner))
                release_spatial_calibration_transaction(ui, "failed", capture.error);
        }
    }
    ImGui::EndDisabled(); ImGui::EndDisabled();
    ImGui::TextWrapped("%s", capture.status.c_str());
    if (!capture.error.empty()) ImGui::TextWrapped("Error: %s", capture.error.c_str());
    if (!capture.saved_path.empty()) {
        ImGui::TextWrapped("Context descriptor: %s", capture.saved_path.c_str());
        ImGui::TextWrapped("This daily asset is not yet selected for any recording. The timed headless context option still captures a fresh image.");
    }
}
}
