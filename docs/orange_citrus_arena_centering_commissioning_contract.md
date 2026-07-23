# Orange–Citrus Arena Centering Commissioning Contract

Status: implementation contract, schema version 2

## Purpose and scope

This workflow centers each persistent Citrus arena on the raster center of its
associated Orange camera without requiring an accepted camera/projector
homography. It is a commissioning operation for a bolted rig. It is not the
daily dish-registration workflow: daily registration may translate removable
dish geometry, but it must not rewrite the canonical canvas.

The required commissioning variable is the canonical arena center in
final-display canvas coordinates. An optional, separately armed variable is
the even square arena size: it is chosen as the largest square whose complete
projected boundary remains inside the native sensor with a configured safety
margin. The experimental-area shape and physical dimensions remain in
arena-relative coordinates and move with the arena. When the arena size
changes, its local center coordinates are rebased by half the size delta so
its offset from the arena center is invariant. A later installed-dish
registration may record a non-canonical daily offset, but it is a separate
artifact and cannot be folded into this transaction.

The required physical state is the unobstructed dry projection shelf: holder,
dishes, water, and camera filters removed. The cameras run through Orange's
calibration preflight with PTP still enabled, the mapped strobe suppressed,
and the previously qualified projector foreground gray (currently 72).

## Deferred operator-facing terminology cleanup

The current implementation and API use `arena_centering` terminology, but this
transaction is not the daily centering of a removable dish. It is one part of
the broader stable-rig commissioning product and permanently changes canonical
canvas geometry.

Keep the current code, environment-variable, launcher, and socket-method names
stable while the complete calibration procedure set is still being built and
validated. After commissioning centering/resizing, homography, physical-scale
validation, installed-holder/tank evidence, daily dish registration, and
recording provenance form one demonstrated operator workflow, perform a
coordinated terminology pass:

- label this existing operator workflow **Canvas/Arena Commissioning**;
- label the translation-only removable-dish workflow **Daily Dish
  Registration**;
- update UI headings, help text, launcher descriptions, and operator docs
  together so “centering” cannot refer ambiguously to both products;
- retain existing machine-facing names as compatibility aliases unless a
  versioned protocol deliberately replaces them.

This deferred rename is documentation and usability cleanup, not a geometry or
calibration-math change. It must not reinterpret `commit_arena_centering` as a
daily operation.

## Ownership

- Citrus owns the live arena centers, projection rendering, staging,
  validation, canvas persistence, compare-and-swap, rollback, and save receipt.
- Orange owns synchronized camera acquisition, center-fiducial detection, the
  four-edge/corner detector, projective maximal-square proposal, local
  Jacobian solve, guided operator state machine, evidence storage, and the two
  explicit commit arms.
- The operator owns the physical-state confirmation and the final permission
  to persist verified centers.

Orange never edits `shadow.json` or another Citrus canvas file directly.

## State machine

```text
idle
  -> begun/baseline presented
  -> +X presented -> captured
  -> -X presented -> captured
  -> +Y presented -> captured
  -> -Y presented -> captured
  -> candidate presented -> verification captured
  -> [optional] resized candidate presented -> center and edge verification captured
  -> committed and prior projection restored

Any error, timeout, cancellation, failed quality gate, failed verification,
or failed save:
  -> original centers restored in memory
  -> prior projection restored after a presentation fence
  -> canvas file left unchanged
```

Projection states are serial. Within every state, Orange requests one grouped
PTP stability-reference capture, waits the configured inter-capture interval,
then requests a second grouped PTP confirmation capture. Both captures must
refer to the same Citrus scene revision and content fingerprint. Center fits
must agree within 2 native camera pixels by default; stages containing the
arena rectangle also require all fitted corners to agree within 12 native
camera pixels. The looser corner limit accounts for the rectangle detector's
4512-to-1280 downsample and broad luminous edge, while the independently fitted
center remains limited to 2 native camera pixels so rigid projection motion is
still rejected. The recorded per-camera corner allowance adds half the observed
luminous-band width—the uncertainty between observing one band edge and both—to
that fixed 12 px detector/downsample repeatability budget. The accepted
confirmation rectangle must still pass center agreement, visibility, and
geometry gates. A one-sided rectangle edge is weak evidence rather than a
physical-alignment failure. Orange accepts it only when the resulting rectangle
center still agrees with the independent center fiducial; otherwise it
recaptures the same immutable Citrus scene, up to five grouped attempts by
default. Exhausting that bound persists the final failed capture before aborting.
The analysis tasks operate on the grouped
capture's owned RGBA buffers, and the state machine joins all tasks before it
may stage the next Citrus state. The same bytes are persisted and checksummed;
camera acquisition buffers are never retained by an analysis task.

## Display ownership and settling

The commissioning launcher treats the operator display and stimulus display
as separate named XRandR outputs. For the Shadow rig the defaults are:

- Orange and the Citrus operator GUI: `DP-0`;
- Citrus fullscreen stimulus output: `DP-3`.

Orange is positioned on the operator monitor before its window becomes
visible. Startup fails if either named output is absent, both names resolve to
one monitor, or Orange's resulting window bounds overlap the reserved stimulus
monitor. Citrus resolves the stimulus output by its configured name instead of
monitor enumeration order and fails if it resolves to the primary monitor.

Immediately before a grouped capture request, Orange checks Citrus status for
the expected stimulus monitor, active fullscreen window and render loop,
nonzero rendered-frame count, and matching configured/observed framebuffer
dimensions. It also requires the OpenGL viewport to begin at `(0, 0)` and cover
the complete framebuffer. This prevents a transient smaller startup framebuffer
from leaving a stale, compressed viewport after the fullscreen window settles.
This status check cannot prove that an unrelated desktop window is
not occluding the projector, so the two-capture image-content gate remains
mandatory.

The default arena-state settle is 1000 ms. The launcher also applies a 1000 ms
post-presentation-fence delay inside the grouped-capture controller; this
protects generic calibration scenes such as the later homography grid as well
as arena-centering scenes. The stability confirmation is requested after an
additional 300 ms by default. These values are configurable, but reducing or
disabling them must be explicit.

## Geometry and solve

The target for a camera with native dimensions `W x H` is the sensor raster
center:

```text
target_camera_px = ((W - 1) / 2, (H - 1) / 2)
```

For each arena, Citrus presents the arena outline and a compact center
fiducial at five absolute staged center states: baseline, `+X`, `-X`, `+Y`,
and `-Y`. Orange fits the center fiducial geometrically in native camera
pixels. Brightness centroid alone is not an accepted detector.

With a symmetric probe magnitude `p` in final-display canvas pixels:

```text
J[:,x] = (camera_center(+X) - camera_center(-X)) / (2p)
J[:,y] = (camera_center(+Y) - camera_center(-Y)) / (2p)
delta_canvas = inverse(J) * (target_camera_px - camera_center(baseline))
candidate_canvas = round(original_canvas + delta_canvas)
```

The solver rejects missing detections, ambiguous fits, probe displacements
below the configured minimum, excessive symmetric nonlinearity, an ill-
conditioned or singular Jacobian, an excessive requested move, or a
candidate outside the canvas. One bounded correction refinement is allowed
only after an otherwise valid verification capture.

For optional sizing, Orange fits four supported projected centerlines and
their corners. The diagonal intersection must agree with the center fiducial.
The observed square corners and their known canvas coordinates define a local
projective mapping. Orange searches the even canvas sizes at the verified
center and selects the largest whose four projected corners remain inside the
sensor safety rectangle plus a prediction reserve. The reserve covers
capture-to-capture edge-fit jitter; it does not weaken the configured measured
safety margin. Citrus stages that size and Orange recaptures it;
predicted-to-measured corners, all four margins, squareness/evenness, center
agreement, and near-maximality must pass before persistence is possible.

A negative determinant is not itself rejected: a camera/projector mounting
may legitimately reflect one axis. The probes establish orientation
empirically; invertibility, condition number, displacement, and symmetric
linearity are the safety gates.

The 2026-07-19 Shadow commissioning capture established the current effective
relationship independently of a homography fit. Across Cam2010093 through
Cam2010096, logical canvas +X moved approximately `+12.39` to `+12.59` camera
pixels per canvas pixel in camera X, while logical canvas +Y moved
approximately `-12.41` to `-12.65` camera pixels per canvas pixel in camera Y.
All four canvas-to-camera Jacobian determinants were negative (approximately
`-154` to `-159`). This is accepted existing behavior. It must be preserved in
calibration provenance rather than normalized away by changing rendering.

## Camera-specific commissioning size versus experimental-area size

The canonical arena square is a camera-support and projector-layout region.
It is not itself the physical experimental area. Maximizing that square
independently for each camera may legitimately produce different canvas-pixel
sizes because camera height, macro-lens focus/magnification, projector scale,
sensor crop, residual optical distortion, and integer/safety-margin
quantization can differ between optical paths.

The experimental area has a separate metric contract. For the current palm
dishes it remains a circle with a `40 mm` radius (`80 mm` diameter). Its
canvas-pixel radius is allowed to differ by arena because it is derived from
that arena's projected-surface pixels/mm scale. A commissioning resize must
not copy a pixel radius from one arena to another, scale the experimental area
with the enclosing arena square, or infer physical experimental-area size
from the percentage of the camera sensor occupied.

When Citrus stages or commits a canonical arena resize, it therefore:

1. leaves experimental-area shape and radius/width/height in millimetres and
   canvas pixels unchanged;
2. rebases only the arena-relative local center by half of the arena-size
   delta, preserving the experimental area's offset from the arena center;
3. recomputes only the enclosing arena region's physical span from the
   arena-specific projected-surface pixels/mm scale; and
4. requires the stronger layout-save arm and records these invariants in the
   receipt.

This is structurally safe against accidentally changing an `80 mm`
experimental area merely because the camera-visible commissioning squares
are different sizes. Its *metrological* accuracy still depends on each
arena's projected-surface pixels/mm scale being correct. Equal `40 mm` radius
fields in configuration are evidence of equal intent, not independent proof
that four projected radii are physically identical. Cross-arena physical
equality should eventually be qualified with an immutable scale artifact or
known-length target in each arena. Lens distortion and camera height/focus
differences may be modeled later if residuals show they are material; they are
not silently absorbed by changing the experimental-area metric dimensions.

### 2026-07-19 unarmed Shadow observation

The first complete four-camera unarmed resize validation proposed:

| Camera | Arena side, canvas px | Approximate enclosing span from current scale |
| --- | ---: | ---: |
| `2010093` | `348` | `83.466 mm` |
| `2010094` | `350` | `83.843 mm` |
| `2010095` | `352` | `83.091 mm` |
| `2010096` | `354` | `84.260 mm` |

At the time of the run, the separate experimental-area fields were internally
consistent with a common physical radius:

| Camera | Projected-surface scale | Experimental radius, canvas px | Experimental radius, mm |
| --- | ---: | ---: | ---: |
| `2010093` | `4.1693377 px/mm` | `166.7735` | `40` |
| `2010094` | `4.1744885 px/mm` | `166.9795` | `40` |
| `2010095` | `4.2363100 px/mm` | `169.4524` | `40` |
| `2010096` | `4.2012701 px/mm` | `168.0508` | `40` |

Thus, different experimental-area pixel radii are expected and necessary
when the per-arena scale differs. For each row, `radius_px / px_per_mm =
40 mm` within stored precision.

The enclosing commissioning spans differ by about `1.17 mm` (`1.4%`). The
opposite projected-edge mismatch was at most `0.16%` for top versus bottom
and `0.45%` for left versus right, which is more consistent with small
camera-specific magnification/placement differences than a large camera-tilt
failure. The configured experimental areas remained `40 mm` radius and were
not changed. The run was deliberately unarmed; Citrus rolled back the staged
centers and sizes and left the canvas checksum unchanged.

## Citrus local-control contract

All mutating messages require unique `request_id` and idempotent
`operation_id` values. Commands are applied on Citrus's main thread. Orange
polls status until the returned `presentation.rendered_frame` fence has been
crossed. Orange then waits 500 ms before requesting camera frames. This drains
an exposure that may have begun under the previous projection and covers the
100 ms exposure plus the 5 fps frame interval with scheduling margin.

Methods:

- `begin_arena_centering`: starts one transaction, snapshots original centers
  and the canvas checksum, locks the exact arena/camera target set, and
  presents `arena_outline`.
- `stage_arena_centers`: stages absolute integer center values for every
  target, never deltas. An optional resized-candidate row also carries even
  `arena_width_canvas_px` and `arena_height_canvas_px`; it refreshes the scene
  revision and presentation fence.
- `arena_centering_status`: returns transaction state, original/staged centers,
  scene identity, presentation fence, last command, and any receipt.
- `commit_arena_centering`: requires `save_verified_centers_armed=true`, the
  expected base checksum, and a verification summary. It performs an atomic
  compare-and-swap update of the active camera's canonical center fields and
  then restores the prior scene. If any size changed, it additionally requires
  `save_verified_layout_armed=true`.
- `abort_arena_centering`: restores the original in-memory centers and prior
  scene, without changing the canvas file.

Every requested center row names `arena_id`, `camera_id`,
`center_x_canvas_px`, and `center_y_canvas_px`. A transaction is rejected if
an arena is absent, the active camera differs, targets are duplicated, an
experiment is armed/active, another calibration transaction is active, or the
loaded canvas/config identity cannot be proved.

## Atomic save and rollback

At `begin`, Citrus records the canonical canvas path and SHA-256 checksum. At
`commit`, Citrus rereads the file. A checksum mismatch rejects the commit and
rolls back; it never overwrites concurrent edits. Citrus starts from that
persisted JSON and changes only the explicitly reported placement fields plus
legacy aliases that already exist. A resize updates active-camera width and
height, rebases only the two experimental-area local center coordinates, and
recomputes `arena_region_width_mm/height_mm` from the accepted projector
pixels/mm scale. Experimental-area shape, radius/width/height in pixels and mm,
and offset from arena center must remain invariant. Citrus serializes the
result to a same-directory temporary file, flushes it, atomically renames it
over the destination, and returns the new checksum.
Failure before the rename leaves the destination unchanged.

The receipt records old and new centers, old and new file checksums, canvas
path/identity, target camera IDs, transaction and operation IDs, UTC time,
verification metrics, and rollback/restore disposition.
It records whether arena size changed, the local-center rebase, old/new pixel
and physical arena sizes, the scale used, and proof that the experimental-area
shape/size and center offset were preserved.

Canonical arena-placement changes make every homography whose configuration
fingerprint includes those centers or sizes stale. Version 2 records that dependency
in the receipt and blocks claiming homography validity; it does not promote,
rewrite, or silently reuse a homography. Fresh plane-specific homographies
must be fitted and accepted afterward.

## Orange evidence contract

One calibration session contains, for every projection state and camera:

- the exact native-resolution image analyzed;
- camera/local frame IDs, PTP/device/system timestamps, and image checksum;
- Citrus transaction, stage, scene revision, content fingerprint, effective
  center, and presentation fence;
- detector result, fit quality, target raster center, and annotated overlay.
- for homography candidates, local dot-core/annulus photometry from that exact
  source image, including saturation and contrast gate results plus the
  immutable projector-intensity report path and SHA-256 that supplied the
  thresholds;
- on baseline/candidate states, the four fitted edges, corners, individual
  sensor margins, diagonal center, and boundary overlay;
- when resizing, the projective proposal and the recaptured resized-candidate
  prediction/measurement comparison.

The session also contains a solver artifact per camera, a before/after
comparison, the cross-camera grouped-capture PTP span for every state, the
commit/abort receipt, the Citrus config checksum, and the physical-state and
operator-arm declarations. Output publication uses Orange's guarded output
ownership path.

Analysis failures use a persist-before-abort rule. All per-camera tasks join
before the gate is evaluated. If any center, rectangle, identity, or PTP gate
fails after a grouped frame exists, Orange marks the transaction failed but
first saves that exact grouped capture, every available center and rectangle
overlay, and the complete detection batch in the same calibration session.
Only after persistence succeeds does Orange request the Citrus rollback. No
new projection state, solver, resize, or geometry mutation may run in between.
If evidence persistence itself fails, the transaction still aborts and the
result reports both the analysis context and persistence failure.

## Commit gates

Commit is forbidden unless all of the following hold:

1. Exactly one valid result exists for every requested camera in all required
   grouped capture states.
2. PTP is enabled and each grouped capture satisfies the configured timestamp
   span limit.
3. All captured states identify the expected Citrus transaction, stage,
   revision, fingerprint, target set, and crossed presentation fence.
4. Each detector and Jacobian passes its quality gates. In particular, an
   elongated center marker is treated as mixed-stage/ghosted evidence and is
   rejected rather than passed to the solver.
5. The final verification agrees with the residual predicted for the nearest
   renderable integer projector position within tolerance for every camera.
   The raw sensor-center residual is retained, but unavoidable integer canvas
   quantization is not misclassified as calibration error.
6. The expected canvas checksum still equals the checksum from `begin`.
7. The operator explicitly armed automatic persistence before `commit`.
8. If sizing is enabled, every measured edge clears the configured safety
   margin, the square is even and near-maximal, predicted and measured corners
   agree, and the stronger layout-save arm is set.

The four-camera operation is all-or-nothing. A single failed camera aborts and
rolls back all staged centers after preserving the failed grouped evidence.

## Non-goals

- Arena centering itself does not infer a homography. The guided commissioning
  runner may explicitly continue from an accepted centered layout into the
  separate immutable homography candidate/review lifecycle; promotion remains
  independently armed.
- It does not infer the daily dish placement or experimental-area translation.
- It does not compensate fish-plane parallax or lens distortion.
- It does not validate biological mask behavior near the wall.
