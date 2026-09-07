# Encoded crop decoder fixture

`crop_black_five_256_hevc.mp4.b64` contains a synthetic five-frame, 256×256,
100-FPS HEVC/NV12 MP4, encoded with NVENC P1, lossless tuning and GOP 1. It contains
no camera imagery or experiment data. All frames are black deliberately: a valid
blank crop must pass encoded-media validation.

Decoded base64 file SHA-256:
`f1014e9f5b855ec2901f3d3f049ee6d5216d879b3cde24bc5a1b49d0d0a16e97`.

Created on 2026-09-06 with the existing installation:

```sh
/opt/orange/lib/ffmpeg-nvidia/bin/ffmpeg -hide_banner -loglevel error \
  -f lavfi -i color=c=black:s=256x256:r=100 -frames:v 5 \
  -c:v hevc_nvenc -gpu 0 -preset p1 -tune lossless -g 1 -an black-five-256.mp4
```

The tests decode this checked-in fixture using the software HEVC decoder. They
do not need a camera, GPU session or encoder installation at test runtime. The
fixture's GOP 1 does not change any recording defaults and is not a GOP 25/live
performance validation.

`crop_black_two_256_hevc.mp4.b64` is a two-frame stream-copy remux of that file
(`-map 0:v:0 -c copy -frames:v 2`). It exercises a short final rolling clip without
re-encoding or requiring GPU access. Decoded base64 SHA-256:
`502e5e85271c9c41555e6aa201dfa7131a9dc2a25f93a04286deecee55535370`.
