# Commissioning record: evaluation and plan, 2026-11-13

Goal (Jeremy, 2026-11-13): every commissioning product and the evidence
behind each acceptance land in one durable commissioning folder, written by
the tools, so that agents can run a (re)commissioning end to end with no hand
edits. Operators only do the physical steps.

## 1. Evaluation of the 2026-11-13 dry recommissioning

Where everything ended up after the session:

| Item | Location | Durable | In the Citrus release |
|---|---|---|---|
| Homographies, scale, acceptance receipts | Citrus `calibration_artifacts/` | yes | yes, by sha256 |
| Source sessions (4) | `~/orange_data/calibrations/sessions/` | yes | yes, by sha256 |
| Projector intensity report | `~/orange_data/calibrations/commissioning/projector_intensity_*` | yes | no |
| Brightness-map image sessions (4) | sessions folder | yes | no |
| Brightness-map analysis (numbers, maps, script) | agent scratchpad | no | no |
| Claude's review evidence (overlays, reprojection plots, step logs, finalize result) | agent scratchpad and `/tmp` | no | no |
| Physical and host state (filters, Brightness, IR, fixtures, geometry) | conversation, rig-designer record | partly | no |

The fits were durable; the evidence behind "Claude accepted it" and all of
the brightness work were not. The scratchpad and `/tmp` copies were moved by
hand into
`~/orange_data/calibrations/commissioning/recommission_20261113_omnifin0/`
(README, SHA256SUMS) the same day. That is the gap this plan closes.

Further findings from the session:

- Labels: the capture launcher can only produce projection-surface captures.
  Orange's schema has the physical-target stages (`camera_physical_*`) and
  purposes (`camera_only_physical_target_calibration`,
  `dry_physical_target_height_parallax_diagnostic`), but the GUI preset
  assumes NIR strobe, R72 filter, projector off, and the launcher cannot
  select a capture stage. A disk on the dish floor could only have been
  saved under a wrong label.
- Labels are never corrected after capture: image sets carry checksums and
  the session index and the Citrus release refer to them by sha256. Fixes go
  into the code so the capture writes the right label.
- Operator steps that broke automation: a collapsed Spatial Layout window
  stalls group capture; the scale step used the launcher default gray (76)
  instead of the commissioned gray (24); Citrus refused the scale
  load/promote over local control (`calibration_transaction_active`) and
  Jeremy promoted in the GUI; YOLO detections were on in a calibration
  launcher; the GPO_0 restore runs before the stream stops (IR strobe
  above 8 % duty unless the IR is unplugged).

## 2. Decisions (2026-11-13)

- The fish-height scale uses the 3 mm hole target on the floor of an
  installed, dry dish. The winglets set the floor height wet or dry; the
  camera looks down, so water under the dish does not matter; water over a
  fish changes the apparent scale by about 0.1 %, corrected by calculation.
  Water is needed only for what the fish see: the wet brightness map and the
  stimulus dot contrast.
- No hand edits of captured metadata, ever.
- No ad hoc captures (Jeremy, later the same day: "focus on getting things
  ready so we can do this properly instead of hacking around with it"). The
  wet brightness map, the floor-height disk, the dark frame and the dot
  contrast all wait for the changes below, and run as one session that writes
  one record.

## 3. The commissioning record

One folder per commissioning session:

```
~/orange_data/calibrations/commissioning/<rig_id>_<UTC start>_<reason>/
  record.json          # index: rig, reason, operator, agents, steps, verdicts, products, physical state
  README.md            # generated summary
  steps/<NN>_<name>/   # per step: launcher result JSON, Orange/Citrus logs, review images, gate values
  analysis/            # brightness maps, physical-target fits, dot contrast
  SHA256SUMS
```

`record.json` per step: the command and launcher version (git commit), the
calibration session ids with their `session_index` sha256, the products
(Citrus candidate set, receipt, active pointers, with sha256), the gates and
their values, the reviewer (`claude` with the session id, or `jeremy`), the
verdict and time, and the physical state declared for that step
(fixtures, filters, projector Brightness, IR plug, water fill, dish type per
arena, plane heights with their source and the rig-record commit). The
record is opened by the first step, appended by each launcher run, and
closed (made read-only, checksummed) after finalize. The record id and its
sha256 go into the finalize request as context once Citrus accepts it
(citrus-agent; agent-contracts).

## 4. Changes

Orange (dish branch), in this order:

1. Record writer: a small library + CLI used by every commissioning launcher
   (`run_gui_arena_centering_*`, `run_gui_guided_capture_smoke.sh`,
   `run_holder_fixture_validation.py`, the finalize client): `--record <dir>`
   opens or appends; the launcher copies its own logs and result JSON in, so
   nothing is left in `/tmp`.
2. Physical-target capture: launcher `--capture-stage` for
   `camera_physical_projected_surface`, `camera_physical_dish_base_inner_surface`,
   `camera_physical_fish_height`, and a projector-backlit preset (uniform gray
   under the target, filters as declared, projector on and visible to the
   camera) beside the existing NIR-strobe one. Plane heights come from the
   rig geometry record, not from GUI defaults.
3. Physical-target fit CLI around `orange::gui::projected_surface_scale::analyze`
   (same target definition and gates as the dry scale), writing the fit,
   overlay and gate values into the record. Fish-plane result as in
   `docs/omnifin0_calibration_procedure.md` section 5.
4. Brightness map tool: the 2026-11-13 analysis as a repo script (block
   means, levels, shape across grays, facing-edge ratios between neighbouring
   cameras), output into the record. Later a brightness-map artifact type that
   travels with recordings through the commissioning release.
5. Stimulus dot contrast (`docs/omnifin0_calibration_procedure.md` section 7):
   capture of the experiment's dot frames (scene from
   experiment-spec and Citrus, approved by Jeremy) through the wet stack;
   Weber contrast per dot `(I_dot - I_bg) / I_bg` from black-subtracted
   images, with the local background measured in an annulus around each dot.
   Scene: Citrus `static_dots` (new; citrus-agent PR after Jeremy's word).
   Orange side: launcher recipe that sends one `static_dots` frame per
   capture and waits for its presentation fence.
6. Dark frame with the projector light blocked: its own capture label
   (fixture/projector state `light_blocked`), so the black floor and the camera
   dark level are separated without notes outside the metadata.
7. Fixes found tonight: group capture independent of panel visibility;
   calibration launchers force YOLO off; scale step uses the commissioned
   gray; GPO_0 restored after the stream stops; Citrus scale promote over
   local control (citrus-agent); PDU switching of the IR outlets around the
   session (needs Jeremy's OK for PDU control from Orange).

Elsewhere: rig-designer keeps the geometry record (plane heights, dish
types); pancake-batter's `rig.yml` media stack update needs Jeremy's OK;
agent-contracts pin manifest with citrus-agent after commissioning.

## 5. Acceptance of this plan

A full recommissioning run by agents, with Jeremy doing only the physical
steps, produces one closed record in which every accepted product can be
traced to its sessions, gates, review images and physical state by sha256,
and nothing needed for that trace lives in `/tmp` or a scratchpad.
