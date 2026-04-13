# Spatial Layout UI Sketch

Purpose: sketch a practical Orange UI for authoring canonical layouts and
reviewing per-recording camera-view overlays before writing any implementation
code.

Date anchored: 2026-04-06.
Status: draft UI sketch, not implemented.

Related documents:

- `docs/spatial_layout_schema.md`
- `docs/spatial_layout_contract.md`
- `src/image_canvas.h`
- `src/usaf_resolution_ui.cpp`

## Design Split

The schema implies two different operator tasks:

1. author a canonical `arena_layout` template
2. register a particular camera view to that template for one recording

Trying to force both into one undifferentiated screen will make the workflow
confusing. The UI should therefore be split into:

- `Layout Template Editor`
- `View Registration`

The same canvas interaction model can be reused in both screens.

## V1 Constraints

- canonical shapes are only `circle` and `rectangle`
- runtime overlays are only `circle` and `oriented_rectangle`
- registration is only `identity`, `translation`, or `similarity`
- the operator is expected to work from an empty-dish or low-clutter frame
- no automatic homography or polygon editing in v1

## Screen 1: Layout Template Editor

Goal: create or edit the canonical `orange.calibration.arena_layout` artifact.

Suggested top-level layout:

```text
+--------------------------------------------------------------------------------------------------+
| Layout Template Editor                                                                          |
| layout_id [ bank4_circle_v1        ]  dish_design_id [ dish4_circle_v1 ]  coordinate [ layout_mm ] |
+----------------------+------------------------------------------------------+--------------------+
| Tools                | Template Canvas                                      | Inspector          |
| [Dish Circle]        |                                                      | layout_id          |
| [Dish Square]        |   outer dish / canvas boundary                       | coordinate_space   |
| [Zone Circle]        |                                                      | selected shape     |
| [Zone Square]        |   z0                    z1                            | type               |
| [Duplicate Zone]     |                                                      | position / size    |
| [Delete Zone]        |   z2                    z3                            | zone_id            |
|                      |                                                      | zone_index         |
|                      |                                                      | display_label      |
+----------------------+------------------------------------------------------+--------------------+
| Validation: 4 zones | inside boundary: yes | duplicate ids: no | overlaps: 0 | ordering: row-major |
+--------------------------------------------------------------------------------------------------+
| [New Layout] [Load Layout] [Save Artifact] [Export JSON Preview]                                |
+--------------------------------------------------------------------------------------------------+
```

Interaction model:

- the center canvas is a synthetic layout-space editor, not a live camera view
- the operator chooses an outer dish shape first
- zones are then added as circles or squares inside the outer shape
- selecting a zone exposes `zone_id`, `zone_index`, and geometry in the right
  inspector
- row-major ordering can be auto-assigned once and then edited manually

Why this split matters:

- `layout_id` and `zone_id` belong to the canonical artifact, not to one
  recording
- authoring in layout space keeps zone identity stable even when dishes shift in
  camera space later

## Screen 2: View Registration

Goal: align one camera's current view to an existing canonical layout and review
the resolved `camera_native_pixels` overlays before saving or using them.

Suggested top-level layout:

```text
+----------------------------------------------------------------------------------------------------------------+
| View Registration                                                                                              |
| camera [ 02010093 ]  frame [ empty_dish_01.png ]  layout [ bank4_circle_v1 ]  registration [ similarity ]     |
+----------------------+----------------------------------------------------------------+------------------------+
| Inputs               | Camera Canvas                                                   | Inspector              |
| [Load Latest Frame]  |                                                                | registration source    |
| [Capture Frame]      |   live image / frozen image                                    | residual_px            |
| [Load Layout]        |                                                                | fit_point_count        |
| [Detect Experimental |   experimental area + valid mask                               | orientation_status     |
|  Area Circle]        |                                                                |                        |
| [Reset Fit]          |                                                                | visible zones          |
|                      |   transformed z0 / z1 / z2 / z3 overlays                       | selected zone status   |
|                      |                                                                | selected geometry      |
+----------------------+----------------------------------------------------------------+------------------------+
| Tools: [Pan] [Zoom] [Canvas Edit Mode: registration|selected_zone] [Toggle Labels]                                |
+----------------------------------------------------------------------------------------------------------------+
| [Save Dish Mask Artifact] [Save Registration Preview] [Write Snapshot Preview]                                 |
+----------------------------------------------------------------------------------------------------------------+
```

Interaction model:

- the center canvas is a real camera image using the existing
  `orange::ui::begin_image_canvas(...)` pattern
- the operator can auto-detect the visible experimental-area circle, then use
  `registration` mode handles to drag the experimental-area center, scale, and
  rotation until zone overlays align with visible chambers
- in `selected_zone` mode, the operator can drag the selected zone center and
  resize handles directly on the camera overlay instead of editing only numeric
  fields
- circle zones stay circles under v1 similarity registration
- square zones are shown as `oriented_rectangle` overlays in the camera view
- the inspector shows residual, orientation status, and the set of visible zones

## Combined V1 Flow

A reasonable first implementation can still keep the UI simple:

1. Add a menu entry: `Calibrations -> Spatial Layout`.
2. Start on `View Registration`, because that is the operator's immediate task.
3. If the chosen `layout_id` does not exist yet, open `Layout Template Editor`
   as a modal or side workflow.
4. Return to `View Registration` after the template is saved.

That avoids building a heavy calibration browser before the data model exists.

## Canvas Behavior

The existing `src/image_canvas.h` helper is already close to what this needs:

- native pixel axes
- pan and zoom
- equal aspect
- image-underlay plotting

V1 canvas behaviors:

- left-drag selected handle
- Shift-drag constrains square aspect when authoring a square zone
- arrow keys nudge selected geometry by 1 px in camera space or 0.1 units in
  layout space
- `Fit` button resets the view to the full image or template bounds
- zone labels render at geometry centers

## Validation Panel

Both screens should expose explicit validation instead of silent acceptance.

Recommended checks:

- `layout_id` present
- unique `zone_id`
- unique `zone_index` when present
- zones inside outer dish boundary
- dish-mask `valid_geometry` inside `outer_geometry`
- registration residual below warning threshold
- symmetric layout requires `orientation_status != ambiguous`

## Recommended First UI Slice

Keep the initial implementation narrow:

1. Canonical layout authoring for circle/square dishes only.
2. Manual dish-mask drawing on a frozen frame.
3. Manual similarity registration to a saved layout template.
4. Resolved overlay preview with labels and zone selection.
5. Save JSON preview before any artifact-writing automation.

That is enough to validate the schema and operator workflow before adding
auto-fit or persistence plumbing.
