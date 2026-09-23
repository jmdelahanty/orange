# GUI pose keypoint overlay (unwired module, 2026-09-22)

`src/gui/pose_overlay.{h,cpp}` holds the pure part of an on-screen pose
keypoint overlay: a per-camera latest-pose mailbox, keypoint-to-screen
mapping for the full-frame preview and the crop preview, skeleton
validation, confidence/staleness filtering, and a draw-command builder.
It has no ImGui, CUDA or GL dependency and is covered by
`tools/gui_pose_overlay_tests.cpp` (`ctest -R gui_pose_overlay_tests`).

It is not called by the `orange` target yet. Wiring, planned after the
current GUI soaks are accepted:

1. `PoseWorker` gets `SetOverlayMailbox(PoseOverlayMailbox*)` (null = no
   work) and fills a `PoseOverlaySnapshot` (keypoints converted to source
   pixels, `crop_x + x`) right beside `publish_pose_result_v2`, in both the
   device-stage and the CPU crop paths, regardless of Shaman v2 being on.
   `Publish()` never blocks or allocates.
2. The GUI main thread, once per camera per frame, reads the mailbox and
   calls `build_pose_overlay_commands(...)` for the crop `ImGui::Image`
   rectangle (`GetItemRectMin/Max`) and for the main canvas (ImPlot
   `PlotToPixels`, the pattern of `src/gui/spatial_layout/preview_overlay.cpp`),
   then turns the commands into `AddRect`/`AddLine`/`AddCircleFilled`.
3. Flag `gui.display.pose_overlay` (app config) bridged to
   `ORANGE_GUI_POSE_OVERLAY` (wrapper allowlist + launcher forwarding),
   default off. Counters under `session.gui_display_frame_rate.pose_overlay`
   (`enabled`, `mailbox_publishes`, `frames_drawn`, `stale_frames`,
   `keypoints_drawn`, `max_keypoints_per_frame`, `draw_ms` bucket).
4. Validation: flag-off run with counters at zero and unchanged detect/GUI
   p95s, then a flag-on 10 minute soak gated on `draw_ms` p99 < 0.2 ms, zero
   camera gaps and recorder drops, unchanged owner-push/peer-pull counters.

Why not a CUDA kernel into the preview staging buffer: that is the display
worker path whose full-frame read perturbed card-A owner pushes; the overlay
must not add GPU work, staging-lock time or waits there. Pose for frame N
lands 0.7-1.7 ms after the display worker's `detections_ready`, so the
overlay is one preview frame late by design; at the 10-15 fps preview cap
that is invisible.
