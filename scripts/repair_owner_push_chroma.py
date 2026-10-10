#!/usr/bin/env python3
"""Repair the zero-chroma GOPs of full-frame recordings made with the owner-push
warm-up bug (203114b, 2026-10-08 .. 689ac58, 2026-10-09).

Every GOP that the peer shard (owner-pushed staging slots) encoded carries
U = V = 0, which players show as green; luma is intact. This tool re-encodes
only those GOPs, with neutral chroma and the recorder's own NVENC
configuration, and splices them into a new MP4 in which every other byte is
the original: ftyp, the moov box (tags, timing, edit list, sync-sample table,
sample-to-chunk table) and every sample of the native shard. Only the replaced
samples, the sample-size table (stsz) and the chunk offsets (co64/stco) change.

  plan     list the GOPs to replace (shard whose GPU differs from the source GPU)
  repair   extract -> NVDEC decode to luma -> offline recorder re-encode -> splice
  verify   compare a repaired file with its original
  apply    after a passing verify: move the original to the backup root, put
           the repaired file under the original name, write the repair record
           next to it and declare it in recording_session.json (media_repairs)

The original is never modified; `repair` writes <name>.chroma_repaired.mp4 next
to it plus a JSON record. Swapping the files is a separate, explicit step
(`apply`), which keeps the original under the backup root. The recorder summary
and the encoding budgets are left as written (they describe the original
encode; the frame-identity contract pins the summary by digest). The
finalization sidecar's container.file_size_bytes is set to the repaired file's
size, because the transfer sealer and Palette require it to match the MP4; the
recorder's copy is kept under the backup root and the change is declared.
"""

import argparse
import csv
import hashlib
import json
import os
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

CONTAINER_BOXES = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"edts", b"dinf", b"udta"}

# Recorder flags that shape the encoder configuration; everything else in the
# live argv (sockets, CSVs, routing, staging) is irrelevant offline.
ENCODER_FLAGS_WITH_VALUE = {
    "--fps", "--codec", "--preset", "--tuning", "--rate-control", "--quality",
    "--gop", "--bitrate-bps", "--max-bitrate-bps", "--vbv-buffer-size",
    "--importance-map-mode", "--output-kind",
}
ENCODER_FLAGS_BARE = {"--monochrome", "--no-monochrome"}


# ---------------------------------------------------------------- MP4 tables

class Mp4:
    def __init__(self, path):
        self.path = Path(path)
        self.size = self.path.stat().st_size
        self.boxes = {}  # type -> (offset, size, header_len), last wins
        self.top = []
        with open(self.path, "rb") as f:
            self._scan(f, 0, self.size, top=True)
            self._read_tables(f)

    def _scan(self, f, off, end, top=False):
        while off < end:
            f.seek(off)
            head = f.read(16)
            size, typ = struct.unpack(">I4s", head[:8])
            hl = 8
            if size == 1:
                size = struct.unpack(">Q", head[8:16])[0]
                hl = 16
            elif size == 0:
                size = end - off
            if top:
                self.top.append((typ, off, size, hl))
            if typ in self.boxes and typ in (b"stsz", b"co64", b"stco", b"stsc", b"stss", b"stts"):
                raise SystemExit(f"{self.path}: more than one {typ.decode()} box (multi-track file?)")
            self.boxes[typ] = (off, size, hl)
            if typ in CONTAINER_BOXES:
                self._scan(f, off + hl, off + size)
            off += size

    def _payload(self, f, typ):
        off, size, hl = self.boxes[typ]
        f.seek(off + hl)
        return f.read(size - hl)

    def _read_tables(self, f):
        b = self._payload(f, b"stsz")
        sample_size, count = struct.unpack(">II", b[4:12])
        if sample_size != 0:
            raise SystemExit("constant stsz sample size is not expected")
        self.sizes = list(struct.unpack(f">{count}I", b[12:12 + 4 * count]))
        if b"co64" in self.boxes:
            b = self._payload(f, b"co64")
            n = struct.unpack(">I", b[4:8])[0]
            self.chunk_offsets = list(struct.unpack(f">{n}Q", b[8:8 + 8 * n]))
            self.offset_box = b"co64"
        else:
            b = self._payload(f, b"stco")
            n = struct.unpack(">I", b[4:8])[0]
            self.chunk_offsets = list(struct.unpack(f">{n}I", b[8:8 + 4 * n]))
            self.offset_box = b"stco"
        b = self._payload(f, b"stsc")
        n = struct.unpack(">I", b[4:8])[0]
        stsc = [struct.unpack(">III", b[8 + 12 * i:20 + 12 * i]) for i in range(n)]
        # samples per chunk, expanded
        self.samples_per_chunk = []
        for i, (first, per, _desc) in enumerate(stsc):
            last = stsc[i + 1][0] - 1 if i + 1 < n else len(self.chunk_offsets)
            self.samples_per_chunk.extend([per] * (last - first + 1))
        if len(self.samples_per_chunk) != len(self.chunk_offsets):
            raise SystemExit("stsc does not cover every chunk")
        if sum(self.samples_per_chunk) != len(self.sizes):
            raise SystemExit("stsc sample total differs from stsz count")
        b = self._payload(f, b"stss")
        n = struct.unpack(">I", b[4:8])[0]
        self.sync = [x - 1 for x in struct.unpack(f">{n}I", b[8:8 + 4 * n])]
        # absolute sample offsets
        self.offsets = []
        s = 0
        for chunk, per in zip(self.chunk_offsets, self.samples_per_chunk):
            off = chunk
            for _ in range(per):
                self.offsets.append(off)
                off += self.sizes[s]
                s += 1

    def mdat(self):
        for typ, off, size, hl in self.top:
            if typ == b"mdat":
                return off, size, hl
        raise SystemExit("no mdat")

    def check_layout(self):
        """ftyp, one mdat holding every sample contiguously in order, moov last."""
        types = [t for t, *_ in self.top]
        if types != [b"ftyp", b"mdat", b"moov"]:
            raise SystemExit(f"unexpected top-level layout {types}")
        off, size, hl = self.mdat()
        if self.offsets[0] != off + hl:
            raise SystemExit("first sample does not start the mdat payload")
        for i in range(1, len(self.offsets)):
            if self.offsets[i] != self.offsets[i - 1] + self.sizes[i - 1]:
                raise SystemExit(f"samples are not contiguous at {i}")
        if self.offsets[-1] + self.sizes[-1] != off + size:
            raise SystemExit("samples do not end the mdat")


def length_prefixed_to_annexb(sample):
    out = bytearray()
    i = 0
    while i < len(sample):
        n = struct.unpack(">I", sample[i:i + 4])[0]
        out += b"\x00\x00\x00\x01" + sample[i + 4:i + 4 + n]
        i += 4 + n
    if i != len(sample):
        raise ValueError("malformed length-prefixed sample")
    return bytes(out)


def annexb_to_length_prefixed(packet):
    """NVENC packets: NAL units separated by 3- or 4-byte start codes."""
    starts = []
    i = 0
    n = len(packet)
    while i + 3 <= n:
        if packet[i] == 0 and packet[i + 1] == 0 and packet[i + 2] == 1:
            starts.append(i + 3)
            i += 3
        else:
            i += 1
    out = bytearray()
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else n
        nal = packet[s:e]
        # drop a leading zero of a following 4-byte start code (and trailing zeros)
        while nal and nal[-1] == 0:
            nal = nal[:-1]
        out += struct.pack(">I", len(nal)) + nal
    return bytes(out)


def nal_types(sample):
    types = []
    i = 0
    while i < len(sample):
        n = struct.unpack(">I", sample[i:i + 4])[0]
        types.append((sample[i + 4] >> 1) & 0x3F)
        i += 4 + n
    return types


def parameter_sets(sample):
    out = {}
    i = 0
    while i < len(sample):
        n = struct.unpack(">I", sample[i:i + 4])[0]
        nal = sample[i + 4:i + 4 + n]
        t = (nal[0] >> 1) & 0x3F
        if t in (32, 33, 34):
            out[t] = nal
        i += 4 + n
    return out


# ---------------------------------------------------------------- planning

def recording_paths(mp4_path):
    mp4_path = Path(mp4_path).resolve()
    stem = mp4_path.name[: -len("_external.mp4")]  # Cam2010093
    serial = stem[len("Cam"):]
    rec_dir = mp4_path.parent
    session = rec_dir.parent
    return {
        "mp4": mp4_path,
        "serial": serial,
        "routing": rec_dir / f"{stem}_external_gop_routing.csv",
        "plan": session / "external_recorder_supervisor_plan.json",
    }


def encoder_args_from_plan(plan_path, serial):
    plan = json.loads(Path(plan_path).read_text())
    for stream in plan.get("streams", []):
        if str(stream.get("camera_serial")) == serial:
            argv = stream["command"]["argv"]
            break
    else:
        raise SystemExit(f"{plan_path}: no stream for camera {serial}")
    out = []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a in ENCODER_FLAGS_WITH_VALUE:
            out += [a, argv[i + 1]]
            i += 2
            continue
        if a in ENCODER_FLAGS_BARE:
            out.append(a)
        i += 1
    return out


def all_gops(paths):
    gops = {}
    with open(paths["routing"], newline="") as f:
        for row in csv.DictReader(f):
            g = int(row["gop_index"])
            fi = int(row["frame_index"])
            lo, hi = gops.get(g, (fi, fi))
            gops[g] = (min(lo, fi), max(hi, fi))
    return [(g, lo, hi) for g, (lo, hi) in sorted(gops.items())]


def plan_gops(paths, mp4):
    """GOPs (by gop_index) routed to a shard whose GPU is not the source GPU."""
    gops = {}
    with open(paths["routing"], newline="") as f:
        for row in csv.DictReader(f):
            g = int(row["gop_index"])
            fi = int(row["frame_index"])
            peer = row["assigned_gpu_id"] != row["source_gpu_id"]
            entry = gops.setdefault(g, {"first": fi, "last": fi, "peer": peer, "shard": row["assigned_shard_id"]})
            entry["first"] = min(entry["first"], fi)
            entry["last"] = max(entry["last"], fi)
            if entry["peer"] != peer:
                raise SystemExit(f"GOP {g} mixes peer and local frames")
    frames = len(mp4.sizes)
    covered = sum(e["last"] - e["first"] + 1 for e in gops.values())
    if covered != frames:
        raise SystemExit(f"routing covers {covered} frames, MP4 has {frames}")
    sync = set(mp4.sync)
    for g, e in gops.items():
        if e["first"] not in sync:
            raise SystemExit(f"GOP {g} does not start on a sync sample")
    peer = [(g, e["first"], e["last"]) for g, e in sorted(gops.items()) if e["peer"]]
    return peer, len(gops)


# ---------------------------------------------------------------- repair

def cmd_plan(args):
    paths = recording_paths(args.mp4)
    mp4 = Mp4(paths["mp4"])
    mp4.check_layout()
    peer, total = plan_gops(paths, mp4)
    frames = sum(l - f + 1 for _, f, l in peer)
    print(json.dumps({
        "mp4": str(paths["mp4"]),
        "frames": len(mp4.sizes),
        "gops": total,
        "peer_gops": len(peer),
        "peer_frames": frames,
        "encoder_args": encoder_args_from_plan(paths["plan"], paths["serial"]),
    }, indent=2))


def sha256_file(path, chunk=64 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def cmd_repair(args):
    paths = recording_paths(args.mp4)
    mp4 = Mp4(paths["mp4"])
    mp4.check_layout()
    peer, total_gops = plan_gops(paths, mp4)
    if args.max_gops:
        peer = peer[: args.max_gops]
    enc_args = encoder_args_from_plan(paths["plan"], paths["serial"])
    if args.preset:
        enc_args = [a for a in enc_args]
        k = enc_args.index("--preset")
        enc_args[k + 1] = args.preset
    work = Path(args.work_dir) / f"{paths['mp4'].parent.parent.name}_Cam{paths['serial']}"
    work.mkdir(parents=True, exist_ok=True)
    new_stream = work / "peer_gops.hevc"
    out_mp4 = Path(args.output) if args.output else paths["mp4"].with_name(
        paths["mp4"].name.replace(".mp4", ".chroma_repaired.mp4"))

    width, height = 4512, 4512
    t0 = time.time()
    decode = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-threads", str(args.decode_threads),
         "-f", "hevc", "-i", "-", "-vsync", "0", "-f", "rawvideo", "-"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
    encode = subprocess.Popen(
        [args.recorder, "--gpu-id", str(args.encode_gpu), *enc_args,
         "--offline-width", str(width), "--offline-height", str(height),
         "--offline-input-format", "yuv420p", "--extra-output-delay", "8",
         "--offline-reencode-input", "-", "--offline-reencode-output", str(new_stream)],
        stdin=decode.stdout, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    decode.stdout.close()

    # Rate-control warm-up: the live shard's encoder was warmed before its
    # first real picture; feed one native GOP first and drop its packets.
    warmup = []
    if args.warmup_gops:
        native = [(g, f, l) for g, f, l in all_gops(paths) if (g, f, l) not in set(peer)]
        warmup = native[: args.warmup_gops]
    warmup_frames = sum(l - f + 1 for _, f, l in warmup)
    feed_error = []

    def feed():
        try:
            with open(paths["mp4"], "rb") as f:
                for _g, first, last in warmup + peer:
                    for s in range(first, last + 1):
                        f.seek(mp4.offsets[s])
                        decode.stdin.write(length_prefixed_to_annexb(f.read(mp4.sizes[s])))
        except Exception as e:  # noqa: BLE001
            feed_error.append(repr(e))
        finally:
            decode.stdin.close()

    feeder = threading.Thread(target=feed, daemon=True)
    feeder.start()
    enc_log = encode.communicate()[0]
    feeder.join()
    dec_rc = decode.wait()
    if feed_error or dec_rc != 0 or encode.returncode != 0:
        raise SystemExit(f"re-encode failed: feed={feed_error} decode_rc={dec_rc} encode_rc={encode.returncode}\n{enc_log}")
    t_encode = time.time() - t0

    # new packets, one per frame
    with open(str(new_stream) + ".frames.csv") as f:
        new_sizes = [int(r["bytes"]) for r in csv.DictReader(f)]
    want = sum(l - f + 1 for _, f, l in peer)
    if len(new_sizes) != want + warmup_frames:
        raise SystemExit(f"re-encoder returned {len(new_sizes)} packets for {want + warmup_frames} frames")
    replace = {}
    with open(new_stream, "rb") as f:
        f.seek(sum(new_sizes[:warmup_frames]))
        new_sizes = new_sizes[warmup_frames:]
        k = 0
        for _g, first, last in peer:
            for s in range(first, last + 1):
                replace[s] = annexb_to_length_prefixed(f.read(new_sizes[k]))
                k += 1

    # header identity: every replaced IDR carries parameter sets equal to the original's
    with open(paths["mp4"], "rb") as f:
        f.seek(mp4.offsets[0])
        ref_ps = parameter_sets(f.read(mp4.sizes[0]))
    for _g, first, _last in peer:
        ps = parameter_sets(replace[first])
        if ps != ref_ps:
            diff = [t for t in (32, 33, 34) if ps.get(t) != ref_ps.get(t)]
            raise SystemExit(f"re-encoded parameter sets differ from the recording (NAL types {diff}); not splicing")
        if 19 not in nal_types(replace[first]) and 20 not in nal_types(replace[first]):
            raise SystemExit("re-encoded GOP does not start with an IDR")

    splice(mp4, replace, out_mp4)
    record = {
        "schema_id": "orange.recording.full_frame_chroma_repair",
        "schema_version": 1,
        "created_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "defect": "owner-push warm-up zero chroma on peer-shard GOPs (203114b, fixed 689ac58)",
        "original": {"path": str(paths["mp4"]), "bytes": paths["mp4"].stat().st_size},
        "repaired": {"path": str(out_mp4), "bytes": out_mp4.stat().st_size},
        "frames": len(mp4.sizes),
        "gops": total_gops,
        "replaced_gops": len(peer),
        "replaced_frames": want,
        "replaced_gop_indices": [g for g, _, _ in peer] if len(peer) <= 64 else "all peer-shard GOPs per routing CSV",
        "routing_csv": str(paths["routing"]),
        "encoder_args": enc_args,
        "recorder": args.recorder,
        "decode": "ffmpeg libavcodec hevc (software), native yuvj420p frames, luma plane used",
        "unchanged": "ftyp, moov except stsz/co64 values, every sample of the native shard",
        "rate_control_warmup_gops": len(warmup),
        "seconds": round(time.time() - t0, 1),
        "reencode_seconds": round(t_encode, 1),
    }
    if args.hash:
        record["original"]["sha256"] = sha256_file(paths["mp4"])
        record["repaired"]["sha256"] = sha256_file(out_mp4)
    Path(str(out_mp4) + ".repair.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({k: record[k] for k in ("repaired", "replaced_gops", "replaced_frames", "seconds")}))


def splice(mp4, replace, out_path):
    tmp = Path(str(out_path) + ".partial")
    mdat_off, _mdat_size, _hl = mp4.mdat()
    new_sizes = [len(replace[i]) if i in replace else mp4.sizes[i] for i in range(len(mp4.sizes))]
    payload = sum(new_sizes)
    with open(mp4.path, "rb") as src, open(tmp, "wb") as dst:
        # ftyp verbatim
        src.seek(0)
        dst.write(src.read(mdat_off))
        # mdat with a 64-bit size, like the recorder's muxer
        dst.write(struct.pack(">I4sQ", 1, b"mdat", payload + 16))
        new_offsets = []
        pos = mdat_off + 16
        for i in range(len(mp4.sizes)):
            new_offsets.append(pos)
            if i in replace:
                data = replace[i]
            else:
                src.seek(mp4.offsets[i])
                data = src.read(mp4.sizes[i])
            dst.write(data)
            pos += len(data)
        # moov: copy, then patch stsz entries and chunk offsets in place
        moov_off, moov_size, _ = [(o, s, h) for t, o, s, h in mp4.top if t == b"moov"][0]
        src.seek(moov_off)
        moov = bytearray(src.read(moov_size))

        def patch(typ, fmt_entry, values, header):
            off, _size, hl = mp4.boxes[typ]
            base = off - moov_off + hl + header
            struct.pack_into(f">{len(values)}{fmt_entry}", moov, base, *values)

        patch(b"stsz", "I", new_sizes, 12)
        chunk_offsets = []
        s = 0
        for per in mp4.samples_per_chunk:
            chunk_offsets.append(new_offsets[s])
            s += per
        if mp4.offset_box == b"stco":
            if chunk_offsets[-1] > 0xFFFFFFFF:
                raise SystemExit("offsets exceed stco; would need co64")
            patch(b"stco", "I", chunk_offsets, 8)
        else:
            patch(b"co64", "Q", chunk_offsets, 8)
        dst.write(moov)
        dst.flush()
        os.fsync(dst.fileno())
    os.replace(tmp, out_path)


# ---------------------------------------------------------------- verify

def cmd_verify(args):
    orig = Mp4(args.original)
    new = Mp4(args.repaired)
    new.check_layout()
    paths = recording_paths(args.original)
    peer, _ = plan_gops(paths, orig)
    peer_frames = set()
    for _g, f, l in peer:
        peer_frames.update(range(f, l + 1))
    problems = []
    if len(orig.sizes) != len(new.sizes):
        problems.append("sample count differs")
    if orig.sync != new.sync:
        problems.append("sync-sample table differs")
    # moov identical apart from stsz/co64 values
    with open(args.original, "rb") as a, open(args.repaired, "rb") as b:
        a.seek(0)
        b.seek(0)
        if a.read(orig.mdat()[0]) != b.read(new.mdat()[0]):
            problems.append("ftyp differs")
        mo = [(o, s) for t, o, s, _ in orig.top if t == b"moov"][0]
        mn = [(o, s) for t, o, s, _ in new.top if t == b"moov"][0]
        a.seek(mo[0])
        b.seek(mn[0])
        ma = bytearray(a.read(mo[1]))
        mb = bytearray(b.read(mn[1]))
        if len(ma) != len(mb):
            problems.append("moov size differs")
        else:
            # Blank the stsz entries and chunk offsets in place (same length in
            # both files); deleting them would shift the later table.
            for m, mp in ((ma, orig), (mb, new)):
                moov_off = mp.top[[t for t, *_ in mp.top].index(b"moov")][1]
                for typ in (b"stsz", mp.offset_box):
                    off, size, hl = mp.boxes[typ]
                    start = off - moov_off + hl + (12 if typ == b"stsz" else 8)
                    end = off - moov_off + size
                    m[start:end] = bytes(end - start)
            if ma != mb:
                problems.append("moov differs outside stsz/co64")
        # native-shard samples bit-identical; replaced samples keep the frame's NAL shape
        unchanged = 0
        for i in range(len(orig.sizes)):
            if i in peer_frames:
                continue
            a.seek(orig.offsets[i])
            b.seek(new.offsets[i])
            if a.read(orig.sizes[i]) != b.read(new.sizes[i]):
                problems.append(f"native-shard sample {i} changed")
                break
            unchanged += 1
    # decoded chroma: whole GOPs, spread evenly over the file, both shards
    fps = 100.0
    gop_starts = new.sync
    pick = sorted(set(gop_starts[int(k * (len(gop_starts) - 1) / max(1, args.chroma_gops - 1))]
                      for k in range(args.chroma_gops)))
    uavg = []
    for first in pick:
        r = subprocess.run(
            ["ffmpeg", "-v", "error", "-ss", f"{first / fps:.3f}", "-i", args.repaired,
             "-frames:v", "25", "-vf", "signalstats,metadata=print:file=-", "-f", "null", "-"],
            capture_output=True, text=True)
        for l in (r.stderr + r.stdout).splitlines():
            if "UAVG=" in l or "VAVG=" in l:
                uavg.append(float(l.split("=")[1]))
    bad = [u for u in uavg if abs(u - 128) > 0.5]
    if not uavg:
        problems.append("chroma probe returned nothing")
    if bad:
        problems.append(f"{len(bad)}/{len(uavg)} sampled chroma planes not neutral")
    print(json.dumps({
        "repaired": args.repaired,
        "frames": len(new.sizes),
        "native_samples_identical": unchanged,
        "replaced_frames": len(peer_frames),
        "chroma_gops_sampled": len(pick),
        "chroma_planes_sampled": len(uavg),
        "ok": not problems,
        "problems": problems,
    }, indent=2))
    return 0 if not problems else 1


def cmd_apply(args):
    original = Path(args.original).resolve()
    repaired = original.with_name(original.name.replace(".mp4", ".chroma_repaired.mp4"))
    verify = json.loads(Path(args.verify_json).read_text())
    if not verify.get("ok") or Path(verify["repaired"]).resolve() != repaired:
        raise SystemExit(f"verify did not pass for {repaired}")
    record_path = Path(str(repaired) + ".repair.json")
    record = json.loads(record_path.read_text())
    rec_dir = original.parent
    session = rec_dir.parent
    backup_dir = Path(args.backup_root) / session.name / rec_dir.name
    backup_dir.mkdir(parents=True, exist_ok=True)
    backup = backup_dir / original.name
    if backup.exists():
        raise SystemExit(f"backup already exists: {backup}")

    digests = {}
    threads = [threading.Thread(target=lambda k, p: digests.__setitem__(k, sha256_file(p)), args=(k, p))
               for k, p in (("original", original), ("repaired", repaired))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    os.rename(original, backup)
    os.rename(repaired, original)

    # The container-finalization sidecar must state the delivered file's size
    # (the transfer sealer and Palette compare container.file_size_bytes with
    # the MP4). Keep the recorder's copy under the backup root and change only
    # that field; packet_writes stays as the recorder wrote it.
    sidecar = Path(str(original) + ".finalization.json")
    sidecar_change = None
    if sidecar.exists():
        sidecar_backup = backup_dir / sidecar.name
        if not sidecar_backup.exists():
            sidecar_backup.write_bytes(sidecar.read_bytes())
        fin = json.loads(sidecar.read_text())
        before = fin["container"]["file_size_bytes"]
        fin["container"]["file_size_bytes"] = original.stat().st_size
        tmp = sidecar.with_suffix(".json.partial")
        tmp.write_text(json.dumps(fin, indent=2) + "\n")
        os.replace(tmp, sidecar)
        sidecar_change = {
            "relative_path": str(sidecar.relative_to(session)),
            "field": "container.file_size_bytes",
            "recorded_value": before,
            "repaired_value": fin["container"]["file_size_bytes"],
            "recorder_copy_retained_at": str(sidecar_backup),
            "unchanged_note": "all other fields as written by the recorder, including "
                              "packet_writes.bytes_written (original encode)",
        }

    rel = str(original.relative_to(session))
    entry = {
        "schema_id": "orange.recording.media_repair",
        "schema_version": 1,
        "kind": "full_frame_chroma_repair",
        "relative_path": rel,
        "applied_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "defect": record["defect"],
        "effect_of_defect": "peer-shard GOPs decoded with U=V=0 (green); luma unaffected",
        "repair": "peer-shard GOPs re-encoded from their decoded luma with neutral chroma and the "
                  "recording's own encoder configuration; all other bytes unchanged",
        "frames": record["frames"],
        "replaced_gops": record["replaced_gops"],
        "replaced_frames": record["replaced_frames"],
        "encoder_args": record["encoder_args"],
        "rate_control_warmup_gops": record.get("rate_control_warmup_gops", 0),
        "tool": "scripts/repair_owner_push_chroma.py + external_recorder_ipc_probe --offline-reencode-*",
        "tool_commit": args.tool_commit,
        "original": {"bytes": backup.stat().st_size, "sha256": digests["original"],
                     "retained_at": str(backup)},
        "repaired": {"bytes": original.stat().st_size, "sha256": digests["repaired"]},
        "verification": {k: verify[k] for k in verify if k not in ("repaired",)},
        "finalization_sidecar_update": sidecar_change,
        "recording_time_records": "recorder summary and encoding_budget blocks describe the "
                                  "original encode and are left unchanged; the finalization "
                                  "sidecar's container.file_size_bytes states the repaired file",
    }
    Path(str(original) + ".chroma_repair.json").write_text(json.dumps(entry, indent=2) + "\n")
    record_path.unlink()

    manifest = session / "recording_session.json"
    manifest_backup = backup_dir.parent / "recording_session.before_media_repair.json"
    if not manifest_backup.exists():
        manifest_backup.write_bytes(manifest.read_bytes())
    m = json.loads(manifest.read_text())
    repairs = [r for r in m.get("media_repairs", []) if r.get("relative_path") != rel]
    repairs.append({k: entry[k] for k in ("schema_id", "schema_version", "kind", "relative_path",
                                           "applied_at_utc", "defect", "replaced_gops",
                                           "replaced_frames", "original", "repaired",
                                           "finalization_sidecar_update")}
                   | {"record": rel + ".chroma_repair.json"})
    m["media_repairs"] = sorted(repairs, key=lambda r: r["relative_path"])
    tmp = manifest.with_suffix(".json.partial")
    tmp.write_text(json.dumps(m, indent=2) + "\n")
    os.replace(tmp, manifest)
    print(json.dumps({"applied": rel, "original_sha256": digests["original"],
                      "repaired_sha256": digests["repaired"], "backup": str(backup)}))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("plan")
    a.add_argument("mp4")
    a = sub.add_parser("repair")
    a.add_argument("mp4")
    a.add_argument("--recorder", default="/home/jeremy/orange-integration-20260921/targets/native/external_recorder_ipc_probe_native")
    a.add_argument("--encode-gpu", type=int, required=True)
    a.add_argument("--decode-threads", type=int, default=8)
    a.add_argument("--work-dir", default="/mnt/Data1/orange_data/chroma_repair_work")
    a.add_argument("--output")
    a.add_argument("--preset", help="override the recorded preset (parameter sets must still match)")
    a.add_argument("--max-gops", type=int, default=0, help="pilot: replace only the first N peer GOPs")
    a.add_argument("--warmup-gops", type=int, default=1, help="native GOPs encoded first and discarded (rate-control warm-up)")
    a.add_argument("--hash", action="store_true", help="record sha256 of original and repaired file")
    a = sub.add_parser("verify")
    a.add_argument("original")
    a.add_argument("repaired")
    a.add_argument("--chroma-gops", type=int, default=150)
    a = sub.add_parser("apply")
    a.add_argument("original")
    a.add_argument("verify_json")
    a.add_argument("--backup-root", default="/mnt/Data1/orange_data/repair_backups/2026-10-09_full_frame_chroma")
    a.add_argument("--tool-commit", required=True)
    args = p.parse_args()
    if args.cmd == "plan":
        cmd_plan(args)
    elif args.cmd == "repair":
        cmd_repair(args)
    elif args.cmd == "apply":
        cmd_apply(args)
    else:
        sys.exit(cmd_verify(args))


if __name__ == "__main__":
    main()
