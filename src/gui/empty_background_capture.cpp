// src/gui/empty_background_capture.cpp (see empty_background_capture.h)
#include "gui/empty_background_capture.h"

#include "empty_background_burst_worker.h"
#include "empty_background_camera.h"
#include "gui/per_camera_stream_runtime.h"
#include "scoped_housekeeping_cpu.h"
#include "timing/clock.h"

#include "imgui.h"

#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace orange::gui {
namespace eb = orange::empty_background;
using json = nlohmann::json;

namespace {

int lowest_cpu_of_this_thread()
{
    cpu_set_t set;
    CPU_ZERO(&set);
    if (pthread_getaffinity_np(pthread_self(), sizeof(set), &set) != 0) return -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &set)) return cpu;
    return -1;
}

std::string utc_now() { return orange::timing::utc_iso8601_now(3); }

std::string invoking_user()
{
    if (const char* sudo_user = std::getenv("SUDO_USER"); sudo_user && *sudo_user) return sudo_user;
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_name) return pw->pw_name;
    return {};
}

json gui_panel_requester()
{
    const std::string user = invoking_user();
    return {{"transport", "gui_panel"}, {"user", user.empty() ? json(nullptr) : json(user)}};
}

json identity(const EmptyBackgroundCapture::Request& r)
{
    return {{"request_id", r.request_id.empty() ? json(nullptr) : json(r.request_id)},
            {"operation_id", r.operation_id.empty() ? json(nullptr) : json(r.operation_id)},
            {"requester", r.requester}};
}

}  // namespace

EmptyBackgroundCapture& GuiEmptyBackgroundCapture()
{
    static EmptyBackgroundCapture capture;
    return capture;
}

void EmptyBackgroundCapture::publish(const std::string& state, const std::string& error)
{
    json cameras = json::object();
    for (const auto& c : cameras_) cameras[c.serial] = c.evidence;
    status_ = identity(active_);
    status_["state"] = state;
    status_["schema_version"] = plan_.schema_version;
    status_["capture_id"] = plan_.capture_id.empty() ? json(nullptr) : json(plan_.capture_id);
    status_["started_at_utc"] = started_at_utc_.empty() ? json(nullptr) : json(started_at_utc_);
    status_["cameras"] = cameras;
    status_["descriptor_path"] = nullptr;
    status_["descriptor_sha256"] = nullptr;
    status_["error"] = error.empty() ? json(nullptr) : json(error);
    if (state == "captured" && last_receipt_) {
        status_["descriptor_path"] = last_receipt_->descriptor_path.string();
        status_["descriptor_sha256"] = last_receipt_->descriptor_sha256;
    }
    if (state == "captured" || state == "failed" || state == "refused") status_["finished_at_utc"] = utc_now();
}

void EmptyBackgroundCapture::refuse(const Request& request, const std::string& reason)
{
    if (phase_ == Phase::kIdle) {
        active_ = request;
        plan_ = eb::CapturePlan{};
        cameras_.clear();
        started_at_utc_.clear();
        publish("refused", reason);
    }
    std::cerr << "[GUI][empty_background] capture refused"
              << (request.request_id.empty() ? std::string() : " request_id=" + request.request_id) << ": " << reason
              << std::endl;
}

bool EmptyBackgroundCapture::Start(const Request& request, const Inputs& in, std::string* error)
{
    auto no = [&](const std::string& reason) {
        refuse(request, reason);
        if (error) *error = reason;
        return false;
    };
    if (Busy()) {
        // Never overwrite the running capture's status.
        const std::string reason = "a background capture is already running (" + plan_.capture_id + ")";
        std::cerr << "[GUI][empty_background] capture refused: " << reason << std::endl;
        if (error) *error = reason;
        return false;
    }
    if (in.recording_busy) return no("a recording is armed, running or finalizing");
    if (!in.streaming || !in.cameras || !in.selection || !in.runtimes || in.num_cameras <= 0)
        return no("the cameras are not streaming");
    const bool remote = !request.request_id.empty();
    if (eb::kWriteSchemaVersion < 2 && remote)
        return no("a local-control capture needs capture descriptor version 2 (agent-contracts "
                  "orange-palette/empty_background_v2), which this Orange does not write yet");
    std::string store_error;
    const std::filesystem::path store = eb::DefaultStoreRoot(&store_error);
    if (store.empty()) return no("no background store: " + store_error);

    std::vector<CameraBurstState> cameras;
    for (int i = 0; i < in.num_cameras; ++i) {
        if (!in.selection[i].stream_on) continue;
        if (in.image_levels_busy && in.image_levels_busy(i))
            return no("the image-levels check is measuring Cam" + in.cameras[i].camera_serial + "; retry in a few seconds");
        CameraBurstState c;
        c.camera_index = i;
        c.serial = in.cameras[i].camera_serial;
        c.configuration = eb::CameraConfigurationFromCamera(in.cameras[i]);
        c.burst.camera_serial = c.serial;
        c.burst.configuration = c.configuration;
        if (static_cast<std::size_t>(i) < in.runtimes->size())
            c.worker = (*in.runtimes)[static_cast<std::size_t>(i)].empty_background_burst_worker();
        cameras.push_back(std::move(c));
    }
    if (cameras.empty()) return no("no camera is streaming");

    Request accepted = request;
    if (eb::kWriteSchemaVersion >= 2 && accepted.requester.is_null()) accepted.requester = gui_panel_requester();
    eb::CapturePlan plan;
    plan.schema_version = eb::kWriteSchemaVersion;
    plan.store_root = store;
    plan.capture_id = eb::NewCaptureId(store, &plan.captured_at_utc);
    plan.scene_declaration = accepted.scene_declaration;
    if (plan.schema_version >= 2) {
        plan.requester = accepted.requester;
        plan.declared_by = accepted.declared_by;
        if (plan.declared_by.empty()) return no("declared_by is required (who declares the dishes empty)");
    }

    // Arm every camera; one that cannot burst is written as failed, the others go on.
    int armed = 0;
    for (auto& c : cameras) {
        std::string why;
        std::uint64_t request_id = 0;
        if (!c.worker) {
            why = "no burst worker on this camera";
        } else if (!c.worker->Request(&request_id, &why)) {
            // reason in why
        } else {
            ++armed;
            continue;
        }
        c.done = true;
        c.burst.failure_reason = why;
        c.evidence = {{"capture_status", "failed"}, {"failure_reason", why}};
    }
    if (armed == 0) {
        std::string reasons;
        for (const auto& c : cameras) reasons += " Cam" + c.serial + ": " + c.burst.failure_reason + ";";
        return no("no camera could start a burst:" + reasons);
    }
    active_ = accepted;
    plan_ = std::move(plan);
    cameras_ = std::move(cameras);
    started_ = std::chrono::steady_clock::now();
    started_at_utc_ = utc_now();
    phase_ = Phase::kBursting;
    publish("bursting");
    std::cout << "[GUI][empty_background] capture " << plan_.capture_id << " started on " << armed << " of "
              << cameras_.size() << " streaming cameras (descriptor v" << plan_.schema_version << ")"
              << (active_.request_id.empty() ? std::string() : " request_id=" + active_.request_id) << std::endl;
    return true;
}

void EmptyBackgroundCapture::QueueLocalControl(const orange::control::PendingLocalControlCommand& command)
{
    if (queued_) {
        Request r;
        r.request_id = command.request_id;
        r.operation_id = command.operation_id;
        refuse(r, "another capture_empty_background request is already queued");
        return;
    }
    queued_ = command;
}

void EmptyBackgroundCapture::finish_bursts(const Inputs& in)
{
    std::vector<eb::CameraBurst> bursts;
    for (auto& c : cameras_) bursts.push_back(std::move(c.burst));
    const int cpu = in.housekeeping_cpu >= 0 ? in.housekeeping_cpu : lowest_cpu_of_this_thread();
    eb::CapturePlan plan = plan_;
    // PNG compression and hashing run here, on a housekeeping CPU, never on the
    // GUI, acquisition, YOLO or pose threads.
    writer_ = std::async(std::launch::async, [plan, bursts = std::move(bursts), cpu]() mutable {
        orange::ScopedHousekeepingCpu affinity(cpu);
        return eb::WriteCapture(plan, &bursts);
    });
    phase_ = Phase::kWriting;
    publish("writing");
}

nlohmann::json EmptyBackgroundCapture::StatusJson() const
{
    if (status_.is_null() && staging_.is_null()) return nullptr;
    json out = status_.is_null() ? json{{"state", "idle"}} : status_;
    out["staging"] = staging_;
    return out;
}

void EmptyBackgroundCapture::refresh_newest()
{
    newest_loaded_ = true;
    newest_error_.clear();
    std::string error;
    const std::filesystem::path store = eb::DefaultStoreRoot(&error);
    std::optional<eb::StoredCapture> found;
    if (store.empty()) newest_error_ = "no background store: " + error;
    else found = eb::NewestCapture(store);
    // A different capture than the one shown is never attached on an old choice.
    if (!found || !newest_ || found->descriptor_sha256 != newest_->descriptor_sha256) {
        attach_ = false;
        scene_confirmed_ = false;
    }
    newest_ = std::move(found);
}

std::optional<eb::BindingSelection> EmptyBackgroundCapture::SelectionForRecordStart(bool* requested)
{
    const bool attach = attach_;
    const bool confirmed = scene_confirmed_;
    scene_confirmed_ = false;  // per recording: never carried to the next start
    if (requested) *requested = attach;
    if (!attach) return std::nullopt;
    if (!newest_) throw std::runtime_error("empty background: the chosen capture is no longer listed");
    if (!confirmed)
        throw std::runtime_error("empty background: confirm that the scene is unchanged since capture " +
                                 newest_->descriptor.capture_id + " apart from the fish, or choose No background");
    eb::BindingSelection selection;
    selection.block_version = eb::kWriteSchemaVersion;
    selection.selected = true;
    selection.selection_source = "gui_operator";
    selection.scene_unchanged_confirmed = true;
    selection.descriptor_path = newest_->descriptor_path;
    selection.expected_capture_id = newest_->descriptor.capture_id;
    selection.expected_descriptor_sha256 = newest_->descriptor_sha256;
    if (selection.block_version >= 2) selection.requester = gui_panel_requester();
    return selection;
}

void EmptyBackgroundCapture::RenderPanel(const bool streaming, const bool recording_locked)
{
    if (!ImGui::CollapsingHeader("Empty-dish background")) return;
    if (!declared_by_[0]) {
        const std::string user = invoking_user();
        std::snprintf(declared_by_, sizeof declared_by_, "%s", user.c_str());
    }
    if (!newest_loaded_) refresh_newest();

    ImGui::TextWrapped("Capture with the dishes empty (water, no fish), with the recording's camera settings and "
                       "illumination, before the fish go in. 17 frames per streaming camera; the median is kept.");
    ImGui::BeginDisabled(recording_locked || Busy() || panel_request_.has_value());
    ImGui::InputText("Declared by", declared_by_, sizeof declared_by_);
    ImGui::Checkbox("The dishes are empty (water, no fish)", &dishes_empty_);
    static const char* kStates[] = {"off", "on", "unknown", "not_displayed"};
    ImGui::Combo("Stimulus display", &display_state_, kStates, 4);
    static const char* kKinds[] = {"solid_color", "canvas", "other"};
    if (display_state_ == 1) {
        ImGui::Combo("Shown", &shown_kind_, kKinds, 3);
        if (shown_kind_ == 0) {
            ImGui::InputInt4("RGBA (0-255)", rgba_);
            for (int& c : rgba_) c = c < 0 ? 0 : (c > 255 ? 255 : c);
        } else if (shown_kind_ == 1) {
            ImGui::InputText("Citrus canvas name", canvas_name_, sizeof canvas_name_);
        } else {
            ImGui::InputText("What is shown", shown_note_, sizeof shown_note_);
        }
    }
    ImGui::InputText("Note (optional)", note_, sizeof note_);
    const bool can_capture = streaming && !recording_locked && dishes_empty_;
    ImGui::BeginDisabled(!can_capture);
    if (ImGui::Button("Capture empty background")) {
        // The panel's declaration is checked by the same rules as a request's.
        json params = {{"declared_by", std::string(declared_by_)},
                       {"dishes_empty", true},
                       {"stimulus_display", {{"state", kStates[display_state_]}}}};
        if (note_[0]) params["note"] = std::string(note_);
        if (display_state_ == 1) {
            json shown = {{"kind", kKinds[shown_kind_]}};
            if (shown_kind_ == 0) shown["rgba8"] = {rgba_[0], rgba_[1], rgba_[2], rgba_[3]};
            else if (shown_kind_ == 1) shown["canvas_name"] = std::string(canvas_name_);
            else shown["note"] = std::string(shown_note_);
            params["stimulus_display"]["shown"] = shown;
        }
        try {
            const eb::CaptureDeclaration declaration = eb::CaptureDeclarationFromLocalControl(params);
            Request r;
            r.declared_by = declaration.declared_by;
            r.scene_declaration = declaration.scene_declaration;
            panel_request_ = std::move(r);
            panel_message_ = "Capture queued.";
            dishes_empty_ = false;  // declared again for every capture
        } catch (const std::exception& ex) {
            panel_message_ = std::string("Not captured: ") + ex.what();
        }
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!streaming) ImGui::TextDisabled("Start streaming to capture.");
    if (!panel_message_.empty()) ImGui::TextWrapped("%s", panel_message_.c_str());
    const json status = StatusJson();
    if (status.is_object()) {
        const std::string state = status.value("state", std::string());
        if (state != "idle") {
            ImGui::Text("Last capture: %s %s", status.value("capture_id", json(nullptr)).is_string()
                                                   ? status.at("capture_id").get<std::string>().c_str()
                                                   : "(none)",
                        state.c_str());
            if (status.contains("error") && status.at("error").is_string())
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f), "%s", status.at("error").get<std::string>().c_str());
        }
        if (status.contains("cameras") && status.at("cameras").is_object()) {
            for (auto it = status.at("cameras").begin(); it != status.at("cameras").end(); ++it) {
                const json& c = it.value();
                const std::string reason = c.value("failure_reason", json(nullptr)).is_string()
                                               ? c.at("failure_reason").get<std::string>()
                                               : std::string();
                ImGui::BulletText("Cam%s %s%s%s", it.key().c_str(), c.value("capture_status", std::string("?")).c_str(),
                                  reason.empty() ? "" : ": ", reason.c_str());
            }
        }
        if (status.contains("staging") && status.at("staging").is_object()) {
            for (auto it = status.at("staging").begin(); it != status.at("staging").end(); ++it) {
                if (it.value().value("reserved", false)) continue;
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f), "Cam%s cannot capture: %s", it.key().c_str(),
                                   it.value().value("error", json(nullptr)).is_string()
                                       ? it.value().at("error").get<std::string>().c_str()
                                       : "no staging");
            }
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Next recording");
    ImGui::BeginDisabled(recording_locked);
    if (newest_) {
        const auto& d = newest_->descriptor;
        std::size_t captured = 0;
        for (const auto& c : d.cameras) captured += c.capture_status == "captured" ? 1 : 0;
        ImGui::Text("Newest capture: %s (%s UTC), %zu of %zu cameras captured", d.capture_id.c_str(),
                    d.captured_at_utc.c_str(), captured, d.cameras.size());
        if (ImGui::RadioButton("No background", !attach_)) {
            attach_ = false;
            scene_confirmed_ = false;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Attach this capture", attach_)) attach_ = true;
        if (attach_) {
            ImGui::Checkbox("Scene unchanged since this capture, apart from adding the fish (this recording only)",
                            &scene_confirmed_);
            if (!scene_confirmed_)
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f),
                                   "Not confirmed: a start now is refused (confirm, or choose No background).");
        }
    } else {
        ImGui::TextDisabled("%s", newest_error_.empty() ? "No capture in the store." : newest_error_.c_str());
        attach_ = false;
    }
    if (ImGui::Button("Refresh captures")) refresh_newest();
    ImGui::EndDisabled();
}

void EmptyBackgroundCapture::Poll(const Inputs& in)
{
    // Whether each streaming camera can capture at all, and why not.
    if (!in.streaming || !in.cameras || !in.selection || !in.runtimes || in.num_cameras <= 0) {
        staging_ = nullptr;
    } else {
        json staging = json::object();
        for (int i = 0; i < in.num_cameras && static_cast<std::size_t>(i) < in.runtimes->size(); ++i) {
            if (!in.selection[i].stream_on) continue;
            const EmptyBackgroundBurstWorker* worker =
                (*in.runtimes)[static_cast<std::size_t>(i)].empty_background_burst_worker();
            std::string error;
            if (!worker) error = "no burst worker on this camera";
            else if (!worker->reserved())
                error = worker->reserve_error().empty() ? "no staging reserved at stream start" : worker->reserve_error();
            staging[in.cameras[i].camera_serial] = {
                {"reserved", worker && worker->reserved()},
                {"bytes", worker ? worker->reserved_bytes() : 0},
                {"error", error.empty() ? json(nullptr) : json(error)}};
        }
        staging_ = std::move(staging);
    }
    if (phase_ == Phase::kIdle && panel_request_) {
        const Request r = std::move(*panel_request_);
        panel_request_.reset();
        std::string error;
        panel_message_ = Start(r, in, &error) ? "Capturing..." : "Not captured: " + error;
        return;
    }
    if (phase_ == Phase::kIdle && queued_) {
        const orange::control::PendingLocalControlCommand command = std::move(*queued_);
        queued_.reset();
        Request r;
        r.request_id = command.request_id;
        r.operation_id = command.operation_id;
        r.requester = {{"transport", "local_control_socket"},
                       {"peer", command.peer},
                       {"request_id", command.request_id},
                       {"operation_id", command.operation_id},
                       {"command_source", command.source},
                       {"received_at_utc", command.received_at_utc}};
        try {
            if (!command.peer.is_object()) throw std::runtime_error("the request's socket peer is unknown");
            const eb::CaptureDeclaration declaration = eb::CaptureDeclarationFromLocalControl(command.params);
            r.declared_by = declaration.declared_by;
            r.scene_declaration = declaration.scene_declaration;
        } catch (const std::exception& ex) {
            refuse(r, ex.what());
            return;
        }
        std::string error;
        Start(r, in, &error);
        return;
    }
    if (phase_ == Phase::kBursting) {
        const bool timed_out = std::chrono::steady_clock::now() - started_ > kBurstTimeout;
        bool all_done = true;
        for (auto& c : cameras_) {
            if (c.done) continue;
            EmptyBackgroundBurstResult result;
            if (c.worker->PopResult(&result)) {
                c.done = true;
                c.burst.source_frames = std::move(result.source_frames);
                if (result.ok) c.burst.median = std::move(result.median);
                else c.burst.failure_reason = result.failure_reason;
                c.evidence = {{"capture_status", result.ok ? "captured" : "failed"},
                              {"failure_reason", result.ok ? json(nullptr) : json(result.failure_reason)},
                              {"gpu_id", result.gpu_id},
                              {"min_free_entries", result.min_free_entries},
                              {"min_free_events", result.min_free_events},
                              {"max_queue_depth", result.max_queue_depth},
                              {"burst_ms", result.burst_ms},
                              {"median_ms", result.median_ms}};
                std::cout << "[GUI][empty_background] Cam" << c.serial << " burst "
                          << (result.ok ? "ok" : "FAILED: " + result.failure_reason) << " burst_ms=" << result.burst_ms
                          << " median_ms=" << result.median_ms << " min_free_entries=" << result.min_free_entries
                          << " min_free_events=" << result.min_free_events
                          << " max_queue_depth=" << result.max_queue_depth << std::endl;
                continue;
            }
            all_done = false;
            if (in.recording_busy) c.worker->Abort("a recording started during the burst");
            else if (timed_out) c.worker->Abort("burst did not complete within 5 s");
        }
        if (all_done) finish_bursts(in);
        return;
    }
    if (phase_ == Phase::kWriting &&
        writer_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        phase_ = Phase::kIdle;
        try {
            last_receipt_ = writer_.get();
            for (auto& c : cameras_) {
                for (const auto& camera : last_receipt_->descriptor.cameras) {
                    if (camera.camera_serial != c.serial) continue;
                    c.evidence["capture_status"] = camera.capture_status;
                    if (camera.failure_reason) c.evidence["failure_reason"] = *camera.failure_reason;
                }
            }
            publish("captured");
            newest_loaded_ = false;  // the panel lists the new capture
            if (active_.request_id.empty()) panel_message_ = "Captured " + plan_.capture_id + ".";
            std::cout << "[GUI][empty_background] capture " << plan_.capture_id << " written: "
                      << last_receipt_->descriptor_path.string() << " " << last_receipt_->descriptor_sha256 << std::endl;
        } catch (const std::exception& ex) {
            publish("failed", ex.what());
            if (active_.request_id.empty()) panel_message_ = std::string("Capture failed: ") + ex.what();
            std::cerr << "[GUI][empty_background] capture " << plan_.capture_id << " FAILED: " << ex.what() << std::endl;
        }
    }
}

}  // namespace orange::gui
