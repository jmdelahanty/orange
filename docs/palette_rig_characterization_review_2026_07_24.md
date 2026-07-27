# Review of Palette Rig Characterization

Date: 2026-07-24
Status: analysis record; no acquisition or encoding policy promoted
Scope: review of the measurements and interpretations reported in Palette's
`rig_characterization_2026-07-24.md`, with consequences for Orange codec and
sensor-characterization work.

The Palette report is a valuable exploratory analysis. It provides strong
evidence for several decoded-domain and metadata findings, but it should not yet
be treated as the authoritative physical characterization of the camera noise
floor or as proof that the current `150 Mbps` P1 master preserves all
scientifically relevant fish detail.

The linked derivation document was not available in this Orange checkout during
this review. The assessment below is based on the supplied characterization
record, its stated methods and commands, arithmetic cross-checks, and Orange's
current source/recording contracts.

## Acquisition Operating Point

The intended current four-camera PTP acquisition profile is `100 fps`, `50 us`
exposure, `Gain = 256`, and `Mono8` for every camera from `2010093` through
`2010096`. On these Emergent cameras, `Gain = 256` is unity digital gain
(`1x`, `0 dB`), not an indication of elevated analog gain.

The checked-in four-camera acquisition profile and the operational
`100_cam4_ptp_fourcam` profile should remain aligned at this operating point.
This policy does not retroactively establish the exposure used by an older
recording: physical conclusions about the July 21 Arena 1 sample must still use
that recording's immutable camera-config snapshot or applied-settings
readback. A historical snapshot that says `100 us` remains `100 us` evidence
even after the current default is corrected to `50 us`.

## Follow-up Review: Gain, Photon Transfer, and Motion Blur

### `Gain = 256` is unity digital gain

The configured Orange value `Gain = 256` does not mean that the camera is
running at elevated analog gain. Emergent defines this node as a digital gain
applied after sensor-to-digital conversion:

```text
linear digital gain = Gain / 256
Gain = 256 / 256 = 1x = 0 dB
```

This is therefore a unity multiplier with no digital amplification. It cannot,
by itself, establish that analog gain is causing the ADC to saturate early.
Consequently, the recommendation to lower analog gain based only on Orange's
configured `Gain = 256` is invalid. See
[Emergent's digital Gain documentation](https://docs.emergentvisiontec.com/camera-features/gain).

The relevant quantities must remain distinct:

| Quantity | Meaning |
| --- | --- |
| Orange `Gain = 256` | Post-conversion digital multiplier; `1x`, `0 dB` |
| `HCG` | Separate high-conversion-gain control that may trade dynamic range for lower read noise |
| `PGAGain` | Separate programmable analog-gain control, if exposed and supported |
| `10.5 e-/DN` | Proposed photon-transfer conversion factor; not the configured camera gain |

Emergent exposes analog controls separately; see its
[analog-control feature overview](https://docs.emergentvisiontec.com/camera-features/analog-control-features)
and [HCG documentation](https://docs.emergentvisiontec.com/camera-features/hcg).
This review has not established whether the HB-20000-SB firmware exposes HCG or
PGAGain for the IMX531, which values were active during the analyzed recording,
or whether Orange currently reads and persists those nodes.

### What remains physically sound

For a linear, shot-noise-limited measurement:

```text
collected photons proportional to illumination * exposure
shot-noise SNR proportional to sqrt(collected photons)
```

Halving illumination and exactly doubling exposure therefore preserves
approximately the same photon count and shot-noise SNR. It does not preserve
motion sharpness: the longer integration time increases motion blur. Retaining
the short `50 us` operating point is consequently desirable for escape
kinematics, provided it supplies adequate signal and does not introduce a
different limiting noise source.

The reported contrast-to-noise arithmetic is also internally consistent:

```text
42.8 DN contrast / 4.279 DN temporal noise = 10.0 CNR
```

Under the same shot-noise-limited assumptions, doubling contrast would produce
the same CNR multiplier as collecting four times as many photons. This makes
illumination geometry, polarization, and oblique or dark-field arrangements
promising experiments. It does not establish that any one arrangement will
deliver that improvement without new reflections, nonuniformity, saturation,
or behavioral effects.

### What two luma bins do not establish

Two bins with similar `sigma / sqrt(mean)` are suggestive of a Poisson-like
component. They do not confirm that the complete system is shot-noise limited,
and they do not justify a conversion-gain result to four-digit precision.

A defensible photon-transfer measurement requires:

1. dark-offset subtraction;
2. multiple controlled illumination or exposure levels;
3. paired flat-field frames at each level;
4. temporal variance calculated from difference images;
5. acquisition at the highest practical camera bit depth;
6. a fitted linear variance-versus-mean interval; and
7. measurements through response saturation and roll-off.

The [EMVA 1288 procedure](https://www.emva.org/wp-content/uploads/EMVA1288-3.1rc2.pdf)
uses maximum available bit depth and recommends substantially more than two
levels; even its reduced production measurement requires at least nine.

The proposed calculation

```text
g ~= 1 / 0.3087^2 ~= 10.5 e-/DN
```

is algebraically valid only under the assumed ideal model. Two bins from one
biological scene cannot resolve black offset, read noise, quantization, spatial
response variation, small scene motion, or HCG state. Even if `10.5 e-/DN`
were later confirmed, multiplying it by the Mono8 numeric range would estimate
the effective signal span of that acquisition mode. It would not prove either
the physical full-well capacity or premature ADC clipping.

The HB-20000-SB product information identifies a Sony IMX531 sensor with
`2.74 um` pixels, Mono8/Mono12 modes, and a specified `74 dB` dynamic range,
but it does not publish a full-well value. See the
[HB-20000-SB specifications](https://www.emergentvisiontec.com/products/bolt-hb-25gige-cameras-rdma-area-scan/hb-20000-sb/).
Substituting a generic `10,000-30,000 e-` scientific-CMOS range is therefore
not evidence about this camera.

Lowering Orange's digital `Gain` below `256`, if attenuation is supported,
would happen after conversion. It would neither distinguish ADC clipping from
physical-well saturation nor create additional electron capacity. A useful
gain experiment must instead inspect and deliberately vary HCG and PGAGain, if
supported, together with ADC/pixel format, exposure or illumination, and
camera offset.

### Motion-blur scale correction

Inferring scale from a `153 px` detection box divided by an assumed `4.4 mm`
fish length produced approximately `35 px/mm`, but that is not a calibrated
camera scale. Shadow's accepted projected-surface scale artifacts report
approximately `52.4-52.75 camera px/mm`. At a representative `52.5 px/mm`,

```text
blur_px = speed_mm_per_s * exposure_s * scale_px_per_mm
```

gives:

| Exposure | Blur at `100 mm/s` | Blur at `500 mm/s` |
| ---: | ---: | ---: |
| `50 us` | `0.26 px` | `1.31 px` |
| `100 us` | `0.53 px` | `2.63 px` |
| `1 ms` | `5.25 px` | `26.25 px` |
| `5 ms` | `26.25 px` | `131.25 px` |

The qualitative conclusion survives: millisecond-scale exposure would be
damaging for fast-motion measurement. The calibrated estimate is about 50%
larger than the original `35 px/mm` table.

At the time of the initial review, a legacy two-camera PTP profile contained a
mixed `50/100 us` setting. The current checked-in four-camera profile and the
operational two- and four-camera PTP profiles have since been aligned to
`50 us`. That configuration correction is prospective. The immutable
configuration snapshot or applied-settings readback from the analyzed Arena 1
recording remains the only valid evidence for its actual exposure.

### Saturation and contrast interpretation

Finding no clipped samples in one lossless crop is useful local evidence: that
fish neighborhood was not wasting signal through Mono8 clipping. It does not
characterize dish-wall reflections, meniscus highlights, the full sensor, or
the other three cameras. A background near DN `208` has some numeric headroom,
but not enough to assume that a fourfold illumination increase can be applied
unchanged.

The strongest practical hypothesis from the analysis is therefore optical
contrast improvement rather than a gain change:

- increase fish/background separation;
- suppress dish-wall and meniscus reflections;
- test crossed polarization;
- test oblique or dark-field IR illumination; and
- quantify silhouette and tail performance across fish orientations.

These must be treated as controlled experiments, including checks for spatial
uniformity, clipping, focus/depth-of-field effects, and animal behavior.

### Required characterization before changing production optics

1. Query and persist `Gain`, `HCG`, `PGAGain`, offset, LUT/gamma state, ADC
   mode, pixel format, exposure, and applied readbacks.
2. Run a characterization capture in Mono12 at its supported lower frame rate.
3. Collect dark and flat-field pairs over controlled exposure or illumination
   levels.
4. Fit the photon-transfer curve and locate response saturation and roll-off.
5. Repeat with HCG disabled and enabled if the node is supported.
6. Separately compare illumination geometries using real fish masks, tail
   keypoints, reflection metrics, and behavioral outcomes.

Implementation status for item 1: Orange now has a getter-only
`orange.camera.sensor_pipeline_state` contract. A standalone EVT probe can
preserve per-camera JSON plus exact GenICam XML evidence without writing any
camera node. Normal Orange startup captures the same inventory after applying
configuration and embeds requested-versus-applied readbacks in each recording
snapshot. The inventory distinguishes ROI offsets from black-level offsets and
output `PixelFormat` from ADC/DualADC controls.

The first live, getter-only four-camera inventory completed on 2026-07-24 for
Cam2010093 through Cam2010096. All four HB-20000SBM cameras reported firmware
`1.0`, EVT SDK `2.55.02`, the same 107,776-byte GenICam XML, and the same XML
SHA-256. Every requested node was either read successfully or reported as
unsupported; there were no read errors.

| Control | Live capability result | Idle value on all four cameras |
| --- | --- | --- |
| `Gain` | readable digital-gain node, range `0-8191` | `256` (`1x`) |
| `PGAGain` | readable node under the vendor's Analog Control category, range `0-240` | `0` |
| `HCG` | not exposed by this firmware XML | unsupported |
| `Offset` | readable vendor offset/black-level node, range `0-1023` | `0` |
| `LUTEnable` | readable | `false` |
| `LUTIndex`, `LUTValue` | readable, range `0-4095` | current index/value `0/0` |
| `GammaEnable`, `Gamma` | not exposed by this firmware XML | unsupported |
| `ADC` | readable enumeration | `Bit8`; options `Bit8`, `Bit10` |
| `DualADC` | readable enable | `false` |
| `PixelFormat` | readable enumeration | `Mono8`; options include `Mono8`, `Mono10`, `Mono10Packed`, `Mono12`, and `Mono12Packed` |

`PGAGain` existing does not by itself identify its physical gain law, units, or
noise/dynamic-range tradeoff; those require a controlled measurement or a
more specific vendor contract. Likewise, the presence of `Bit10` ADC and
Mono12 transport choices does not establish their effective information
content without paired captures.

The standalone probe deliberately records the cameras' current idle state; it
does not apply the Orange profile. At capture time, Cam2010093, Cam2010094, and
Cam2010096 read `10 fps / 1000 us`, while Cam2010095 read
`100 fps / 250 us`. These are evidence of leftover or idle camera state, not
evidence that the intended four-camera recording profile changed. The
authoritative production check is the new `post_configuration_pre_stream`
inventory embedded after Orange applies `100 fps / 50 us`; requested-versus-
applied mismatch is then explicit in the recording snapshot.

Immutable evidence is under
`orange_data/calibrations/rig_characterization/sensor_pipeline/`, timestamped
`20260724_235450`, with one JSON inventory and exact XML file per camera.

The first controlled follow-up is implemented in
`docs/sensor_baseline_characterization.md`. It captures separate operator-
confirmed dark and uniform-field conditions at the unchanged production
Mono8/100 fps/50 us operating point, preserves 24 unaveraged pre-compression
frames per camera, and validates the applied sensor-pipeline readbacks and PTP
timestamps before reporting descriptive noise metrics. It remains a one-level
baseline: no PGA, ADC, DualADC, LUT, or other setting is swept or promoted.

The corrected conclusion is:

> Short exposure is valuable, and optical contrast may be the best practical
> lever. The proposed analog-gain limitation and premature ADC-clipping
> diagnosis are not supported by `Gain = 256` or by the current two-bin
> measurement.

## Findings That Are Already Useful

### Mono and range semantics

- Constant neutral chroma together with signal-bearing luma is strong evidence
  that these HEVC files preserve the intended Mono8 semantics through NV12.
- Reading the decoded luma plane directly, without a range-aware color
  conversion, is the correct validation method.
- Luma samples below `16` and above `235` on both sides of the July 2 metadata
  cutover strongly support the conclusion that older `tv`-tagged files contain
  full-range luma and are mislabeled.
- Consumers that honor the incorrect old range tag may remap pixels and create
  a real cross-era domain difference even though direct decoded luma does not.

### Paired decoded-domain temporal variation

For the stated constant-position crop interval, the reported temporal standard
deviations were:

| Decoded source | Temporal standard deviation |
| --- | ---: |
| Lossless crop | `4.279` gray levels |
| P1 master | `1.502` gray levels |

The ratio is `0.351`. The decoded master therefore has approximately `64.9%`
less temporal standard deviation, or `87.7%` less temporal variance, than the
lossless crop over that sample.

This is credible evidence that inter prediction and quantization suppress small
temporal changes in the decoded master. Calling the resulting behavior
"temporal denoising" is reasonable as an observed effect, but it does not imply
that NVENC ran a distinct denoising algorithm.

The correct narrow conclusion is:

> The paired decoded master and lossless crop are measurably different temporal
> image domains over the sampled static background pixels.

It is not yet a complete physical sensor-noise-floor measurement because the
sample covered only frames `131-144`, less than one `GOP 25`, and one background
selection. The result should be repeated across complete GOPs, I/P-frame
positions, cameras, field positions, and recording conditions.

### Coarse fish signal

The reported broad fish contrast (`p99-p1 = 120` gray levels), large bounding
box (`153x121` pixels), and strongest gradients (`28.6` gray levels/pixel) are
encouraging for body silhouette, detection, and coarse heading.

They do not establish preservation of thin fins, tail curvature, internal
texture, or weak tail edges:

- `p99-p1` emphasizes the broadest intensity separation;
- a `p99.5` gradient emphasizes only the strongest edges; and
- neither statistic describes the lower-gradient boundary regions most likely
  to control fine mask and keypoint error.

## Interpretations That Need Qualification

### "The encoder removes 65% of sensor noise"

The measured reduction applies to decoded temporal standard deviation in the
sampled pixels. It should not be generalized to all sensor noise or to the
moving fish. The lossless value may contain shot noise, read noise, residual
scene/optical motion, and acquisition variation. Codec behavior can also vary
with GOP position and motion.

### "Approximately 2,000 photoelectrons"

The order-of-magnitude estimate follows from a linear, shot-noise-dominated
model similar to `(signal / sigma)^2`. It is plausible but conditional on:

- subtracting the black offset;
- linear, unsaturated response;
- negligible read and electronic noise;
- stable illumination and scene; and
- a genuinely lossless source measurement.

It should be described as an effective shot-noise hypothesis rather than a
measured electron count. Dark frames, flat fields, and a lossless
exposure/illumination ramp are required to separate shot, read, and
fixed-pattern components.

### "Noise is not consuming bitrate"

This is too strong. A codec may discard temporal noise from the reconstruction
while that noise still creates prediction residuals, changes coding decisions,
or consumes some bits. The measurement proves that much of the variation is not
faithfully reconstructed; it does not prove zero bitrate cost.

Similarly, static texture is not encoded only once per recording. With
`GOP 25 @ 100 fps`, an intra frame recurs four times per second. Static content
is predicted efficiently between intra frames, but it is not literally free.

### Illumination is fully ruled out

A whole-frame mean stable to less than one gray level over two seconds is good
evidence against large global flicker during that interval. It does not rule
out:

- slower drift;
- spatial or row-wise banding that cancels in a global mean;
- field-dependent illumination changes; or
- variation suppressed in the lossy master.

The stronger test uses lossless/raw regional means, row profiles, multiple
field positions, and longer intervals.

### Edge displacement is directly measured

The approximation

```text
apparent edge displacement ~= local intensity error / local normal gradient
```

is a useful first-order sensitivity model for a small perturbation measured at
the same boundary. Dividing a fish-region error statistic by the strongest-edge
percentile does not directly measure mask-boundary displacement. The reported
`0.067-0.111 px` values should be labeled optimistic proxies, not observed
boundary errors.

Task-level validation should instead compare masks and keypoints using boundary
F-score, contour distance, Hausdorff tails, IoU, and landmark displacement.

### P1 is proven necessary for acquisition

The A6000 FFmpeg transcode sweep demonstrates the throughput/efficiency tradeoff
for that particular decode-transcode pipeline. It does not directly establish
the limit for Orange's acquisition recorder, which uses:

- direct NVENC SDK configuration;
- external process isolation;
- split-GOP routing across two A16 recorder GPUs; and
- targeted `P3 LL`, rather than an unspecified/default FFmpeg tuning path.

The FFmpeg observation that `-rc vbr -cq N -b:v 0` produced byte-identical files
also applies only to that FFmpeg build and command shape. Orange directly sets
NVENC `targetQuality`, bitrate ceilings, and VBV state and persists the resolved
rate-control configuration.

## Arithmetic and Label Corrections

For `4512x4512 @ 100 fps`:

- `150 Mbps` is `0.07368` bits per source pixel per frame.
- Encoder-ready NV12 is `24.43 Gbps`; the ratio to `150 Mbps` is approximately
  `163:1`.
- Sensor-native Mono8 is `16.29 Gbps`; the ratio to `150 Mbps` is approximately
  `109:1`.
- The distinction matters when describing whether the denominator is the sensor
  stream or the encoder surface.

For crop geometry:

- a `153x121` bounding box occupies approximately `0.091%` of the full frame;
- a `256x256` crop occupies approximately `0.322%`; and
- a `384x384` crop occupies approximately `0.724%`.

The report's `0.32%` value is the `256x256` crop fraction, not the fish fraction.

At `100 fps`, raw NV12 rates are:

- `256x256`: `78.64 Mbps`;
- `384x384`: `176.95 Mbps`.

Consequently, a `32 Mbps` lossless output is `2.46:1` relative to a `256x256`
NV12 input but `5.53:1` relative to a `384x384` NV12 input. Protocol/crop size
must be stated with each ratio.

Finally, `150 Mbps` over `1398.77 s` is approximately `26.23 GB`, not `32.5 GB`.
The reported `32.5 GB` population mean cannot be combined with that reference
duration without reconciling recording durations, achieved bitrate, or counted
artifact duplication.

## Reference-Layer Limitation of the QP Ladder

The constant-QP ladder used the existing lossy P1 master as its reference. It
therefore measures additional transcoding damage relative to an already
compressed and temporally smoothed source.

It cannot establish:

- what P1/150 Mbps already removed from the camera/pre-encoder image;
- whether fine tail features were already lost;
- how a candidate behaves when encoded directly from the acquisition source;
  or
- whether a second-generation transcode predicts direct acquisition quality.

The paired master and lossless crop provide a better local reference for the
scientifically important region. Orange's bounded pre-encoder reference capture
is the preferred full codec-comparison reference when available.

## Model-Domain Interpretation

Palette's offline master-video detector and lossless-crop mask/keypoint stages
may genuinely consume different noise/compression domains. Whether that helps
or harms accuracy depends on the training and deployment domains and must be
measured.

This distinction does not describe live Orange YOLO. For Mono8 cameras,
Orange's live `YoloWorker` consumes the raw camera frame before recording
compression; see `docs/palette_orange_tensor_input_contract.md`. Any statement
that "YOLO sees master pixels" must identify Palette's offline decode path, not
the Orange acquisition path.

## Consequences for Importance Maps

The report's warnings about a dynamic detector-driven QP map are valid:

- encoding must never wait for a detection;
- missed and late detections must not remove protection;
- fast escape frames require explicit scoring; and
- spatial quantization decisions require durable provenance.

Orange's current first slice deliberately avoids the correlated-failure problem:

- the accepted daily dish prior is immutable for the recording;
- a future YOLO box is only an additional quality boost;
- missing/stale YOLO falls back to the dish prior, not to an unprotected or
  uniform frame; and
- the recorder remains independent of detector liveness.

## Recommended Validation

### Codec and fish-detail validation

1. Capture a bounded immutable pre-encoder or lossless paired reference.
2. Encode the exact same frames with P1/P3 VBR and VBR-CQ candidates.
3. Cover multiple complete GOPs and group results by I/P position.
4. Include stationary, ordinary swimming, chase, retreat, and escape epochs.
5. Compare paired luma residuals in fish, near-fish background, and distant
   background regions.
6. Measure gradient retention and temporal spectra, but treat them as image
   diagnostics rather than substitutes for model accuracy.
7. Run the actual detection, mask, and keypoint consumers and report task-level
   differences.
8. Require sustained `100 fps`, zero drops, bounded recorder queues, and
   preserved analytics latency before promoting a setting.

The current live-fish VBR-CQ matrix remains a useful screening step. Its P3
rows compare P1/P3 at matched VBR `150 Mbps` and matched VBR-CQ `20`. Because
the live runs are sequential rather than immutable-source replays, small visual
differences must not be treated as definitive.

### Physical sensor characterization

1. Use lossless/raw frames rather than the denoised master.
2. Capture dark frames and uniform flat fields.
3. Sweep exposure, illumination, and, if applicable, gain without saturation.
4. Measure temporal variance versus black-offset-corrected signal.
5. Repeat across cameras and field positions.
6. Record exposure, gain, illumination, lens/aperture, optical filter, and
   camera operating-point provenance.
7. Test binning only if its implementation is known; true sensor binning and
   post-read digital averaging have different noise behavior.

Increasing photons is a valid lever if shot noise dominates, but brighter
illumination and wider aperture are not free: illumination may affect the
animal or saturation, while aperture changes depth of field and optical
performance.

## Current Decision

- Retain the range/mono findings as actionable consumer-contract evidence.
- Retain the paired temporal-variance mismatch as a high-priority domain-shift
  finding, with its scope stated narrowly.
- Do not yet promote `sigma = 4.279` as the rig-wide physical noise floor.
- Do not use the current transcode ladder to declare `150 Mbps` sufficient or
  insufficient for original fish detail.
- Keep the targeted P3 acquisition screen rather than extrapolating from the
  single-process FFmpeg transcode result.
- Require identical-source, task-level validation before changing the
  production recording profile.
