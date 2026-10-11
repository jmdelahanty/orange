#pragma once
#include "gui_recording_evidence.h"

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
void RenderRecordingMediaSelection(bool stream_locked);
recording::RecordingMediaSelection RecordingMediaSelectionForStream();
// Parent recording contexts from app config recording.contexts (throws on a parse error).
recording::RecordingContextsConfig RecordingContextsForStream();
// Subject references (MetaZebrobot dish per camera) and the lookup endpoint,
// taken at every record start like the contexts.
recording::SubjectReferencesConfig SubjectReferencesForStream();
// Record start (GUI button or local-control start): the subject references the
// operator confirmed for this recording; throws when not confirmed or edited
// since. Fills `confirmation_out` (orange.recording_subject_confirmation v1) and
// consumes the confirmation.
recording::SubjectReferencesConfig SubjectReferencesForRecordStart(nlohmann::json* confirmation_out);
// After pre-arm: Citrus's per-camera dish declarations that differ from the
// confirmed entries (one line each; empty = all matched or none declared).
// Shown in the Subject panel until the next start.
void SetSubjectCrossCheckResult(const std::vector<std::string>& mismatches, bool citrus_declared);
recording::ZebrobotLookupConfig ZebrobotLookupForStream();
// Default dish/count plus one editable row per camera serial (overrides).
void RenderSubjectReferenceSelection(bool locked, const std::vector<std::string>& camera_serials);
// "Recording context" panel: the parent recording context (Citrus transfer-v2)
// chosen per session; applies at the next record start, saved on request.
void RenderRecordingContextSelection(bool locked);
} // namespace orange::gui
