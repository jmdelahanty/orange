# Aperture Calibration Usage Guide

Purpose: explain how to interpret Orange aperture calibration artifacts and how
to reuse them across exposures.

Date anchored: 2026-03-17.

## What The Calibration Measures

An Orange aperture characterization run primarily measures:

- `iris -> relative sensor irradiance`
- per-step brightness statistics
- an exposure-specific `saturated / usable / too_dim` classification

If FOV metadata is provided, Orange also derives:

- approximate field width and height
- approximate magnification
- approximate effective reference f-number
- per-step estimated effective f-number

Important distinction:

- The irradiance curve is measured.
- The f-number outputs are inferred.

## What Is Actually Calibrated

After a successful run, you should treat the artifact as containing:

- a measured relative iris curve for this camera/lens/focus state
- an approximate magnification estimate for this setup, if `field_width_mm`
  and/or `field_height_mm` were entered
- an estimated effective f-number curve only if a reference f-number was
  supplied

The current FOV ruler captures are audit evidence for the entered field size.
Orange does not yet auto-read the ruler marks.

## What Transfers Across Exposure Changes

If you later use a different exposure, the following generally still transfer:

- the FOV and magnification estimate
- the relative iris curve
- `relative_mean`
- `delta_ev`
- the ordering of brighter vs dimmer iris steps

The following do not transfer directly:

- `saturated / usable / too_dim` labels
- the exposure-specific usable iris window
- any absolute anchor chosen from a clipped step

Practical interpretation:

- aperture calibration tells you how light changes with iris
- exposure determines where that curve lands on the sensor range

So a later fish-facing exposure can still use the same iris curve even if that
later exposure saturates some images. That does not invalidate the curve; it
only means those particular clipped images are not good for brightness fitting.

## When A Calibration Run Is Good Enough

A run is good enough for relative calibration when:

- focus is the same as the intended operating setup
- gain and pixel format are fixed
- the scene is stable and illumination is stable
- the run completes without iris verification or frame progression errors

A run is good enough for an absolute anchor only when:

- the chosen reference iris is not saturated
- the chosen reference iris is not too dim
- the reference f-number actually corresponds to that iris step

## Should A Run Include Saturated Points?

Sometimes yes, but only for boundary-finding.

Saturated steps are useful for:

- identifying where the bright-end saturation ceiling begins
- defining the upper edge of the exposure-specific usable window

Saturated steps are not good for:

- fitting the brightness curve
- choosing an absolute reference anchor
- deriving trustworthy f-number estimates

Why a higher-exposure run can look more dramatic:

- the open end clips earlier
- clipping compresses the bright region
- later iris changes can appear to produce a larger dynamic shift simply because
  the top of the response curve is flattened

That does not mean the saturated run is better calibrated. It means more of the
bright-end data are no longer quantitatively reliable.

Recommended interpretation:

- use `saturated` points to mark the upper boundary
- use `usable` points for curve fitting and reference anchoring
- use `too_dim` points to mark the lower boundary

Ideal practice:

- keep enough exposure for solid signal at the dim end
- keep the intended reference iris out of saturation
- allow a few bright-end steps to saturate only if you want to record the
  saturation boundary explicitly

## How To Choose A Reference F-Number

Use `reference_f_number` only when you want Orange to derive estimated
f-number-style outputs.

The correct rule is:

- `reference_iris` = the iris step used as the anchor
- `reference_f_number` = the best-known f-number for that anchor step

Recommended practice:

- use a usable, non-saturated reference iris
- if you want to use the lens spec as the anchor, rerun with low enough
  exposure that wide open is not saturated

Do not blindly do this:

- `reference_iris = 0`
- `reference_f_number = 2.8`

unless `iris=0` is actually usable in that run.

If wide open is saturated, the run can still be valuable for relative
calibration, but it is a poor absolute anchor.

## What Orange Computes Today

If a reference f-number is supplied, Orange computes:

- `estimated_f_number`

from the measured brightness ratio:

```text
estimated_f_number =
  reference_f_number * sqrt(reference_mean / step_mean)
```

If FOV calibration metadata is enabled and magnification is available, Orange
also computes:

- `effective_reference_f_number = reference_f_number * (1 + mean_magnification)`
- `estimated_effective_f_number`

These are still estimates, not direct readbacks from the lens.

## Recommended Workflow

For a strong calibration artifact:

1. Keep camera, lens, focus, gain, and pixel format fixed.
2. Use a uniform target.
3. Capture horizontal and vertical ruler references.
4. Enter `field_width_mm` and `field_height_mm`.
5. Lower exposure or illumination until the intended reference iris is
   classified `usable`.
6. Run the full iris sweep.
7. Use the resulting artifact for later operating exposures, but treat the
   usable window as exposure-specific.

## When To Recalibrate

Recalibrate if any of these change materially:

- focus
- lens
- camera
- pixel format
- gain or nonlinear image processing path
- optical geometry or working plane

Changing exposure alone does not require redoing the iris curve, but it may
change which steps are usable for a given task.

## Suggested Language

When describing the current Orange artifact, prefer:

> Orange calibrated iris setting versus relative sensor irradiance for this
> camera/lens/focus state, and optionally derived an estimated effective
> f-number curve from user-supplied FOV and reference-f-number inputs.

Avoid claiming:

> Orange measured the exact geometric f-number of the lens.
