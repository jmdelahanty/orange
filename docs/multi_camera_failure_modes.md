# Multi-Camera Failure Modes

Date: 2026-04-21
Branch: `exp/gop-split-a16`

Related notes:

- `docs/ptp_sync_hardening_todo.md`
- `docs/ptp_recording_sink_experiment_plan.md`
- `docs/gpudirect_buffer_lifetime_review.md`

## Purpose

This note summarizes the distinct multi-camera failure modes observed during
recent split-GOP and PTP-gated recording work.

It is meant to be a compact matrix of:

- what fails,
- how it presents,
- what evidence supports it,
- what is probably *not* the cause,
- and what the next diagnostic should be.

## Summary Matrix

| Scenario | Symptom | Evidence | Current Read |
| --- | --- | --- | --- |
| Dual-camera `100 fps` `free_run` recording | Throughput collapse with split-GOP output overflow | backlog exceptions, `overflow_events > 0`, `peak_backlog_gops > max_inflight_gops` | Recording-path backlog overflow under unsynchronized high-rate multi-camera load |
| Dual-camera `80 fps` `ptp_gate` recording, no stagger | Both cameras drop to about `54-57 fps` | single-camera `80 fps ptp_gate` works, dual-camera `80 fps free_run` works, dual-camera `80 fps ptp_gate` fails | Synchronized burst contention once tightly aligned arrivals interact with recording |
| Dual-camera `80 fps` `ptp_gate` recording, `2 ms` stagger | Stable at about `80 fps` on both cameras | `0` camera drops, balanced helper routing, `overflow_events = 0` | Stagger relieves the synchronized burst problem at `80 fps` |
| Dual-camera `100 fps` `ptp_gate` stream-only, no stagger | Stable at about `100 fps` on both cameras | `0` camera drops, no stale dump | Raw synchronized acquisition is fine without recording |
| Dual-camera `100 fps` `ptp_gate` stream-only, `2 ms` stagger | Stable at about `100 fps` on both cameras | `0` camera drops, no stale dump | Offset alone is not enough to trigger stale-frame onset; recording pressure is part of the bad interaction |
| Dual-camera `100 fps` `ptp_gate` recording, nonzero stagger | One or both cameras collapse; bad camera eventually shows multi-second stale-frame lag | `latch_minus_frame_ns` jumps from `~9 ms` to seconds, `overflow_events = 0`, failure follows offset camera for larger offsets | PTP-gated offset acquisition becomes unstable at `100 fps`; this is a different mode than GOP backlog overflow |
| Dual-camera `100 fps` helper preprocess probes after cross-GPU prewarm | First helper queue-wait spike mostly removed, but acquisition still settles near `69-70 fps` | helperprobe6 first helper queue wait about `4 ms`, camera drops still `351-401` | CUDA/helper cold-start is real but not the whole failure; remaining issue is upstream acquisition timing/backpressure once helper routing is active |
| Dual-camera `100 fps` helper preprocess probes after deferred source release and acquisition cadence sidecar | Raw source buffers are now held until CUDA source reads are complete, but acquisition still settles near `69-70 fps` | helperprobe10 `free_run` and helperprobe11 `ptp_gate` completed with `0` source-release event misses; cadenceprobe1 shows both cameras jump from `~10 ms` frame deltas to `~20 ms` at frame `102` after helper routing starts at frame `101` | Premature source recycling and submit cost are not the remaining throughput root cause; the current target is an upstream GPUDirect/EVT/acquisition-buffer interaction triggered by helper routing |
| Dual-camera `100 fps` helper source-read no-op | Split-GOP routing stays active and throughput recovers to `100 fps` | helpernoop2: `1200` submitted, `600` primary routed, `600` helper dispatched, `0` camera drops, cadence sidecars keep sequential frame IDs through helper activation | Routing and submit overhead are not sufficient to cause the failure; helper GPU source access/copy is the trigger |
| Dual-camera `100 fps` force ring-copy with real helper source reads | Throughput still collapses to about `70 fps` | ringcopy1: `direct=0`, ring-copy active, frame IDs still jump `101 -> 103 -> 105` after helper routing starts, post-warmup drops `400-401` | The issue is not limited to camera-owned GPUDirect buffers; helper cross-GPU reads from acquisition-GPU memory are the stronger suspect |
| Dual-camera `100 fps` helper peer-copy byte sweep | Healthy until the helper copy payload reaches a few MiB; clear degradation by `8 MiB` | `0`, `4 KiB`, and `1 MiB` pass with no cadence jumps; `4 MiB` first jumps at frame `102`; `8 MiB` falls to about `96.7 fps` with `40-41` post-warmup drops | The decisive variable is copy payload size, not route activation alone; because routing is GOP-level, helper-copy pressure arrives in bursts during helper-owned GOPs |
| Dual-camera `100 fps` helper peer-copy `250 us` delay | Worse than no-delay full-copy baseline and crashes during cleanup | sidecars from copydelay250us_probe1 show frame-ID jumps from frame `102`, `59` jumps per camera in the probe window, and terminal output reports `2010096` at `451` drops before EVT socket-error cleanup | A naive per-frame sleep before helper copy does not find a safe window; sustained helper-owned-GOP copy pressure remains the stronger suspect |
| Dual-camera `100 fps` `ptp_gate` recording, `2 ms` stagger, experimental `Continuous` acquisition mode | Offset camera still collapses while the `0 ns` camera stays healthy | `2010095 ≈ 100 fps`, offset `2010096 ≈ 7 fps`, `overflow_events = 0` | Switching from `MultiFrame` to `Continuous` does not by itself fix the `100 fps` offset-camera instability |
| Recabled dual-camera `100 fps` `free_run` real recording via sudo wrapper | Both videos complete near `100 fps`, but one camera reports source-side errors | `2010095` passes with `0` camera drops; `2010096` fails strict policy with `278` `EVT_CameraGetFrame`/camera drops; cadence probe frames `80-160` show no jumps | Recabled topology avoids the earlier throughput collapse, but camera-side source health is still not clean; strict `dropped_frames_camera` policy is necessary and the narrow cadence window can miss late drop bursts |
| Post-reboot recabled dual-camera `100 fps` `free_run` real recording via sudo wrapper | Both videos complete near `100 fps`, but source-side errors recur and shift cameras | `2010095` fails strict policy with `4448` camera drops; `2010096` fails strict policy with `3` camera drops; both produce `253 MiB` MP4s with balanced `702/700` primary/helper routing and no preprocess or encode failures | The recabled topology still avoids the old encode/helper throughput collapse, but source-health failures are not isolated to a single camera; the next telemetry gap is exact drop-event timing beyond the narrow cadence window |
| Split receive-error telemetry | Current builds split true frame-ID gaps from SDK receive errors | `camera_dropped_frames` / `dropped_frames_camera` now mean frame-ID gaps; `get_frame_errors`, `last_get_frame_error_code`, and `get_frame_errors_by_code` capture `EVT_CameraGetFrame` failures such as error `12` (`EVT_ERROR_NOMEM`) | The two recabled rows above were recorded before this split and likely mixed true gaps with SDK buffer-pressure errors; rerun with the split metrics before treating those counts as lost images |
| Recabled dual-camera `100 fps` `free_run` real recording with split receive-error telemetry | Strict policy passes while SDK receive-buffer pressure is visible separately | `2010095`: `0` frame-ID gaps, `126` `GetFrame` errors, all code `12`; `2010096`: `0` frame-ID gaps, `0` `GetFrame` errors; both videos present near `100 fps` | Confirms the prior large drop counts were likely dominated by `EVT_ERROR_NOMEM` receive-buffer pressure, not lost frame IDs; next work should reduce/diagnose buffer pressure without conflating it with frame integrity |
| GPUDirect buffer lifetime code review | Direct pass-through and ring-copy deferred requeue paths store `&ecam->frame_recv` as the SDK frame to return later | `ecam->frame_recv` is a single reusable scratch frame populated by each `EVT_CameraGetFrame`; it can be overwritten before downstream release requeues it | First fix should receive into a stable per-entry descriptor, with `imagePtr -> evt_frame[]` lookup as fallback; then add lease telemetry before increasing SDK buffer counts |
| Recabled dual-camera `100 fps` `free_run` real recording after stable GPUDirect receive/requeue fix | Passes cleanly in headless validation | artifact `2010095_2010096_split_gop_hevc_100fps_real_gpudirect_stable_frame_patch`; both cameras `1001` frames, `0` frame-ID gaps, `0` GetFrame errors, `0` preprocess drops, `0` encode failures | Validated operating point for the recabled A16 topology and headless free-run split-GOP HEVC path; still validate GUI, PTP-gated mode, longer runs, and more than two cameras separately |
| Invalid split-GOP config | GUI shows red validation and blocks stream start | missing helper or overlapping GPU claims are rejected by preflight | Config/policy failure, not runtime throughput failure |
| Headless PTP startup before hardening | Cameras open but local PTP gate never really engages, or host stack is absent | old post-reboot hangs and zero-participant barrier state | Operational setup failure; largely addressed by host-stack preflight/auto-start |

## Current Validated Operating Point

As of commit `951f910` (`fix gpudirect receive buffer requeue`), Orange has a
validated dual-camera `20 MP` `100 fps` recording point in the recabled A16
topology.

Validated scope:

- headless `free_run`
- cameras `2010095` and `2010096`
- real GPUDirect input
- split-GOP HEVC, `gop=25`
- recabled source/helper GPU pairs visible as PIX-local in the run snapshot
- wrapper path:
  `sudo -n /usr/local/bin/orange-local-benchmark --orange-client /home/jeremy/orange-gop-split-a16/targets/release/orange_client <spec>`

Validation artifact:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_real_gpudirect_stable_frame_patch`

Checked-in recabled validation config and dual-camera spec:

- `config/validated_split_gop_hevc_100fps_gop25_recabled_a16/`
- `experiment_specs/2010095_2010096_split_gop_hevc_100fps_real_gpudirect_stable_frame_patch.json`

Observed result:

- `2010095`: `1001` frames, `0` frame-ID gaps, `0` GetFrame errors, `0`
  preprocess drops, `0` encode failures.
- `2010096`: `1001` frames, `0` frame-ID gaps, `0` GetFrame errors, `0`
  preprocess drops, `0` encode failures.

Not yet claimed by this validation:

- GUI recording path
- PTP-gated synchronized recording
- long-duration soak behavior
- more than two cameras

## Detailed Failure Modes

### 1. Split-GOP Backlog Overflow

Observed in:

- dual-camera `100 fps` `free_run` recording

Typical signal:

- terminal logs like:
  - `split_gop-ending GOP backlog exceeded configured limit`
- `recording_snapshot.json` shows:
  - `peak_backlog_gops > max_inflight_gops`
  - `overflow_events > 0`
  - `frontier_present = true`
  - `frontier_complete = false`

Interpretation:

- the pipeline is receiving work fast enough to keep creating later GOPs
- but the oldest GOP needed for ordered output is not completing fast enough
- newer GOPs pile up behind the incomplete frontier GOP

This looks like:

- recording/output path overload
- not a camera-side PTP issue

Representative artifacts:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_rerun2`
- `/home/jeremy/orange_data/exp/unsorted/2026_04_17_12_50_42`

### 2. Synchronized Burst Contention

Observed in:

- dual-camera `80 fps` `ptp_gate` recording with no stagger

Typical signal:

- both cameras degrade together to about `54-57 fps`
- preprocess/encode track the lower acquisition rate
- single-camera `80 fps ptp_gate` is healthy
- dual-camera `80 fps free_run` is healthy

Interpretation:

- tightly aligned PTP-gated arrivals create bursty instantaneous load
- average throughput is not the main limit
- some shared part of the acquisition-to-recording path cannot absorb the
  synchronized bursts cleanly

What this is probably not:

- not a wrong GPU pairing issue
- not a generic PTP misconfiguration
- not a simple average-bandwidth limit

Why we believe this:

- `2 ms` stagger restores healthy `80 fps` dual-camera recording

Representative artifacts:

- failing unstaggered run:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_80fps_gop25_dual_pix_ptp_rerun7`
- healthy staggered run:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_80fps_gop25_dual_pix_ptp_stagger2ms_rerun1`

### 3. PTP-Gated Offset Instability At 100 FPS

Observed in:

- dual-camera `100 fps` `ptp_gate` recording with any nonzero stagger tested

Typical signal:

- run does not show split-GOP backlog overflow
- `overflow_events = 0`
- one camera starts healthy, then falls badly behind
- on the bad camera:
  - `latch_minus_frame_ns` begins near `~9-10 ms`
  - later jumps to multi-second values

Interpretation:

- the camera clock continues advancing
- but the frames reaching the receiver are extremely stale relative to the
  camera's current PTP time
- that suggests buffering or gated-acquisition instability after gate open

New direct evidence from stale-onset receive-history logging:

- threshold-triggered dumps were captured from:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_staleprobe1`
- at the moment `latch_minus_frame_ns` first crossed `50 ms`, the receive-side
  history still showed:
  - contiguous camera frame ids
  - `camera_dropped_frames = 0`
  - `acquisition_resource_starvations = 0`
  - near-full free entry / event pools
  - `direct=1`, `ring_copy=0`
- so the stale onset is happening before the frame enters preprocess/encode and
  before any visible app-side queue starvation or frame-id-gap accounting
  begins

New handoff-side evidence from recording-submit history logging:

- a recording-enabled probe was captured from:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_handoffprobe1`
- when the bad camera first crossed the `50 ms` stale threshold, the recent
  `recording_ingress->SubmitFrame(...)` history still showed:
  - primary-only routing on the bad camera
  - no helper requests or helper dispatches yet
  - no preprocess waits or preprocess drops
  - no encode failures
  - near-full preprocess buffer / event pools
  - only shallow queue depth at the ingress/preprocess boundary
- so the stale onset is now narrowed further:
  - it is not just "before encode"
  - it is also before any obvious recording-side queue growth, helper-routing
    pressure, or preprocess resource exhaustion becomes visible in the app
  - recording still has to be enabled to trigger the bad interaction, but the
    first visible failure happens upstream of the normal recording hot spots

New sink-mode discriminator:

- headless-only experimental sink modes were added:
  - `immediate_recycle`
  - `threaded_handoff_only`
- validated artifacts:
  - `immediate_recycle`:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_immediaterecycle_rerun2`
  - `threaded_handoff_only`:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_threadedhandoff_rerun2`
- both sink runs stayed near `100 fps` on both cameras with:
  - `0` camera drops
  - `0` acquisition starvation
  - no stale-frame onset

That is the strongest current evidence that the bad `100 fps` stagger failure
does not come from:

- bookkeeping alone
- or a simple cross-thread handoff / delayed release

It requires real downstream recording work, not just a lightweight recording
sink.

Important discriminator:

- for larger offsets, the bad behavior follows the offset camera
- swapped `2 ms` order moved the catastrophic failure to the camera with the
  stagger

What this is probably not:

- not the old GOP backlog overflow mode
- not a wrong GPU assignment issue
- not simply "PTP is broken" in all cases

Relevant sweep outcomes:

- `25 us`
- `50 us`
- `100 us`
- `250 us`
- `2 ms`
- swapped `2 ms`

New stream-only discriminator:

- checked-in dual-camera stream-only specs now exist for the same two-camera
  PTP path:
  - `experiment_specs/2010095_2010096_split_gop_hevc_100fps_stream_only_dual_pix_ptp.json`
  - `experiment_specs/2010095_2010096_split_gop_hevc_100fps_stream_only_dual_pix_ptp_stagger2ms.json`
- validated artifacts:
  - no stagger:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_stream_only_dual_pix_ptp`
  - `2 ms` stagger:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_stream_only_dual_pix_ptp_stagger2ms`
- both runs sustained about `100 fps` on both cameras with:
  - `0` camera drops
  - `0` acquisition starvation
  - no `[PTP_STALE_DUMP]` output

That is the strongest current evidence that the catastrophic `100 fps` stagger
failures are not caused by PTP gating or offset acquisition alone. The stale
onset requires recording to be active, even though the stale frames are already
old when they first enter `acquire_frames(...)`.

New acquisition-buffer discriminator:

- forcing acquisition to use Orange-owned ring-buffer copies does not fix the
  bad `100 fps` stagger case
- artifact:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_forceringcopy_rerun1`
- result:
  - `2010095` improved to `enc_fps_mean = 80.2815`
  - `2010096` still collapsed to `enc_fps_mean = 5.35862`
  - stale-frame onset still occurred

Interpretation:

- the direct camera-buffer pass-through path is not the sole cause
- direct buffer lifetime may still contribute, but the remaining failure
  requires more than just GPUDirect pass-through reuse

New preprocess-only discriminator:

- forcing the run through real preprocess workers with no HW encoder/output
  still fails
- artifact:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_preprocessonly_rerun1`
- result:
  - `2010095`: `acq_fps_mean = 90.6936`
  - `2010096`: `acq_fps_mean = 82.2947`
  - stale-frame onset still occurs on the offset camera

Most important timing clue:

- on the bad camera, stale onset happens immediately after helper routing
  begins
- in the stale dump for `2010096`:
  - recording frames `1-100` were still routed primary-only
  - helper routing begins at recording frame `101`
  - stale threshold fires at local frame `106`

Interpretation:

- encode/shared-output work are not required
- helper-path preprocess is now the narrowest remaining trigger

Primary-only preprocess control:

- rerunning the same `preprocess_only` case with
  `fixed.recording.mode = "single_session"` removes helper routing entirely
- artifact:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_preprocessonly_primaryonly_rerun1`
- result:
  - `2010095`: `99.839172 fps`, `0` drops
  - `2010096`: `99.840248 fps`, `0` drops
  - no stale-frame onset

Interpretation:

- real primary preprocess is healthy
- the failure requires helper routing / cross-GPU helper preprocess
- the narrowest confirmed trigger is now the helper preprocess path under
  `100 fps` `ptp_gate` with nonzero stagger

No-stagger control:

- rerunning the same primary-only preprocess control with
  `fixed.ptp_gate_stagger_ns = 0` is also healthy
- artifact:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_hevc_100fps_gop25_dual_pix_ptp_nostagger_preprocessonly_primaryonly_rerun1`
- result:
  - `2010095`: `99.835815 fps`, `0` drops
  - `2010096`: `99.835266 fps`, `0` drops
  - no stale-frame onset

Interpretation:

- the fix is not “stagger-specific”
- helper routing is the decisive variable
- the offset only becomes pathological once the helper cross-GPU preprocess
  path is in play

New helper-host baseline:

- lightweight host-side helper sampling is now available via:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_helperprobe5`
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_ptp_helperprobe5`
- both runs degrade to the same general band:
  - `free_run`: about `69.9-70.0 fps`
  - `ptp_gate`: about `69.3-69.4 fps`
- the first helper-routed frames show a large helper queue-wait spike:
  - `free_run`: about `28.5-28.8 ms`
  - `ptp_gate`: about `33.3-33.6 ms`
- helper worker service itself is tiny:
  - about `0.04-0.07 ms` on the first helper frames
  - mostly `0.01-0.10 ms` after that
- queue wait then decays quickly over the next few helper frames

Interpretation:

- the helper worker is not slow in steady state
- the narrowest observed issue is now a startup backlog when helper routing
  first begins at recording frame `101`
- `ptp_gate` is somewhat worse at onset, but the helper-startup backlog is not
  unique to PTP in this probe

Helper cross-GPU prewarm follow-up:

- helper preprocess workers now prewarm cross-GPU input setup before recording
  starts:
  - enable peer access from source acquisition GPU to helper preprocess GPU
  - allocate helper-side staging memory
  - record and synchronize a lightweight event on the helper stream
- validation artifacts:
  - `free_run`:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_helperprobe6`
  - `ptp_gate`:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_ptp_helperprobe6`
- first helper-routed frame queue wait improved to:
  - `free_run`: about `3.7-3.9 ms`
  - `ptp_gate`: about `4.2 ms`
- both runs still missed target:
  - `free_run`: about `70.3 fps`, `400-401` camera drops
  - `ptp_gate`: about `69.5 fps`, `351` camera drops

Interpretation:

- one-time CUDA/helper setup was contributing to the first helper frame stall
- prewarm removes most of that stall
- the remaining dual-camera `100 fps` failure is not simply helper worker
  startup latency
- the next localization target is the acquisition timing pattern that alternates
  between near-`50 fps` and near-`100 fps` once helper routing is active

Deferred source-release follow-up:

- preprocess now defers raw source entry recycling until queued CUDA source
  reads are complete:
  - helper cross-GPU frames are released after the peer copy into helper staging
  - same-GPU frames are released after the source-safe preprocess event
- validation artifacts:
  - `free_run`:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_helperprobe10`
  - `ptp_gate`:
    `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_ptp_helperprobe11`
- both runs completed with `0` source-release event misses
- sampled source-release waits were small in the normal degraded case:
  - same-GPU primary frames: about `0.4-0.5 ms`
  - helper cross-GPU frames: about `3.2 ms`
- throughput still missed target:
  - `free_run`: about `70.3 fps`, `400-401` camera drops
  - `ptp_gate`: about `69.4-69.6 fps`, `351` camera drops

Interpretation:

- premature raw-source reuse is now guarded against
- normal source-release latency is not large enough to explain the missing
  `100 fps` cadence
- the remaining evidence still points upstream of encode/output, toward camera
  receive cadence and acquisition timing around helper route activation

Acquisition cadence sidecar follow-up:

- Orange now writes:
  - `<recording_folder>/Cam<serial>_acquisition_cadence_probe.csv`
- the sidecar records frames `80-160`, including:
  - `EVT_CameraGetFrame` wait duration
  - host receive delta
  - camera timestamp delta
  - camera frame ID
  - selected primary/helper target GPU
  - receive-to-recording-submit latency
  - acquisition and preprocess resource counters
- first validation artifact:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_cadenceprobe1`
- key result:
  - frame `101` is the first helper route
  - frame `101` still has a normal `~10 ms` camera timestamp delta
  - frame `102` and later switch to `~20 ms` camera timestamp deltas
  - camera frame IDs skip every other frame after that point
  - receive-to-submit remains only a few microseconds

Interpretation:

- the collapse is visible at `EVT_CameraGetFrame` receive time
- `RecordingIngress::SubmitFrame` is not blocking long enough to explain the
  drop
- the next investigation should focus on why activating the helper route causes
  the camera/driver receive cadence to skip every other frame

Helper source-read no-op follow-up:

- headless experiment specs now support:
  - `fixed.helper_noop_source_read = true`
- with that enabled, cross-GPU/helper-routed preprocess workers receive routed
  frames but release them without reading/copying from the source GPU
- primary-routed frames still run through normal preprocess
- validation artifact:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_helpernoop2`
- result:
  - both cameras passed
  - `2010095`: `99.9989 fps`, `0` camera drops
  - `2010096`: `100.001 fps`, `0` camera drops
  - `1200` submitted frames per camera
  - `600` primary-routed frames per camera
  - `600` helper-dispatched frames per camera
  - cadence sidecars keep sequential frame IDs through helper activation

Interpretation:

- split-GOP routing itself is not enough to cause the `50 fps` collapse
- `RecordingIngress::SubmitFrame` cost is not enough to cause the collapse
- helper thread scheduling is not enough to cause the collapse
- the failure is now localized to helper GPU source access:
  peer access, `cudaMemcpyPeerAsync`, or the lifetime/requeue behavior when a
  helper GPU reads an EVT/GPUDirect camera-owned source buffer

Force ring-copy with real helper source-read follow-up:

- validation artifact:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_ringcopy1`
- experiment setting:
  - `fixed.acquisition_buffer_mode = "force_ring_copy"`
  - real helper source-read path enabled
  - `recording_sink_mode = "preprocess_only"`
- result:
  - completed as marginal
  - `2010095`: `70.1342 fps`, `400` post-warmup camera drops
  - `2010096`: `70.1779 fps`, `401` post-warmup camera drops
  - cadence sidecars still show frame IDs skipping every other frame after
    helper activation
  - acquisition counters confirm the ring-copy path, not direct source use

Interpretation:

- moving the source frame into an Orange-owned ring buffer is not enough
- this weakens the "camera-owned GPUDirect buffer lifetime" hypothesis
- the stronger suspect is the helper cross-GPU source read/copy from acquisition
  GPU memory itself, likely via source-GPU/PCIe/RDMA contention or a peer-copy
  interaction

The real helper source-read path remained unstable at `100 fps`.

New helper peer-copy byte sweep:

- experiment setting:
  - `fixed.helper_copy_bytes = 0 | 4096 | 1048576 | 4194304 | 8388608`
  - `recording_sink_mode = "preprocess_only"`
  - `sync_mode = "free_run"`
- validation artifacts:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_copy0probe1`
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_copy4kprobe1`
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_copy1mprobe1`
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_copy4mprobe1`
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_copy8mprobe1`
- result:
  - `0`, `4 KiB`, and `1 MiB` stay healthy at about `100 fps`
  - those healthy cases show no frame-ID jumps in the acquisition cadence
    sidecar
  - `4 MiB` is the first tested copy size that shows the frame-`102`
    `101 -> 103` jump on both cameras
  - `8 MiB` is clearly degraded, with about `96.7 fps` and `40-41`
    post-warmup camera drops

Interpretation:

- the helper route can be active without hurting acquisition cadence
- small peer copies can also remain healthy
- the failure scales with source-to-helper copy payload size
- the current target is source-GPU to helper-GPU copy pressure interfering with
  sustained camera receive, not helper scheduling or app-side submit overhead
  alone

Route-shape clarification:

- split-GOP routing is GOP-level:
  - target GPU is selected from `gop_index % route_gpu_count`
  - `gop_index = (recording_frame_id - 1) / gop_length`
- with `gop_length = 25` and two encoder GPUs, helper routing is not every
  other frame
- it is 25-frame bursts:
  - one 25-frame GOP on one GPU
  - next 25-frame GOP on the other GPU
  - repeat
- the run-level counters still average to about `50 fps` primary and `50 fps`
  helper per camera
- during a helper-owned GOP, however, the source GPU sees helper-copy demand at
  the full camera rate
- for `4512 x 4512 Mono8`, that burst is about `100 * 20 MiB/sec`, or roughly
  `2 GiB/sec` per active camera before considering overhead and topology

This makes the next diagnostic clearer:

- a small helper-copy delay is worth testing because it may move the large
  peer-copy burst away from the camera receive/requeue moment
- if delay helps, the failure is timing/burst contention
- if delay does not help, the problem is more likely sustained contention on
  the source GPU, copy engine, PCIe path, or GPUDirect/RDMA path during the
  helper-owned GOP

First helper-copy delay test:

- setting:
  - `fixed.helper_copy_delay_ns = 250000`
  - full-frame helper copy
  - `recording_sink_mode = "preprocess_only"`
  - `sync_mode = "free_run"`
- artifact:
  - `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_preprocessonly_dual_pix_freerun_copydelay250us_probe1`
- result:
  - no `runs.csv` because the process hit the known EVT socket-error/segfault
    cleanup path
  - cadence sidecars were written and show the failure clearly
  - both cameras start helper routing at frame `101`
  - both cameras start skipping every other frame at frame `102`
  - each camera has `59` frame-ID jumps in the `80-160` cadence probe window
  - terminal output showed `2010096` with `451` camera drops before shutdown

Interpretation:

- a naive per-frame `250 us` delay makes the full-copy case worse, not better
- this weakens the simple "copy starts slightly too early" hypothesis
- the stronger remaining explanation is sustained helper-copy pressure during
  the helper-owned GOP, or a topology/copy-engine interaction that requires a
  more structural scheduling change than a fixed per-frame sleep

One more controlled comparison is now available:

- `2 ms` stagger with experimental `Continuous` gate acquisition mode

That run still failed in the same general way:

- `2010095`: `enc_fps_mean = 100.005`
- `2010096`: `enc_fps_mean = 7.10428`
- `overflow_events = 0`

So the current evidence does not support a simple story of:

- "`MultiFrame + AcquisitionFrameCount=1` is the only thing wrong"

Representative artifacts:

- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger25us_rerun1`
- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger50us_rerun1`
- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger100us_rerun1`
- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger0p25ms_rerun1`
- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_rerun2`
- `/home/jeremy/orange_data/exp/unsorted/2010095_2010096_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_continuous_rerun1`
- `/home/jeremy/orange_data/exp/unsorted/2010096_2010095_split_gop_hevc_100fps_gop25_dual_pix_ptp_stagger2ms_swaporder_rerun1`

### 4. Operational / Setup Failures

Observed earlier in the work:

- host linuxptp stack not running after reboot
- local PTP barrier waiting with zero effective participants
- occasional `GVCP ACK error` on camera open
- invalid split-GOP camera configs

Interpretation:

- these are real failures, but they are different from the performance modes
  above
- the host-stack setup gap is now substantially reduced by headless preflight
  and auto-start support
- invalid split-GOP configs are now caught by shared preflight before stream
  start

Representative areas:

- `src/orange_headless_client.cpp`
- `src/recording_validation.cpp`
- `scripts/ptp_stack.sh`

## What We Have Mostly Ruled Out

### Wrong GPU Assignment

In the core multi-camera tests, the validated source/helper claims are:

- `2010095 -> source 1, helper 2`
- `2010096 -> source 5, helper 6`

These are disjoint and correct for the tested runs.

### Average Streaming Bandwidth Limit

This is the strongest non-cause we currently have.

Why:

- dual-camera `100 fps` `free_run` stream-only works
- dual-camera `100 fps` `ptp_gate` stream-only works

So average dual-camera acquisition bandwidth is not the whole story.

The more likely issue is burstiness and queueing pressure once recording work
is added, especially under tight PTP phase alignment.

### Generic Single-Camera PTP Failure

Single-camera `ptp_gate` is healthy at the rates we tested.

So the problem is specifically multi-camera and rate-sensitive.

## Suspicious But Not Yet Proven Root Cause

The camera-side PTP-gated acquisition programming still looks brittle:

- `TriggerSelector = AcquisitionStart`
- `TriggerSource = Software`
- `TriggerMode = On`
- `AcquisitionMode = MultiFrame`
- `AcquisitionFrameCount = 1`

This clearly works in some regimes:

- single-camera `80 fps ptp_gate`
- dual-camera `60 fps ptp_gate`
- dual-camera `80 fps ptp_gate` with `2 ms` stagger

So it is not simply broken. But it remains a plausible source of fragile
high-rate multi-camera behavior, especially once offsets are introduced.

However, the new `Continuous` comparison means this camera-side mode choice is
probably not the entire explanation on its own.

## Current Best Next Diagnostics

1. Instrument acquisition timing around helper-route activation.
   - The helper warmup reduced first-helper queue wait, but acquisition still
     alternates between near-`50 fps` and near-`100 fps` samples.
   - Capture camera frame deltas, SDK receive gaps, and buffer return timing
     across frames `90-140`.
2. Keep comparing free-run and PTP-gated helper probes.
   - The helper-start behavior is not unique to PTP, but PTP still gives more
     direct frame timing evidence.
3. Keep `80 fps` stagger as the current validated synchronized baseline.
   - Do not treat `100 fps` nonzero stagger as usable yet.
