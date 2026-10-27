# Iris → f-number characterization: run plan (2026-10-27)

Status: plan only. Nothing has run. The run needs Jeremy's go, and never
during a SpecSail run or a recording.

## Goal

Measure, per camera, how image brightness changes with the EF lens `Iris`
count, and from that the f-number at every Iris step. Today's files in
`~/orange_data/calibrations/optics/2026-10-24/` are inferred: the 1/8-stop
model (Iris 24 ≈ f/7.9) and one March sweep on 2010096 (≈ f/6.9), with no
dark subtraction. The other three cameras have no measurement and no Iris
range on record.

What this run gives:
- a measured relative curve per camera (ΔEV per Iris step, from dark-subtracted
  frames);
- an absolute scale, anchored at Iris 0. The manual says Iris 0 is "set iris
  wide open", which is f/2.8 on this lens;
- the Iris range (min/max/inc) of 2010093–2010095, shown in Camera Properties
  at camera open;
- stepper repeatability: an up sweep and a down sweep, and Iris 24 repeated
  through the run.

What it doesn't give: depth of field. That needs a fine target at several
heights (Phase 2 below).

## Why the live stream, not the aperture tool

The aperture tool (GUI "Aperture Characterization" and
`evt_aperture_characterize`) runs one camera on its own acquisition, with
streaming stopped (`src/orange.cpp:3412`). The IR arrays flash on 2010096's
exposure output (GPO mode `Exposure`, `src/camera.cpp:1562`), so a sweep of
2010093–2010095 alone would capture dark frames.

The normal PTP-gated stream lights all four cameras exactly as a recording
does, and the GUI already has what the sweep needs:
- the **Iris** slider in Camera Properties writes while streaming. It is
  locked only during a recording. Each write waits for the lens and verifies
  the read-back (`src/gui/camera_properties_panel.cpp:495`);
- **Save pictures all** writes one full-resolution frame from every camera
  under one timestamp name (`src/orange.cpp:7319`);
- the **Intensity Histogram** panel shows the mean, p99 and clipped % over N
  preview frames, as a live sanity check.

So the run needs no code, no Citrus scene, and no filter change: the R72
stays in, the light is the production 855 nm illumination, and focus is
untouched.

## Conditions

- No SpecSail run and no recording. Send "hold" to citrus-agent,
  citrus-orientation-agent and protocol-builder before, and "done" after.
- No builds or other heavy work on pancake0.
- Citrus idle. The projector stays at its current state (Brightness 50, idle
  frame); write that state down. Room lights in their recording state.
- A diffuse white target at the fish plane in each camera's view, flat and
  fixed for the whole run: matte paper or a PTFE sheet. Uniformity doesn't
  matter; stability does.
- The IR arrays have been on (streaming) for at least 30 minutes before the
  first frame. Brightness rose 4.5 % per 10 min after start on 09-23.
- Focus is never touched.
- Picture save folder:
  `~/orange_data/calibrations/optics/<date>/raw/` (new folder; the 2026-10-24
  files stay as they are).

## Exposure segments

Wide open is about 3 EV brighter than Iris 24, and Iris 56 about 3–4 EV
darker. At production settings (100 µs, gain 256 = 1×, Iris 24 → mean
≈ 180 DN with the tank), no single setting covers 0–56 without clipping or
running into the noise floor. Three segments, with overlaps to stitch them:

| Segment | Exposure | Gain | Iris values (step 4) | Aim |
|---|---|---|---|---|
| S1 bright end | ~12 µs (set so Iris 0 sits at 150–200 DN, clipped < 0.1 %) | 1× | 0 → 32 | the anchor |
| S2 production | 100 µs | 1× | 24 → 56 | production settings |
| S3 dim end | 100 µs | ~4× (set so Iris 56 sits ≥ 60 DN) | 40 → 56 | the dim end with signal |

- S1 shortens the IR pulse, because the strobe follows exposure. The S1/S2
  overlap (24–32) tests whether light scales with pulse length; S2/S3
  (40–56) tests gain linearity. If an overlap ratio drifts across its
  values, that segment is not used for the fit.
- S3 uses gain, not a longer exposure, so the IR duty cycle never exceeds
  production's (1 %).
- The exposure minimum and the gain scale are read at camera open (Camera
  Properties). The exact values are set at the run, not guessed here.

## Procedure

All four cameras step together; each camera's iris is independent.

1. Launch the GUI as usual (your sudo), open cameras, start streaming. Note
   the four Iris ranges and the exposure/gain limits from Camera Properties.
2. Save one test picture and check that it is 4512 × 4512 Mono8.
3. Wait out the 30 minute warm-up.
4. For each segment:
   - set exposure and gain on all four cameras;
   - **darks:** caps on all four lenses, Save pictures all ×3, caps off;
   - **up sweep:** Iris 0 → 32 (S1), 24 → 56 (S2), 40 → 56 (S3), step 4. At
     each value: set Iris on all four (slider release), wait until the GUI
     log shows the verified iris plus 2 s, then Save pictures all ×3;
   - **down sweep** over the same values;
   - Iris 24 (S3: Iris 48) at the start, middle and end, for drift;
   - write the sheet as you go: picture timestamp name, Iris, exposure, gain,
     segment, up/down. The GUI log has the iris writes with times as a
     cross-check.
5. Restore on all four: Iris 24, exposure 100 µs, gain 256. Remove the
   targets. Stop streaming, start again: the exposure check must say `ok`
   (or `ok after re-home`) on every camera before the next fish recording.

Time: about 70 Iris states × 3 saves, at ~1 min per state, plus darks and
warm-up: about **1.5–2 h** for all four cameras together. Disk: about 20 MB
per frame, so ~17 GB on the OS drive (1.2 T free).

## Analysis (offline, after the run)

- Per frame: dark-subtracted mean over a central ROI (the centre 1/4 of the
  field, to keep wide-open vignetting out of the anchor), plus the 8 × 8 grid.
  Frames with any clipped pixel in the ROI are dropped.
- Per segment: ΔEV(Iris) = log2(mean(Iris) / mean(ref)); segments stitched by
  the median overlap ratio.
- Drift: Iris 24 repeats bracket each sweep. Drift above 1 % is linearly
  corrected; above 5 % the segment is repeated.
- Hysteresis: up minus down at each Iris value, reported, never averaged
  away.
- f-number: N(Iris) = 2.8 · 2^(ΔEV₀(Iris)/2), with ΔEV₀ from Iris 0, plus the
  fit N = N0 · 2^(k · Iris). k = 1/16 means exact 1/8 stops. Working f-number
  N(1 + m), m = 0.145.
- Output: `~/orange_data/calibrations/optics/<date>/Cam<serial>_iris_f_number.{json,csv}`,
  schema `orange.optics.iris_f_number_characterization` v1, method
  `photometric_relative` anchored at Iris 0. It carries every source file's
  sha256, and measured and inferred values are never mixed. rig-designer
  reads it.
- The analysis script is offline Python (juicebox env) and can be written and
  tested on the March frames before the run.

## Limits

- The Iris 0 anchor assumes Iris 0 is the true f/2.8 wide-open stop. If the
  stitched curve shows a kink at 0–4 (the stepper's initialise move), the
  anchor moves to the DOF target (Phase 2).
- This measures the photometric f-number (transmission included), not the
  geometric one. For diffraction they differ by the lens's transmission loss,
  usually a few %.
- At f/8 and 855 nm the system is diffraction-limited (Airy ≈ 7 px), so the
  f-number at Iris 24 matters for resolution, not just brightness.

## Phase 2 (separate go): depth of field

Needs the Edmund DOF 5-15 target (#54-440) or a USAF target at the fish plane,
at several heights. Procedure as sent to rig-designer on 2026-10-24: Iris 8,
16, 24, 32, 40, 48; 16 frames averaged per setting; contrast vs depth per
lp/mm group, blur vs defocus, and in-focus MTF against the diffraction limit.
Phase 1's curve sets the exposure compensation per Iris value. The same live
stream method applies.

## Rollback

If anything goes wrong mid-run (lens busy, an iris read-back mismatch, a
camera dropping out): stop, set Iris 24, exposure 100, gain 256 on every
camera, stop and start streaming, and check the exposure check. A lens that
won't settle gets Iris 0 then 24 (re-home). A camera answering "GVCP ACK
error" after a crash needs `evt_force_reboot`.
