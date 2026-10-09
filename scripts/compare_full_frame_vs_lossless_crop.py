#!/usr/bin/env python3
"""Full-frame bitrate A/B against the lossless crops (2026-10-09).

For a run folder, decode frames of the full-frame recording, cut the crop
window the crop ledger (Cam<serial>_crop_meta.csv) names for that
recording frame, and compare it with the same frame of the lossless crop
clip: PSNR and SSIM per frame, summarized per camera. The lossless crop is
the reference (it was encoded with const QP 0), so the numbers measure what
the full-frame encoder did to the fish region at its bitrate.

Usage: compare_full_frame_vs_lossless_crop.py <run folder> [--every N] [--max-frames M] [--cameras 2010093,...]
Run with the juicebox python (numpy, av/cv2).
"""
import argparse, csv, json, os, sys, glob, math
import numpy as np

def ssim(a, b):
    a = a.astype(np.float64); b = b.astype(np.float64)
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    mu_a, mu_b = a.mean(), b.mean()
    va, vb = a.var(), b.var()
    cov = ((a - mu_a) * (b - mu_b)).mean()
    return ((2 * mu_a * mu_b + c1) * (2 * cov + c2)) / ((mu_a ** 2 + mu_b ** 2 + c1) * (va + vb + c2))

def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)

def open_decoder(path):
    """Decode with the ffmpeg CLI to 8-bit gray raw frames (no PyAV needed)."""
    import subprocess
    probe = subprocess.check_output(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                     "stream=width,height", "-of", "csv=p=0", path]).decode().strip().split(",")
    w, h = int(probe[0]), int(probe[1])
    proc = subprocess.Popen(["ffmpeg", "-v", "error", "-threads", "4", "-i", path, "-f", "rawvideo", "-pix_fmt", "gray", "-"],
                            stdout=subprocess.PIPE, bufsize=w * h * 4)
    def gen():
        while True:
            buf = proc.stdout.read(w * h)
            if len(buf) < w * h: break
            yield np.frombuffer(buf, dtype=np.uint8).reshape(h, w)
        proc.stdout.close(); proc.wait()
    class Closer:
        def close(self):
            if proc.poll() is None:
                proc.kill(); proc.wait()
    return Closer(), gen()

def clip_files(run, serial, kind):
    """kind: 'external' (full frame) or 'crop_external'; returns clips in order with their first frame index."""
    files = sorted(glob.glob(os.path.join(run, "external_recorder", "clips", "clip_*", f"Cam{serial}_{kind}.mp4")))
    if not files:
        f = os.path.join(run, "external_recorder", f"Cam{serial}_{kind}.mp4")
        files = [f] if os.path.exists(f) else []
    return files

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--every", type=int, default=25)
    ap.add_argument("--max-frames", type=int, default=200)
    ap.add_argument("--cameras", default="")
    args = ap.parse_args()
    run = args.run
    serials = args.cameras.split(",") if args.cameras else sorted(
        os.path.basename(p)[3:10] for p in glob.glob(os.path.join(run, "Cam*_crop_meta.csv")))
    summary = {}
    for serial in serials:
        ledger = {}
        with open(os.path.join(run, f"Cam{serial}_crop_meta.csv")) as fh:
            for row in csv.DictReader(fh):
                ledger[int(row["recording_frame_id"])] = row
        full = clip_files(run, serial, "external"); crop = clip_files(run, serial, "crop_external")
        if not full or not crop:
            print(serial, "missing media", file=sys.stderr); continue
        # Concatenate decode across clips; recording_frame_id advances by one per frame (gaps are 0 on clean runs).
        def frames(paths):
            for p in paths:
                c, gen = open_decoder(p)
                for f in gen: yield f
                c.close()
        fid = 0; n = 0; psnrs = []; ssims = []
        for ff, cf in zip(frames(full), frames(crop)):
            fid += 1
            if fid % args.every: continue
            row = ledger.get(fid)
            if not row or row.get("has_detection", "0") != "1": continue
            # crop ledger columns: crop_x, crop_y, crop_w, crop_h (source-frame pixels)
            x, y = int(float(row["crop_x"])), int(float(row["crop_y"]))
            h, w = cf.shape
            win = ff[y:y + h, x:x + w]
            if win.shape != cf.shape: continue
            psnrs.append(psnr(win, cf)); ssims.append(ssim(win, cf)); n += 1
            if n >= args.max_frames: break
        if n:
            summary[serial] = {"frames": n, "psnr_mean": float(np.mean(psnrs)), "psnr_p05": float(np.percentile(psnrs, 5)),
                               "ssim_mean": float(np.mean(ssims)), "ssim_p05": float(np.percentile(ssims, 5))}
    print(json.dumps({"run": run, "cameras": summary}, indent=1))

if __name__ == "__main__":
    main()
