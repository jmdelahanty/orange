# GUI registration and NVENC journal integration handoff

Date: 2026-09-21

The pending GUI edits now form two focused commits: explicit arena-centering
error conventions, followed by daily-registration adjustment direction guidance.
The NVENC scheduling explanation is a separate documentation-only commit in its
own worktree. This handoff identifies the exact changes to integrate and the
checks completed before committing them.

The ten GUI files were already modified when this session inspected the worktree.
This commit pass reviewed and validated those edits, then committed their existing
contents without rewriting them. The journal explanation and scheduling diagram
were written in this session at the user's request.

## Commits to integrate

### 1. Arena-centering conventions and regression coverage

- Commit: `c24332f2d7b7eda4b3eb309ce744b348a137ecf9`
- Subject: `Document arena-centering residual conventions and reflected-axis behavior`
- Worktree: `/home/jeremy/orange-gop-split-a16`
- Branch: `agent/acquisition/shaman-v2-authoritative-20260824`

Fiducial detection reports detected center minus expected center, whereas the
centering solver and verification report target minus detected center. The commit
adds explicit definition strings beside those JSON fields, documents the
conventions, and tests reflected and rotated Jacobians, correction direction,
integer quantization, and refinement. Numeric residuals and solver behavior retain
their existing definitions.

Files: `src/gui/arena_centering_analysis.{cpp,h}`,
`src/gui/arena_centering_autorun.cpp`,
`tools/arena_centering_analysis_tests.cpp`, and
`docs/orange_citrus_arena_centering_commissioning_contract.md`.

### 2. Daily-registration direction guidance

- Commit: `4bc80398a348e266b576a0a7366dd314e7226f7d`
- Subject: `Explain daily-registration adjustments in canvas and camera coordinates`
- Same worktree and branch as commit 1; apply after commit 1.

The manual X/Y controls now explicitly describe logical canvas pixels. A geometry
helper projects a one-pixel positive step through the accepted canvas-to-camera
homography at the relevant center. The GUI shows the resulting camera-image
direction and a numerical tooltip, or an unavailable hint for missing/invalid
geometry. These hints describe camera raster directions rather than physical room
directions. The commit also labels geometry-review and preview residual
definitions, with tests for reflection, rotation/perspective, and invalid or
degenerate transforms. It removes the UI's hard-coded approximate millimeters per
canvas pixel.

Files: `src/gui/daily_registration_geometry.{cpp,h}`,
`src/gui/spatial_layout/daily_registration_workflow.cpp`,
`tools/daily_registration_geometry_tests.cpp`, and
`docs/orange_citrus_guided_daily_registration_contract.md`.

### 3. NVENC journal explanation

- Commit: `8853bd111cb4f58ba6f0e64ed0e4b4f0d3f32f27`
- Subject: `docs: explain NVENC peer-copy ownership and scheduling`
- Worktree: `/home/jeremy/orange-nvenc-native-integration-20260919`
- Branch: `agent/encoder/nvenc-native-integration-20260919`
- File: `docs/nvenc_input_layout_journal.md`

Adds the dated push/pull comparison, CUDA IPC import explanation, source and
destination lifetime rules, and Mermaid/plain-text scheduling diagrams. The entry
records the proposal and evidence available on September 19. Its earlier open
benchmark questions are historical: subsequent work in the device-ROI branch
tested them and implemented owner push with per-card serialization. Preserve
later journal entries and their qualification status when integrating this entry.
This commit carries no runtime implementation or default change.

## Integration procedure

The GUI commits were prepared on base
`ca817a052f48be0f651363506848cc91bb904787` (`Harden guided daily registration workflow`).
The actual branch is the Shaman acquisition branch named above, despite the older
`exp/gop-split-a16` label in `AGENTS.md`. There were already seven commits ahead
of its remote tracking branch before this commit pass.

Integrate the two GUI commits onto the intended destination branch, preserving its
existing guided-registration work. Review conflicts against both sides, especially
`daily_registration_workflow.cpp`; do not replace that file wholesale. If the
destination lacks the base workflow or test targets, resolve those prerequisites
explicitly instead of importing the entire source branch as an incidental merge.
If equivalent changes already exist, reconcile or skip them rather than duplicate
the UI or JSON fields.

From the destination worktree, when the changes are not already present:

```bash
git cherry-pick c24332f2d7b7eda4b3eb309ce744b348a137ecf9
git cherry-pick 4bc80398a348e266b576a0a7366dd314e7226f7d
```

Integrate `8853bd111cb4f58ba6f0e64ed0e4b4f0d3f32f27` separately if the destination
maintains this journal. If its journal has advanced, merge the dated entry and
navigation link while keeping its newer content. The GUI commits do not depend
on this documentation commit or on the NVENC integration branch's code history.

This handoff is documentation for the integration agent. No integration target
was selected or merged, and no commits were pushed during this commit pass.
The prior native-NVENC implementation has its own handoff at
`/home/jeremy/orange-nvenc-native-integration-20260919/docs/nvenc_native_handoff_20260919.md`;
it is outside this pending-GUI/journal batch, as are the later owner-push and
crop-only investigations.

## Validation completed

- Fresh CMake Release configuration using GNU C++ 11.4.0, CUDA 12.2.140, and
  OpenCV 4.10.0 in an isolated `/tmp` build directory.
- Built and passed `arena_centering_analysis_tests` and
  `daily_registration_geometry_tests` through CTest: 2/2 passed.
- Passed `python3 tools/validate_gui_arena_centering_result_tests.py`.
- Passed `-fsyntax-only` checks for `arena_centering_autorun.cpp` and
  `daily_registration_workflow.cpp`, using their GUI target commands from the
  generated `compile_commands.json` with compile/output options replaced by
  syntax checking.
- Passed `git diff --check` for both worktrees before committing.

The local check directory is
`/tmp/orange-gui-thematic-20260921-kecisp7n`. It contains the initial file hashes
and patches, commit receipts, generated build files, CTest log, and exact GUI
syntax-check commands/logs. It is temporary diagnostic evidence; the source,
tests, and this validation record are retained in Git.

To reproduce the focused tests from the integrated worktree:

```bash
ORANGE_GUI_CHECK_BUILD=$(mktemp -d /tmp/orange-gui-integration-check.XXXXXX)
cmake -S . -B "$ORANGE_GUI_CHECK_BUILD" \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DBUILD_TESTING=ON
cmake --build "$ORANGE_GUI_CHECK_BUILD" \
  --target arena_centering_analysis_tests daily_registration_geometry_tests \
  --parallel 4
ctest --test-dir "$ORANGE_GUI_CHECK_BUILD" --output-on-failure \
  -R '^(arena_centering_analysis_tests|daily_registration_geometry_tests)$'
python3 tools/validate_gui_arena_centering_result_tests.py
```

This pass did not link or launch the full GUI, run cameras/projectors, or alter
calibration/runtime settings. Integration should include a full GUI build and an
operator check that the X/Y labels, per-camera hints, unavailable state, and
numeric tooltips remain readable and match the selected accepted homography.
These checks establish the geometry and compile-time behavior, not a fresh
physical registration acceptance or a recording-performance result.

`build-gop-split/` was not touched or included in any commit.
