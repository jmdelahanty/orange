# Crop-only runner acceptance — 2026-09-07

Status: runner/profile and camera-free validation implementation. No live run,
wrapper installation, production config change or deployment performed here.
Based on Orange `982e887`, branch `agent/acquisition/master-frame-journal-v1-20260906`.

## Execution boundary

Reuse the existing headless benchmark wrapper and GUI/Citrus local-control
orchestrator. Full-frame profiles remain unchanged. Crop-only selects a distinct
media-validation contract; it does not disable the full-frame checks and declare
success without replacement evidence.

The source wrappers now explicitly allow these additional matching binaries:

- `/tmp/orange-timing-build-20260906/orange_client`
- `/tmp/orange-timing-build-20260906/orange`

There is no wildcard executable allowance for `/tmp`, no new sudoers rule, and no
production-binary replacement. Existing allowlisted binaries remain supported,
including the older fixed-ROI headless isolation path already present in the
installed wrapper. These binaries and their build directory remain trusted local
code; an allowlist is not a binary signature. `/tmp` is not durable deployment.

The installed wrappers do **not** update themselves. The user can install the
reviewed source wrappers when ready:

```bash
cd /tmp/orange-timing-evidence-20260906
sudo bash scripts/install_orange_local_benchmark_wrapper.sh
sudo bash scripts/install_orange_gui_validation_wrapper.sh
```

No `--install-sudoers` is necessary when the existing rules are already present.
These commands change the installed wrappers only, not Orange/Citrus defaults.
Build the helper alongside the matching application if using a different build:

```bash
cmake --build /tmp/orange-timing-build-20260906 --parallel 4 \
  --target orange orange_client external_recorder_ipc_probe verify_crop_only_recording
```

## One-camera headless profiles

`experiment_specs/crop_only_2010093_single_v1.json` is five recording seconds;
`crop_only_2010093_rolling_v1.json` is seven seconds with two-second clips and a
partial tail. Both use camera `2010093`, analytics GPU 3, paired crop GPU 4,
full-rate real detection, existing 384×384 moving/lossless crops and GOP25
interleaving. The configured source profile targets 100 FPS. The matrix's P1/LL
fields describe the unselected full-frame encoder; crops retain HEVC P7/lossless.
Do not interpret those full-frame matrix fields as crop encoder settings.

Templates intentionally omit context and master CPUs: direct execution fails
admission rather than inventing registration acceptance or CPU placement.
Supply an actual enabled `registered_scene_context` JSON object (native capture
or daily-context reuse) and writer CPUs from the rig's housekeeping plan. The
context must cover the selected camera set exactly. A saved four-camera context
is not silently subsetted for this one-camera test.

Prepare a side-effect-free plan, using actual paths and CPU numbers in place of
the examples' placeholders:

```bash
python3 scripts/run_crop_only_headless_validation.py \
  --profile single --context-config /tmp/actual-context-config.json \
  --writer-cpu HOUSEKEEPING_CPU
```

This prints JSON only. Add `--write-spec /tmp/new-single-spec.json` to create a
new inspectable spec; existing files are never overwritten. The runner preserves
the supplied context declaration, adds master configuration, and gives each run
a fresh timestamp/UUID experiment ID. It does not create fake acceptance IDs,
change camera settings, or select a different detector.

When the camera, illumination, accepted geometry and scene are ready, adding
`--execute --confirm-scene` is an explicit real hardware recording. The runner
first uses the application's camera-free spec parser and the installed wrapper's
dry run. Then it invokes the existing `sudo -n orange-local-benchmark` command.
The working directory remains the Orange checkout so the existing headless PTP
preflight can find its script. No new PTP polls/latches or startup policy is added.

Repeat with `--profile rolling`. Keep exact run folders from `runs.json`; never
use newest-folder discovery while another experiment could be writing. Headless
uses its integrated crop collection result validation, including acquisition/FPS
policies. It needs neither Citrus nor a projected stimulus for this first test.

## GUI and Citrus-triggered runs

Use a separate app-config copy with the existing persistent fields:
`recording.media_products` selects `registered_context_and_moving_crops`, and
`recording.registered_context_recording` contains enabled master and actual saved
daily-context reuse configuration. The launcher validates the presence of these
selections; Orange still performs the authoritative closed parsing, registration,
camera/raster, byte-custody and arm checks.

```bash
bash scripts/run_gui_fourcam_external_ipc_validation.sh \
  --crop-only --app-config /tmp/actual-crop-only-app.json \
  --citrus-display-safe --record-seconds 30 --clip-seconds 2
```

The crop-only profile defaults to the matching isolated GUI binary. It streams
but does **not** autorun recording or close after finalization. It permits the
normal local-control start/stop path. Confirm the unchanged scene in Orange
before each recording; no saved flag or runner option fabricates GUI confirmation.
The record duration is a ceiling; choose it longer than the intended Citrus run.
The launcher does not modify the app config. Existing `--validate-only` and
`--print-exec-env-only` remain camera-free preflight modes.

After Orange is streaming and the operator has confirmed the scene, inspect the
orchestrator's default dry run:

```bash
bash scripts/run_orange_citrus_fourcam_orchestrator.sh \
  --crop-only --attach-orange --crop-validator-cpu HOUSEKEEPING_CPU \
  --record-seconds 30 --clip-seconds 2 --citrus-run-seconds 7
```

Add `--execute` only for the live test. Attach mode cannot change an already
running GUI's app settings or clip duration: the duration arguments above are
validation expectations and must match the GUI launch. Use explicit matching
socket/protocol/canvas options for the intended setup. This remains the four-camera
profile; first validate one camera through headless rather than pretending this
profile exercises a one-camera Citrus configuration.

The crop-only orchestrator requires attach mode and its strict media validator;
`--skip-orange-validation` and replacing that validator are refused in this
profile. Existing start/stop ACK, event-log, Citrus output, observation-binding,
source-version, CPU isolation, PTP, YOLO and GUI pacing checks remain. Any
downstream binding failure stays a failure; this profile does not assert new
Palette timing admission or relax a scientific contract to make a run pass.

## Media verification

`verify_crop_only_recording` is a small, read-only command-line adapter to the
same C++ master/context/media/index verifier used by Orange. It is **not** another
recorder or a new camera process. It returns a versioned JSON report to stdout,
requires an explicit housekeeping CPU, and cannot repair missing completion
artifacts or arm a recording.

The existing GUI validator now accepts:

```bash
python3 scripts/validate_gui_ptp_recording.py /exact/recording/folder \
  --media-products registered_context_and_moving_crops \
  --crop-validator-cpu HOUSEKEEPING_CPU \
  --crop-evidence-verifier /tmp/orange-timing-build-20260906/verify_crop_only_recording \
  --expected-cameras 2010093,2010094,2010095,2010096 \
  --json-out /tmp/new-crop-validation.json
```

This substitutes verified per-video collection/master/context evidence for
full-frame and aggregate-v2 crop-video assumptions. It keeps the GUI lifecycle,
stop, telemetry and performance checks. Required camera drop/GetFrame/starvation/
preprocess counters cannot silently disappear. Legitimate blank crops are valid;
missing clips, changed bytes, uncompleted parents and wrong camera sets fail.

The full-frame supervisor status/storage/hello and aggregate crop profile flags
are not applicable to this product and are rejected, not silently ignored.
Crop identity/mux/decode completion remains required through the bound media
receipts. Queue/GPU configuration and performance remain recorded in crop
supervisor/performance artifacts; additional quantitative queue/latency acceptance
limits are a separate live tuning decision, not replaced by full-frame metrics.
Full-frame black-frame bypass flags are likewise invalid here. Exact-folder
selection is mandatory; `--latest` and `--latest-complete` are refused.

The generic GUI launcher still prints its existing full-frame validation examples;
for crop-only use the explicit command above or the crop-only orchestrator profile.

## Validation and remaining live checklist

Camera-free coverage includes the real C++ verifier process over native media
fixtures with daily context, single/rolling outputs, two cameras and missing-index
failure without repair; adapter schema/path/camera failures; valid headless spec
materialization; dry-run side effects; no-execute-without-confirmation; GUI
operator waiting; orchestrator lifecycle retention; and narrow wrapper paths.
Existing full-frame runner, GUI validator and orchestrator regressions are retained.

Verification passed: application/recorder/verifier builds; all 23 selected CTest
suites (including nine runner test groups and the real verifier CLI fixtures);
93 existing camera-free configuration cases; and the existing base/four-camera
GUI launcher, GUI summary, crop-preview/local-control validator, orchestrator
profile and orchestrator-core Python suites. Shell syntax, spec JSON parsing and
`git diff --check` pass. No camera was opened and no PTP service was started.

- [x] Explicit isolation binary paths and dry-run wrapper support in source.
- [x] Single and partial-tail rolling headless templates and safe plan materializer.
- [x] GUI stream-and-confirm profile; crop-only attached orchestrator validation.
- [x] Shared read-only verifier and product-aware GUI media checks.
- [ ] User installs the reviewed wrappers.
- [ ] Confirm cameras/lights/accepted geometry/context/housekeeping CPU assignments.
- [ ] Live headless single, rolling and repeated recordings with real detections.
- [ ] Live GUI/operator and Citrus-triggered single, rolling and consecutive runs.
- [ ] Cancellation/failure injection; dropped-frame and latency/throughput review.
- [ ] Revalidate full-frame-only and full-frame-plus-crops on the rig.

No-fish runs can check blank-frame correspondence and infrastructure, but do not
validate moving-window behavior or positive detection/crop quality.
