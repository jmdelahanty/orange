# Daily registration autorun: plan and findings (2026-11-15)

Jeremy, 2026-11-15: "build an automatic mode first, like the other scripted
captures ... I want things to be as automated as possible with these runners."

The goal is that each arena's experimental-area centre follows today's dish, so
the dish never has to be nudged. Citrus calls this daily projection registration:
a small canvas translation per arena, measured from Orange's rim fit. Today's
recordings ran on `commissioned_base_geometry`, with no translation. The
registration accuracy check (`scripts/registration_accuracy_check.py`, v3)
measured each dish centre against the projected crosshair at about
0.7 / 2.3-2.9 / 1.9-2.4 / 1.5 mm (2010093..96). That is the error this
removes.

## 1. Today's manual flow (Orange GUI + Citrus)

Launch: `scripts/run_daily_registration_citrus_gui.sh`, then
`scripts/run_daily_registration_orange_gui.sh`. The Orange script names the
Citrus one, but `run_daily_registration_citrus_gui.sh` does not exist on this
branch: the manual flow has no checked-in Citrus launcher. The autorun
launcher starts Citrus itself with `CITRUS_GUI_AUTORUN_RIG=omnifin0`, which a
coming Citrus change (one rig per process) needs, or Citrus waits for an
operator to pick the rig. The second streams all four
cameras with recording, YOLO and crops off, and needs the Citrus socket. The
Orange panel is `src/gui/spatial_layout/daily_registration_workflow.cpp`.
Citrus methods (`docs/orange_citrus_guided_daily_registration_contract.md`):
`begin_daily_registration`, `create_daily_registration_candidate`,
`preview_daily_registration_candidate`, `restore_daily_registration_preview`,
`accept_daily_registration`, `reject_daily_registration`,
`abort_daily_registration`, `select_daily_registration_runtime_mode`,
`daily_registration_status` and `active_projection_snapshot`.

| Stage | Who | What happens | Persisted |
|---|---|---|---|
| start | operator ticks "dishes installed, water settled, IR filters in place" and presses Start | Orange takes the calibration transaction lease and selects `base_only`. Citrus `begin_daily_registration` checks the commissioning release, then black scene | transaction dir under the calibration session, checkpoint JSON |
| capture rims | button | Citrus shows black; Orange takes a PTP-grouped temporal mean per camera on the normal IR path; Hough proposal (`DetectDailyRim`) | capture frames |
| review rims | **human: per-camera "rim follows the water-side inner edge"**, then "Accept Rims And Compute Translation" | Orange writes one `orange.calibration.dish_top_rim_observation` v3 per camera (operator-confirmed). Citrus `create_daily_registration_candidate` maps each rim centre through the commissioned homography and rounds it to an integer canvas translation | rim artifacts, candidate (Citrus) |
| geometry review | automatic QC; the operator presses "Project Automatic Alignment For Manual Review" (or Reject) | Orange inverse-projects the translated experimental area. `GeometryReviewPasses` checks the centre residual against the integer-quantisation bound | review JSON and overlays |
| manual alignment | **human: looks at the projected outline with the eye, nudges ±1/±5 canvas px, confirms each arena, freezes** | Citrus writes a derived candidate (`operator_visual_alignment`) | derived candidate |
| accept | **human: confirms the outline, arms accept and select** | Citrus `accept_daily_registration` (armed, checksum, future `valid_until_utc`, `verification.result: passed`) writes an immutable accepted artifact. Orange then `select_daily_registration_runtime_mode selected_daily_registration` | accepted artifact, runtime-selection pointer |
| START | Citrus | `Arena::PrepareStart` reads `calibration_artifacts/daily_registration_runtime_selection.json` and applies the translation. The H5 records the mode and status | |

### What is a human judgement, and what is mechanical

- **Citrus does not require a human.** `accept_daily_registration` needs
  `accept_registration_armed: true`, the candidate checksum, a future
  `valid_until_utc` and a `verification` object whose `result` is `passed`. The
  select step needs `select_runtime_mode_armed: true` and the artifact path and
  sha. Both flags are request fields: Citrus cannot tell, and does not ask,
  whether a person set them. `operator_visual_alignment_not_confirmed` applies
  only to the optional manual-freeze path. (Citrus main b01eddb, which matches
  rig 240c6cc; read only. No Citrus code or config change is needed.)
- **`verification` is built by Orange**, in `AcceptAndSelect`, from its own
  automatic QC and operator flags. Without the manual freeze, its policy is
  `orange_guided_daily_registration_rim_only_v2`.
- **The published rim observation v3 is a human act.** It records
  `accepted_by_operator: true` and the interpretation
  `operator_confirmed_dish_inner_rim_...`. Geometry contract v2, which every
  recording carries, pins v3. The quality-gate route (rim v4 with
  `acceptance.basis: quality_gate`, contract v3; agent-contracts #133/#134) is
  merged, but its writes are OFF until Palette's gate-recompute PR is deployed,
  Citrus admits contract v3, and Jeremy says so.
- **Manual visual alignment needs the eye on the projected outline.** With the
  NIR filters in, the cameras cannot see the projector, so production cannot
  automate it. It exists because the projected-surface homography is not a
  fish-plane transform (contract, "Step 4"). Today's accuracy check bounds
  that error: the homography residual is ≤ 0.6 mm, and the floor edge and the
  wall top agree within 0.6 mm.

## 2. The 2026-11-10 blocker is cleared

AGENTS.md: "guided daily registration is blocked until the Citrus commissioning
release is refit on the current canvas; Orange's canvas import reports the
homographies `stale_or_incompatible`." That status is ORANGE's own check
(`src/gui/spatial_layout/citrus_import.cpp:249`); Citrus never emits it. I
re-ran each part of it read-only against today's rig files
(`~/citrus/targets/rigs/omnifin0/shadow/`), using Orange's own code where it
is header-only:

- **Active homography pointers** (`homography_active_arena_N_<serial>.json`):
  - schema `citrus.calibration.active_homography` v1, `accepted`, rig, canvas,
    arena and camera match;
  - `target_plane` is `projected_surface`, and the direction is
    camera-native → final display canvas;
  - verification, quality, orientation validation and source photometry are
    all `passed`;
  - accepted 2026-11-13T00:31:21Z.
- **`configuration_fingerprint`** equals Orange's
  `expected_homography_configuration_fingerprint` recomputed from the current
  `shadow.json` camera calibrations, on all four cameras.
- **Canvas compatibility**
  (`canvas_compatibility::validate_active_artifact_canvas`): `shadow.json`
  changed after acceptance (sha 698bcfb6… vs accepted 61f337e7…). The active
  commissioning release
  (`commissioning_rig_canvas_commissioning_2026-11-13_recommission_after_projector_move`)
  anchors that change. Result: `compatible = true`, basis
  `projection_geometry_identity_v2`, warning
  `canvas_experimental_region_geometry_changed`, on all four cameras.
  - Citrus classifies the same change as non-geometry: dish_config, refraction
    and z_eff only, warning `canvas_non_geometry_calibration_state_only_change`.
  - The two classifications differ. It doesn't block, but it is worth one look
    by citrus-agent.
- **Citrus side** (citrus-agent, by reading the code): `begin_daily_registration`
  needs `BuildRuntimeCompatibilityStatus` compatible. The 11-13 release's
  commissioning sha, manifest and rig config sha match, so expect it to be
  compatible.

So the 2026-11-13 recommission cleared the blocker, and AGENTS.md is stale on
this point. Neither side has been exercised live since; the first autorun
trial is that test.

## 3. The rim fit (`src/daily_rim_fit.{h,cpp}`)

Jeremy, 2026-11-15:
- **Wall top only:** the inner rim at the top of the wall. The floor edge,
  16-32 px inside, is an illumination edge.
- **Known radius:** fixed from the calipered 80.15 ± 0.3 mm, converted to
  camera px at the rim-top plane with the fish-plane scale model (scale file
  `47a9f050…`, z = P_floor + 2.8 mm wall). That gives 2142-2145 px per
  camera; only the centre is fitted.
- **Frame edge:** every rim is inside its camera's view by design (2010094's is
  within ~1 px of the bottom). So the fit should reach close to 100 % arc
  coverage, and coverage well below that is a fault with a per-arc reason.
- **Edge rule:** reuse the registration accuracy check's wall-top edge rule
  (`scripts/registration_accuracy_check.py`, which Jeremy approved).

**Per ray** (720 rays from the starting centre):
- The window runs from the known radius −80 px to +110 px, clipped where the
  frame begins (never discarded). Bilinear sampling needs 1 px of frame.
- Each radial sample averages 5 points over ±4 px along the rim.
- Bright-to-dark steps come from a derivative of Gaussian (σ 2 px,
  half-width 6 px, so an edge needs 6 real samples on both sides and sits at
  least 6 px inside the frame). A step must be at least
  max(0.4 DN/px, 1.2 % of the ray's floor-to-dark contrast); a ray under 8 DN
  contrast has no edge. This is the accuracy tool's rule.
- One addition for the production frames: the dark level is the darker of the
  window's end and its darkest 5 px stretch. The IR rim capture shows the rim
  as a ~10 px dark ring with a bright field again outside it, where the
  accuracy tool's "window end" level (built for filters-off frames, dark
  outside) undercounts the contrast.
- The step nearest the known radius is taken.

**Edge model per frame (method v3, 2026-11-15).** On 2010094's 11-04 IR frame,
Jeremy saw that the step rule (the bright-to-dark step nearest the known
radius) followed faint floor lines about 30 px inside the dark ring on the
bottom and left. The fit now picks one of two models per frame:
- `ir_dark_ring`: the rim images as a dark band, a single ring 5-15 px wide in
  IR, or two lines about 10 px apart in tonight's filters-out frames.
  - A matched dark-line response finds the strongest valley on each ray: the
    lower of the two flank means minus the core mean, so a plain step gives no
    response.
  - The edge point is the band's inner boundary: the strongest bright-to-dark
    step between 30 px inside the valley and the valley itself.
  - A ray with no valley (a dark field outside the rim) takes the window's
    strongest step instead. This is counted in
    `rays_ring_step_without_valley`.
- `wall_top_step`: the approved filters-out rule, unchanged.

The model is decided from the profiles around the starting centre:
`ir_dark_ring` when at least half the rays hold a valley of at least 15 DN.
The evidence separates cleanly: IR and tonight's frames give 0.72-1.00, the
11-15 uniform-gray frames at most 0.03. The launcher's declared NIR filter
position is recorded beside the choice, and a disagreement is a warning
(`edge_model_disagrees_with_declared_nir_filters`). Each fit reports, for each
side (right, bottom, left, top), its inliers and its median signed residual
against the fixed fit, so a side that follows another edge shows up as an
outlier.

**Evidence: 234 frames** (`~/orange_data/registration_accuracy/rim_fit_ir_review_20261115T215604Z/index.html`):
- the 222 IR rim captures from 34 sessions;
- the four 11-15 gray frames;
- tonight's four filters-out autorun frames (`calsess_2026_11_15T21_37_24Z`).

Each frame is shown before (v2) and after (v3). The four sides agree within
4.7 px on every frame (median 0.4-1.6 px), and coverage is at least 0.917.

v3 passes all 68 October-November IR frames and all eight filters-out frames.
Across all 234 it refuses 16:
- 14 June-August frames on `rim_center_uncertainty_too_large`. On those frames
  the known radius (from the 11-13 scale) is 15-26 px off the measured free
  radius, a different dish or geometry, so the fixed fit's residuals are
  inflated.
- 2 frames on an arc gap (`rim_arc_gap_too_large`).

The fixed centre moves 3.5-7 px (median) against v2, and up to 26 px, which
is where v2 mixed edges.

**Which line is the rim?** This is open, for Jeremy.
- On tonight's filters-out frames, v2 sat about 6-7 px outside the band's
  inner boundary (on the step into the outer line). v3 sits on the band's
  innermost boundary. Their centres differ by 5.5-6.2 px.
- Against the known radius (80.15 mm at the rim-top plane), v3's free radius
  is +2.4 to +4.6 px (median) on the current-geometry IR frames and -6.9 px on
  tonight's frames. These are different dishes and days, all inside the 20 px
  check.
- No saved session shows one dish both in IR and with the filters out.
- The centre does not depend on the radius: fixed and free centres agree
  within 1.5 px everywhere.

**The fit:**
- The model radius is R + ec·cos2t + es·sin2t, because the rim is a slight
  ellipse in the image (2-10 px: lens distortion or a small tilt).
- A robust free-radius fit decides which points belong to the rim. Its band
  is set once, from the provisional fit over every edge point, and held while
  the fit is refined: clamp(max(4 × median |residual|, 8 px), 8 px, 12 px).
  The floor edge is 16-32 px inside, so the 12 px cap keeps it out.
- Why that rule (fixed 2026-11-15 after the rig trial `20261115T211404Z`):
  the first rule recomputed 3 × 1.4826 × MAD from the trimmed points every
  round, so it shrank with them. On 2010093 it fell to 4.37 px. The wall-top
  edge has coherent arcs 5-10 px off the ellipse model (not random scatter:
  2010093's 21° right arc sits +5..+10 px, median +7.7). The fit dropped that
  arc and was refused at coverage 0.892, although its centre was good (fixed −
  free 0.20 px). The coordinator's first proposal (2.5 × median, 6 px floor)
  is tighter than the old rule on most frames and makes things worse. On the
  11-15 gray frame of 2010093 it gives coverage 0.900 with a 33° gap, and on
  the trial's 2010095 0.856. The 8 px floor is what covers the coherent arcs.
- The **primary fit** fixes R at the known value and fits the centre (and
  ellipticity) on those inliers.
- Reported: centre σ from σ²(JᵀJ)⁻¹, the free fit, |fixed − free centre|, arc
  coverage, and every gap ≥ 3° with its side and reason (frame_border /
  no_edge / off_rim_edge).

**Evidence** (CLI `tools/daily_rim_fit_cli.cpp`, read only; 720 rays):

| Frame | Camera | Inliers | Coverage | Largest gap | Centre σ px (mm) | Free − known R px | Fixed − free centre px |
|---|---|---|---|---|---|---|---|
| 11-04 IR rim capture (production path) | 2010093 | 584 | 0.91 | 5° | 0.33 (0.006) | +0.9 | 0.22 |
| | 2010094 | 601 | 0.94 | 3° | 0.33 (0.006) | +0.5 | 0.18 |
| | 2010095 | 646 | 0.96 | 5° | 0.17 (0.003) | +2.9 | 0.06 |
| | 2010096 | 683 | 0.98 | 2° | 0.24 (0.004) | −0.3 | 0.03 |
| 11-15 uniform gray 64, filters off | 2010093 | 718 | 1.00 | 0° | 0.19 (0.004) | −1.7 | 0.01 |
| | 2010094 | 662 | 0.93 | 12° (bottom, frame) | 0.20 (0.004) | −2.7 | 0.27 |
| | 2010095 | 695 | 0.99 | 2° | 0.16 (0.003) | −1.9 | 0.11 |
| | 2010096 | 678 | 0.98 | 2° | 0.21 (0.004) | +2.3 | 0.22 |

- **The free radius equals the known radius within 3 px everywhere.** So the
  edge found is the wall top as modelled, and the calipered size is
  consistent with the camera.
- **Agreement with the accuracy tool** (`wall_top_fixed_radius`, v3) on the
  gray frames: 0.8-2.5 px, and 5 px on 2010096, where the tool had no points
  on one side.
- **Against the 11-04 operator-accepted centres:** 2-12 px. Their radii were
  8-30 px larger than the known radius.

**Tolerance rule, all twelve fits** (four rig-trial fits recomputed from their
saved edge points, the eight frames above rerun with the CLI). "Before" is
the first rule; "after" is the provisional-once rule with k 4, floor 8, cap
12. Centre shift is fixed centre after minus before.

| Fit | Camera | Provisional median px | Tolerance px before → after | Coverage before → after | Largest gap before → after | Centre shift px |
|---|---|---|---|---|---|---|
| rig trial 11-15 21:14Z | 2010093 | 1.72 | 4.37 → 8.00 | **0.892 (refused) → 0.961** | 21° → 7° | 0.16 |
| | 2010094 | 1.45 | 5.46 → 8.00 | 0.969 → 0.972 | 5° → 5° | 0.14 |
| | 2010095 | 2.94 | 12.0 → 11.8 | 0.994 → 0.994 | 2° → 2° | 0.00 |
| | 2010096 | 1.23 | 4.75 → 8.00 | 0.939 → 0.986 | 11° → 1° | 0.16 |
| 11-04 IR | 2010093 | 4.99 | 12.0 → 12.0 | 0.914 → 0.914 | 5° → 5° | 0.00 |
| | 2010094 | 5.20 | 12.0 → 12.0 | 0.947 → 0.947 | 2° → 2° | 0.00 |
| | 2010095 | 1.06 | 3.85 → 8.00 | 0.989 → 0.992 | 1° → 1° | 0.06 |
| | 2010096 | 2.18 | 9.82 → 8.74 | 0.961 → 0.953 | 3° → 3° | 0.23 |
| 11-15 gray 64 | 2010093 | 2.06 | 9.18 → 8.25 | 1.000 → 0.997 | 0° → 1° | 0.09 |
| | 2010094 | 1.87 | 7.08 → 8.00 | 0.931 → 0.933 | 13° → 13° (bottom, frame) | 0.09 |
| | 2010095 | 1.77 | 7.49 → 8.00 | 0.983 → 0.981 | 2° → 3° | 0.03 |
| | 2010096 | 2.42 | 10.3 → 9.68 | 0.983 → 0.967 | 2° → 2° | 0.10 |

Every fit passes, with a lowest coverage of 0.914 (11-04 IR 2010093, at the
cap and unchanged). The fixed − free centre is ≤ 0.27 px everywhere, and no
centre moves by more than 0.23 px. The other guards are unchanged.

**Review images** (`src/daily_rim_fit_images.{h,cpp}`) sit beside each
`daily_rim_fit.json`, refused fits included:
- `daily_rim_frame.png`: the grey frame the fit ran on, native and lossless.
- `daily_rim_fit_overlay.png`: the view at 1/4 with:
  - the fixed fit (white) and the free fit (magenta);
  - inlier points (green) and rejected points (red);
  - every uncovered arc (orange), labelled with side, width and reason;
  - a summary strip.
- `daily_rim_fit_zoom.png`: 320 × 320 native-px crops at the top, right,
  bottom and left, shown ×2, in the accuracy tool's layout.

The autorun writes them on worker threads in stage `saving_rim_images`
(about 1 s per camera). Their paths are listed in:
- the pending-confirmation file (v2, `targets[].images`);
- `awaiting.rim_images` in the status;
- the launcher's AWAITING block;
- the result's `rim_fits.<serial>.images`.

A fit is not offered for confirmation if its images failed
(`rim_images_write_failed`). The CLI takes `--images <dir>`.

**Refusal limits** (recorded in each fit's output):
- inliers ≥ 180;
- coverage ≥ 0.90;
- no single gap > 20°;
- centre σ ≤ 1.0 px;
- |free − known R| ≤ 20 px;
- |fixed − free centre| ≤ 3 px.

A rim past the frame edge is a warning; a cut arc is refused through the gap
rule, with reason frame_border.

Synthetic tests (`tools/daily_rim_fit_tests.cpp`, ctest `daily_rim_fit_tests`):
- an IR-style full ring;
- a 5 px ellipse;
- a rim 3 px from the frame bottom (fits, coverage > 95 %);
- a rim cut by the frame (refused, bottom arc reason frame_border);
- a short arc (refused);
- a floor-edge decoy 28 px inside (the wall top is chosen);
- a known radius 30 px off (refused, with the radius disagreement named);
- a filters-off step edge;
- a coherent +7 px arc over 25° on a sharp edge (the rig-trial case): kept by
  the rule, and lost with a 3 px band, which reproduces the refusal.

`tools/daily_rim_fit_images_tests.cpp` (ctest `daily_rim_fit_images_tests`):
- the frame PNG is byte-identical;
- the overlay and zoom have the expected sizes;
- a refused fit still gets its frame and overlay;
- a size mismatch writes nothing.

## 4. The autorun: design and recommendation

**Recommendation: (a) one human confirmation, from anywhere, at the single
point where a judgement is recorded.** Everything else is mechanical. (b), the
quality-gate route, is built on the dish-registration branch behind
`kDishRimAutoAcceptWritesEnabled = false`. It is not usable until Palette's
recompute PR is deployed and Citrus admits contract v3, and then it would need
the v4/v3 writers here too.

So (a) is what runs today; (b) replaces the confirmation later by flipping one
switch, without changing the runner. A person must be in the loop at least
once anyway, because rim v3 is by definition an operator-confirmed artifact.

Sequence (`ORANGE_GUI_DAILY_REGISTRATION_AUTORUN=1`, launcher
`scripts/run_daily_registration_autorun.sh`):

1. **Launch:** the launcher starts Citrus (autorun omnifin0/shadow,
   wait_for_control, no protocol start) and the Orange GUI, which streams with
   recording, YOLO, crops and pose off.
   - Between the two, `scripts/citrus_rig_claim_ready.py` waits until Citrus
     is ready for omnifin0.
   - A Citrus that claims one rig per process (draft #124) has
     `status.rig_claim`. It is null, and commands are refused with
     `rig_binding_no_rig_claimed`, until the claim; the helper waits for
     `rig_id` omnifin0, and a claim for another rig fails.
   - A Citrus without the field (240c6cc) is ready at once.
2. **Import:** the autorun imports the Citrus canvas templates for the
   selected cameras.
3. **Begin:** `BeginWorkflow`, with the launcher's `--declared-by` standing in
   for the GUI's "dishes installed, water settled, IR filters in place"
   checkbox, as the empty-background capture does. Then the rim capture.
4. **Fit:** the rim fit runs per camera, and any refusal aborts the
   transaction (Citrus abort, restore). Otherwise each target's accepted
   circle becomes (fixed-radius centre, known radius). Writes
   `targets/<serial>_<arena>/daily_rim_fit.json` and the three review
   images (off the GUI thread; refused fits too), then
   `autorun_pending_confirmation.json` in the transaction dir, and then
   **waits**. Status: `status.local_control.daily_registration_autorun`, stage
   `awaiting_operator_confirmation`.
5. **Confirm:** the person reads the summary and looks at each camera's
   overlay and zoom images (paths printed by the launcher),
   then sends `confirm_daily_registration` (local control) with
   `{transaction_id, step: "rims_and_acceptance", declared_by}`:
   - it is mutating and needs the GUI start permission;
   - it refuses an unknown SO_PEERCRED peer;
   - it is recorded with the peer.

   It means "these rims follow the inner rim, and accept and select the
   resulting translation if Orange's automatic geometry QC passes." Timeout:
   abort.
6. **Write rims:** the rim v3 artifacts are written (operator-confirmed: the
   person did confirm), then Citrus `create_daily_registration_candidate`.
7. **QC:** automatic geometry QC (`GeometryReviewPasses`). If it fails, the
   candidate is rejected and the run ends without changing runtime.
8. **Accept and select:** `AcceptAndSelect` with policy
   `orange_daily_registration_autorun_rim_only_v1`. `verification` carries
   the confirmation record and the rim-fit summaries, and records that no
   manual visual alignment was done. Orange then selects
   `selected_daily_registration`.
9. **Result:** `ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_RESULT_JSON` gets the
   transaction, per-target fits, translation, accepted artifact and
   selection, and the GUI exits if asked. Rollback is
   `select_daily_registration_runtime_mode base_only`.

**Not automated:** the manual visual alignment (eye on the outline), by
design; see section 1. Its absence is recorded in `verification`.

**The IR path:** the autorun uses the normal camera preset (`GPO_0` on
2010096 follows exposure, so the IR strobe pulses as in production), not the
guided-capture calibration timing config, which holds the strobe low. The rim
is captured on the production optical path.

### Sudo-wrapper allow-list (needs a wrapper reinstall after promotion)

The wrapper source (`scripts/orange_gui_validation_wrapper.sh`) and the
forwarder (`scripts/run_gui_aq_off_validation.sh`, which forwards every
`ORANGE_GUI_DAILY_REGISTRATION_AUTORUN*` key) are updated on the branch. New
GUI env keys:
- `ORANGE_GUI_DAILY_REGISTRATION_AUTORUN`;
- `ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_CITRUS_CONFIG_PATH`;
- `ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_FISH_PLANE_SCALE_PATH`;
- `ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_DECLARED_BY`;
- `ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_RESULT_JSON`;
- `ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_CONFIRM_TIMEOUT_SECONDS`;
- `ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_EXIT_AFTER_COMPLETION`.

## 5. Before and after

The accuracy check takes the before session (today's
`calsess_2026_11_15T19_47_21Z_shadow`, base geometry) and an after session
(same capture command, after the autorun has selected the registration;
filters off for the projected crosshair). It reports the per-arena dish
centre minus crosshair before and after, and the applied translation. NOT YET VERIFIED: whether Citrus's calibration scene
`experimental_area_center_and_outline` draws the crosshair at the effective
(translated) centre once the selection is active. If it draws the canonical
centre, the after-capture shows no change and the comparison must use the
applied translation from the selection instead. This needs checking, read-only,
in Citrus's scene code before the trial.
