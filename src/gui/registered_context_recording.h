#pragma once
#include "gui_recording_evidence.h"
#include "session/recording_start_declaration.h"

#include <string>
#include <vector>

namespace orange::gui {
// GUI-thread only. Shared by Daily Registration, recording panel and local-control
// starts. Persisted options are separate from the per-arm human confirmation.
void LoadRegisteredContextRecordingSettings(const std::string& app_config_path, bool load_media = true);
void SelectDailyContextForGuiRecording(const nlohmann::json& context);
void RenderRegisteredContextRecordingSettings(bool locked);
recording::GuiRecordingEvidenceConfig RegisteredContextRecordingConfigForArm();
void ConsumeRegisteredContextRecordingConfirmation();
// Read-only view of the streaming cameras' recording.media_products.
void RenderRecordingMediaSelection(const std::vector<recording::CameraConfigMediaProducts>& cameras);
// Resolves the streaming cameras' recording.media_products (throws on a
// mismatch, or when the app config still holds recording.media_products) and
// keeps it as this stream's selection for the registered-context controls.
recording::RecordingMediaSelection RecordingMediaSelectionForStream(
    const std::vector<recording::CameraConfigMediaProducts>& cameras);
// Parent recording contexts from app config recording.contexts (throws on a parse error).
recording::RecordingContextsConfig RecordingContextsForStream();
// A start that carries its own declaration (local-control start_recording
// with params.declaration) instead of the Subject panel's Confirm.
// `requester` is who sent it as the transport saw it.
struct RemoteRecordStart {
    session::RecordingStartDeclaration declaration;
    nlohmann::json requester;
};
// The contexts a record start uses: RecordingContextsForStream(), with every
// entry's recording_intent replaced by a remote start's declared intent.
// Throws when a remote start declares an intent but no context is configured.
recording::RecordingContextsConfig RecordingContextsForRecordStart(const RemoteRecordStart* remote);
// Subject references (MetaZebrobot dish per camera) and the lookup endpoint,
// taken at every record start like the contexts.
recording::SubjectReferencesConfig SubjectReferencesForStream();
// Record start: the subject references declared for this recording. Without
// `remote`, the ones the operator confirmed in the Subject panel (throws when
// not confirmed or edited since). With `remote`, its declaration, never the
// panel or the saved table. Fills `confirmation_out`
// (orange.recording_subject_confirmation v2, session::BuildSubjectConfirmation)
// and consumes any panel confirmation either way.
recording::SubjectReferencesConfig SubjectReferencesForRecordStart(nlohmann::json* confirmation_out,
                                                                   const std::vector<std::string>& camera_serials,
                                                                   const RemoteRecordStart* remote = nullptr);
// After pre-arm: Citrus's per-camera dish declarations that differ from the
// confirmed entries (one line each; empty = all matched or none declared).
// Shown in the Subject panel until the next start.
void SetSubjectCrossCheckResult(const std::vector<std::string>& mismatches, bool citrus_declared);
// Orange's local control socket (Citrus's stop and receipts reach Orange
// through it), shown beside the recording intent: up, or down with the reason.
void SetCitrusControlSocketStatus(bool up, const std::string& detail);
recording::ZebrobotLookupConfig ZebrobotLookupForStream();
// Default dish/count plus one editable row per camera serial (overrides).
void RenderSubjectReferenceSelection(bool locked, const std::vector<std::string>& camera_serials);
// "Recording context" panel: the parent recording context (Citrus transfer-v2)
// chosen per session; applies at the next record start, saved on request.
void RenderRecordingContextSelection(bool locked);
} // namespace orange::gui
