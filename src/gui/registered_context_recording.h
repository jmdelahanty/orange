#pragma once
#include "gui_recording_evidence.h"

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
// "Recording context" panel: the parent recording context (Citrus transfer-v2)
// chosen per session; applies at the next record start, saved on request.
void RenderRecordingContextSelection(bool locked);
} // namespace orange::gui
