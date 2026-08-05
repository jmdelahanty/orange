#!/usr/bin/env python3
"""Validate finalized Orange/Citrus MP4 playback and sync-sample metadata."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterable


DEFAULT_FFPROBE = Path("/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe")
PLAYBACK_INTENT_KEY = "com.apple.quicktime.full-frame-rate-playback-intent"
ALLOWED_SCHEMA_IDS = {
    "orange.video_container_finalization",
    "citrus.stimulus_video_container_finalization",
}


@dataclass(frozen=True)
class Box:
    offset: int
    header_size: int
    size: int
    type: bytes

    @property
    def payload_offset(self) -> int:
        return self.offset + self.header_size

    @property
    def end(self) -> int:
        return self.offset + self.size


def read_exact(file: BinaryIO, offset: int, size: int) -> bytes:
    file.seek(offset)
    value = file.read(size)
    if len(value) != size:
        raise ValueError(f"short MP4 read at offset {offset}: {len(value)} != {size}")
    return value


def boxes(file: BinaryIO, begin: int, end: int) -> Iterable[Box]:
    offset = begin
    while offset < end:
        if end - offset < 8:
            raise ValueError(f"truncated MP4 box header at offset {offset}")
        header = read_exact(file, offset, 8)
        compact_size, box_type = struct.unpack(">I4s", header)
        header_size = 8
        size = compact_size
        if compact_size == 1:
            size = struct.unpack(">Q", read_exact(file, offset + 8, 8))[0]
            header_size = 16
        elif compact_size == 0:
            size = end - offset
        if size < header_size or size > end - offset:
            raise ValueError(
                f"invalid MP4 box size {size} for {box_type!r} at offset {offset}"
            )
        yield Box(offset, header_size, size, box_type)
        offset += size


def child(file: BinaryIO, begin: int, end: int, wanted: bytes) -> Box:
    for box in boxes(file, begin, end):
        if box.type == wanted:
            return box
    raise ValueError(f"missing MP4 box {wanted.decode('latin1')}")


def playback_intent_payload(mp4: Path) -> bytes:
    with mp4.open("rb") as file:
        file.seek(0, 2)
        file_size = file.tell()
        moov = child(file, 0, file_size, b"moov")
        udta = child(file, moov.payload_offset, moov.end, b"udta")
        meta = child(file, udta.payload_offset, udta.end, b"meta")
        meta_children = meta.payload_offset + 4  # Full-box version and flags.
        keys = child(file, meta_children, meta.end, b"keys")
        ilst = child(file, meta_children, meta.end, b"ilst")

        keys_header = read_exact(file, keys.payload_offset, 8)
        entry_count = struct.unpack(">I", keys_header[4:8])[0]
        offset = keys.payload_offset + 8
        key_index: int | None = None
        for index in range(1, entry_count + 1):
            entry_header = read_exact(file, offset, 8)
            entry_size, namespace = struct.unpack(">I4s", entry_header)
            if entry_size < 8 or entry_size > keys.end - offset:
                raise ValueError("invalid QuickTime metadata key entry")
            name = read_exact(file, offset + 8, entry_size - 8).decode("utf-8")
            if namespace == b"mdta" and name == PLAYBACK_INTENT_KEY:
                key_index = index
                break
            offset += entry_size
        if key_index is None:
            raise ValueError("missing full-frame-rate playback-intent key")

        item = child(
            file,
            ilst.payload_offset,
            ilst.end,
            struct.pack(">I", key_index),
        )
        data = child(file, item.payload_offset, item.end, b"data")
        if data.size != data.header_size + 9:
            raise ValueError(
                f"playback-intent data payload is {data.size - data.header_size} bytes, expected 9"
            )
        return read_exact(file, data.payload_offset, 9)


CONTAINER_BOXES = {
    b"moov",
    b"trak",
    b"mdia",
    b"minf",
    b"stbl",
    b"edts",
    b"dinf",
    b"mvex",
    b"moof",
    b"traf",
}


def contains_box_type(file: BinaryIO, begin: int, end: int, wanted: bytes) -> bool:
    for box in boxes(file, begin, end):
        if box.type == wanted:
            return True
        if box.type in CONTAINER_BOXES and contains_box_type(
            file, box.payload_offset, box.end, wanted
        ):
            return True
    return False


def has_stss(mp4: Path) -> bool:
    with mp4.open("rb") as file:
        file.seek(0, 2)
        return contains_box_type(file, 0, file.tell(), b"stss")


def run_json(command: list[str]) -> dict:
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or f"exit {result.returncode}"
        raise ValueError(f"ffprobe failed: {detail}")
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise ValueError(f"ffprobe returned invalid JSON: {error}") from error


def ffprobe_format(ffprobe: str, mp4: Path) -> dict:
    return run_json(
        [
            ffprobe,
            "-v",
            "error",
            "-show_entries",
            "format_tags:stream=index,codec_type,r_frame_rate,avg_frame_rate",
            "-of",
            "json",
            str(mp4),
        ]
    )


def sampled_packet_key_flags(ffprobe: str, mp4: Path, limit: int) -> list[bool]:
    probe = run_json(
        [
            ffprobe,
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-read_intervals",
            f"%+#{limit}",
            "-show_entries",
            "packet=flags",
            "-of",
            "json",
            str(mp4),
        ]
    )
    return ["K" in str(packet.get("flags", "")) for packet in probe.get("packets", [])]


def keyframe_sidecar_for(mp4: Path) -> Path | None:
    candidates = (
        mp4.with_name(mp4.stem + "_keyframe.json"),
        mp4.with_name(mp4.stem + "_keyframes.json"),
    )
    return next((candidate for candidate in candidates if candidate.is_file()), None)


def actual_keyframe_prefix(mp4: Path, sample_count: int) -> tuple[int, list[int], Path]:
    sidecar = keyframe_sidecar_for(mp4)
    if sidecar is None:
        raise ValueError(
            "stss is absent and no adjacent Orange keyframe sidecar proves actual IDRs"
        )
    # GOP=1 keyframe sidecars can be large at 700 fps. Read only enough text
    # to prove the prefix corresponding to the bounded packet sample instead
    # of materializing every frame index in memory.
    try:
        with sidecar.open("r", encoding="utf-8") as file:
            text = ""
            while len(text) < 16 * 1024 * 1024:
                chunk = file.read(256 * 1024)
                text += chunk
                total_match = re.search(r'"total_frames"\s*:\s*(\d+)', text)
                keyframes_match = re.search(r'"keyframe_frames"\s*:\s*\[', text)
                if total_match and keyframes_match:
                    suffix = text[keyframes_match.end() :]
                    values = [int(value) for value in re.findall(r"\d+", suffix)]
                    if len(values) > sample_count or "]" in suffix:
                        total_frames = int(total_match.group(1))
                        return total_frames, values[:sample_count], sidecar
                if not chunk:
                    break
    except (OSError, UnicodeError) as error:
        raise ValueError(f"invalid keyframe sidecar {sidecar}: {error}") from error
    raise ValueError(
        f"could not read {sample_count} keyframe indices from {sidecar} within 16 MiB"
    )


def discover(inputs: list[Path], recursive: bool) -> list[Path]:
    videos: set[Path] = set()
    for candidate in inputs:
        if candidate.is_file():
            if candidate.suffix.lower() != ".mp4":
                raise ValueError(f"not an MP4: {candidate}")
            videos.add(candidate.resolve())
        elif candidate.is_dir():
            iterator = candidate.rglob("*.mp4") if recursive else candidate.glob("*.mp4")
            videos.update(path.resolve() for path in iterator if path.is_file())
        else:
            raise ValueError(f"path does not exist: {candidate}")
    return sorted(videos)


def validate_one(mp4: Path, ffprobe: str, packet_limit: int) -> tuple[list[str], str]:
    errors: list[str] = []
    sidecar = Path(str(mp4) + ".finalization.json")
    try:
        document = json.loads(sidecar.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return [f"finalization sidecar unavailable or invalid: {error}"], "unknown"

    if document.get("schema_id") not in ALLOWED_SCHEMA_IDS:
        errors.append(f"unexpected schema_id: {document.get('schema_id')!r}")
    if document.get("schema_version") != 1:
        errors.append(f"unexpected schema_version: {document.get('schema_version')!r}")
    if document.get("status") != "complete" or document.get("terminal") is not True:
        errors.append(
            f"finalization is not terminal-complete: status={document.get('status')!r}"
        )
    try:
        recorded_video_path = Path(str(document.get("video_path", ""))).resolve()
        if recorded_video_path != mp4:
            errors.append(
                f"sidecar video_path does not match: {document.get('video_path')!r}"
            )
        recorded_sidecar_path = Path(str(document.get("sidecar_path", ""))).resolve()
        if recorded_sidecar_path != sidecar.resolve():
            errors.append(
                f"sidecar self path does not match: {document.get('sidecar_path')!r}"
            )
    except (OSError, RuntimeError, ValueError) as error:
        errors.append(f"sidecar paths are invalid: {error}")
    fps = document.get("recording_fps")
    if not isinstance(fps, int) or isinstance(fps, bool) or fps < 1:
        errors.append(f"invalid recording_fps: {fps!r}")

    container = document.get("container", {})
    required_container_truths = (
        "header_written",
        "trailer_attempted",
        "trailer_written",
        "output_close_attempted",
        "output_closed",
        "finalized",
    )
    missing_truths = [name for name in required_container_truths if container.get(name) is not True]
    if missing_truths:
        errors.append("container proof is incomplete: " + ", ".join(missing_truths))
    if container.get("file_size_bytes") != mp4.stat().st_size:
        errors.append(
            "sidecar file size does not match MP4: "
            f"{container.get('file_size_bytes')!r} != {mp4.stat().st_size}"
        )

    intent = document.get("quicktime_full_frame_rate_playback_intent", {})
    if not (
        intent.get("key") == PLAYBACK_INTENT_KEY
        and intent.get("requested_value") == 1
        and intent.get("required_data_type") == "UInt8"
        and intent.get("quicktime_data_atom_type") == 22
        and intent.get("patch_attempted") is True
        and intent.get("patch_applied") is True
        and intent.get("error") is None
    ):
        errors.append("sidecar does not prove a successfully typed playback-intent value")

    try:
        payload = playback_intent_payload(mp4)
        expected = struct.pack(">II", 22, 0) + b"\x01"
        if payload != expected:
            errors.append(f"playback-intent data atom is not UInt8(1): {payload.hex()}")
    except (OSError, UnicodeDecodeError, ValueError) as error:
        errors.append(f"could not independently validate playback-intent atom: {error}")

    try:
        format_probe = ffprobe_format(ffprobe, mp4)
        tags = format_probe.get("format", {}).get("tags", {})
        if str(tags.get(PLAYBACK_INTENT_KEY, "")) != "1":
            errors.append("ffprobe did not recover playback intent value 1")
    except ValueError as error:
        errors.append(str(error))

    sync_description = "unknown"
    try:
        stss_present = has_stss(mp4)
        flags = sampled_packet_key_flags(ffprobe, mp4, packet_limit)
        if not flags:
            sync_description = f"stss={'present' if stss_present else 'absent'}, no packets"
        elif stss_present:
            sync_description = (
                f"stss=present, sampled_key_packets={sum(flags)}/{len(flags)}"
            )
            if not any(flags):
                errors.append("stss is present but no sampled packet is marked key")
        else:
            total_frames, keyframes, keyframe_sidecar = actual_keyframe_prefix(
                mp4, len(flags)
            )
            sync_description = (
                "stss=absent (all-sync declaration), "
                f"sampled_key_packets={sum(flags)}/{len(flags)}, "
                f"actual_idr_sidecar={keyframe_sidecar.name}"
            )
            if not all(flags):
                errors.append(
                    "stss is absent but at least one sampled packet is not marked key"
                )
            if total_frames < len(flags) or keyframes != list(range(len(flags))):
                errors.append(
                    "stss is absent but the Orange keyframe sidecar does not prove "
                    "every sampled packet is an actual IDR"
                )
    except (OSError, ValueError) as error:
        errors.append(f"could not validate sync-sample semantics: {error}")

    return errors, sync_description


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path, help="MP4 file or recording folder")
    parser.add_argument(
        "--recursive",
        action="store_true",
        help="recursively discover MP4 files below folder arguments",
    )
    parser.add_argument(
        "--ffprobe",
        default=(
            str(DEFAULT_FFPROBE)
            if DEFAULT_FFPROBE.exists()
            else shutil.which("ffprobe") or "ffprobe"
        ),
    )
    parser.add_argument(
        "--packet-limit",
        type=int,
        default=4096,
        help="maximum leading video packets to inspect for sync semantics",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.packet_limit < 1:
        print("[FAIL] --packet-limit must be at least 1", file=sys.stderr)
        return 2
    try:
        videos = discover(args.paths, args.recursive)
    except ValueError as error:
        print(f"[FAIL] {error}", file=sys.stderr)
        return 2
    if not videos:
        print("[FAIL] no MP4 files found", file=sys.stderr)
        return 2

    failure_count = 0
    for mp4 in videos:
        errors, sync_description = validate_one(mp4, args.ffprobe, args.packet_limit)
        if errors:
            failure_count += 1
            print(f"[FAIL] {mp4}")
            for error in errors:
                print(f"  - {error}")
        else:
            print(f"[PASS] {mp4}")
        print(f"  sync: {sync_description}")

    print(
        f"Validated {len(videos)} MP4(s): "
        f"{len(videos) - failure_count} passed, {failure_count} failed"
    )
    return 1 if failure_count else 0


if __name__ == "__main__":
    raise SystemExit(main())
