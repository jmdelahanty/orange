#pragma once
#include "gui/spatial_layout/state.h"
#include "spatial_snapshot_worker.h"

namespace orange::gui::spatial_layout {
class DailyNativeContextCapture;
void poll_daily_native_context_capture(SpatialLayoutUiState*, const CameraParams*, int,
                                      SpatialSnapshotWorker* const*, bool recording_locked);
bool consume_daily_native_context_snapshot(SpatialLayoutUiState*, SpatialSnapshotResult*);
void render_daily_native_context_capture(SpatialLayoutUiState*, const CameraParams*, int,
                                        SpatialSnapshotWorker* const*, bool recording_locked);
}
