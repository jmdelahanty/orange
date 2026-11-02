# How many frames for the empty-dish background median

Date: 2026-11-02. Companion to `empty_background_capture_design.md`, which
uses 17 frames (`median_17`). This note shows how that number was chosen, so
the choice can be checked or revisited with the same method.

## The question

A background is meant to be "the dish without the fish". One frame of the dish
also contains the camera's random noise: every pixel jitters a little from
frame to frame. Taking the per-pixel median of several frames averages that
jitter away. More frames give a cleaner background, but cost memory and capture
time. How many are enough?

## Step 1: measure the noise of one frame

From the lossless crops of the 2026-11-01 250 Hz session (`2026_11_01_14_19_25`,
Cam2010095, first 2500 frames), we used pairs of consecutive frames where the
crop window did not move. We kept only background pixels (more than 12 px from
anything darker than 150) and took the spread of the frame-to-frame difference.
A difference of two frames carries the noise of both, so one frame's noise is
that spread divided by sqrt(2).

Result: **one frame's noise is 4.1 grey levels** (290 pairs, 10th to 90th
percentile 4.0 to 4.3), on a background of about 247.

## Step 2: how the median shrinks it

Averaging N independent noisy samples shrinks the noise by sqrt(N): 4 frames
halve it, 16 frames quarter it. The median does almost as well; its noise is
about 1.25 / sqrt(N) of one frame's. The extra 25% is the price of the
median's robustness: one odd frame (a drifting bubble, a speck of dust, a
missed strobe pulse) cannot pull a median, but it does pull a mean.

That square root is where the diminishing returns come from. Each halving of
the noise needs four times the frames:

- going from 1 to 5 frames removes 1.9 grey levels of noise (4.13 to 2.22);
- going from 17 to 65 frames, 48 more frames, removes only 0.57 more (1.27 to
  0.70). The numbers come from the table in Step 4.

## Step 3: ask what the background is for

The background is subtracted from a live frame: `live - background`. The live
frame brings its own 4.1 grey levels of noise, and no background can remove
that. Noise from independent sources adds as sqrt(a^2 + b^2), so the
difference image's noise is

    sqrt(4.1^2 + background_noise^2)

The live frame's 4.1 is a floor. A background is good enough once its own
noise adds little above that floor.

## Step 4: the table

Simulated with the measured noise (4.12 grey levels, 8-bit rounding and
clipping included, 200,000 pixels per row), and checked against the
1.25 / sqrt(N) formula. Memory is for the burst held in host memory before the
median; time is the burst's length.

| Frames | Background noise (grey levels) | Fraction of one frame | Difference-image noise | Above the 4.12 floor | Memory per camera, 4512 x 4512 | 4512 x 1792 | Time at 100 Hz | at 250 Hz |
|---|---|---|---|---|---|---|---|---|
| 1 | 4.13 | 1.00 | 5.83 | 42% | 20 MB | 8 MB | 10 ms | 4 ms |
| 5 | 2.22 | 0.54 | 4.67 | 13% | 102 MB | 40 MB | 50 ms | 20 ms |
| 9 | 1.70 | 0.41 | 4.47 | 8.6% | 183 MB | 73 MB | 90 ms | 36 ms |
| **17** | **1.27** | **0.31** | **4.33** | **5.2%** | **346 MB** | **137 MB** | **170 ms** | **68 ms** |
| 33 | 0.94 | 0.23 | 4.24 | 2.8% | 672 MB | 267 MB | 330 ms | 132 ms |
| 65 | 0.70 | 0.17 | 4.19 | 1.7% | 1.3 GB | 526 MB | 650 ms | 260 ms |
| 129 | 0.52 | 0.13 | 4.17 | 1.2% | 2.6 GB | 1.0 GB | 1.3 s | 516 ms |

Reading it:

- One frame as a background makes the subtraction 42% noisier than the floor.
- 17 frames bring that to 5%. Doubling to 33 frames halves the remainder to
  3%, for twice the memory. Going on to 129 frames gains another 1.6 points for
  eight times the memory.
- For finding a fish, none of this decides anything: a larva is 100 to 200
  grey levels darker than the background, against a few grey levels of noise.
  The background's quality matters for fainter structure (fins, the edge of
  the tail, shadows), and there the 5% line is a reasonable place to stop.

## Why 17 and not 16

With an odd count, the median is the middle sample, one of the real pixel
values. With an even count it is halfway between the two middle samples,
which needs a rounding rule for 8-bit output. An odd count avoids defining
that rule.

## Observation for the operator, not part of the choice

This session's background sits at about 247 grey levels, close to the 8-bit
ceiling of 255. With 4.1 grey levels of noise, about 3% of background pixels
hit 255 in any single frame. The median handles that, because fewer than half
of a pixel's samples clip. But a pixel at 255 says only "at least 255", so
subtraction near saturation loses information. A background in the low 200s
would leave headroom; that is an iris and exposure question for setup.

## Re-running this for another setup

The inputs are the one-frame noise (Step 1) and the margin wanted above the
floor (Step 3). A camera with more noise, or a different background level,
shifts the absolute numbers but not the fractions in the table: the fraction
column depends only on N.
