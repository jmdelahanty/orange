#!/usr/bin/env python3
"""scripts/external_video_sanity.py: the one video check, shared by headless and GUI.

Encodes tiny clips with ffmpeg (64x64, 20 frames), so it needs ffmpeg/ffprobe.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SCRIPTS = REPO / "scripts"
sys.path.insert(0, str(SCRIPTS))

import external_video_sanity  # noqa: E402
import validate_gui_ptp_recording  # noqa: E402


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def tool(name: str) -> str:
    default = Path("/opt/orange/lib/ffmpeg-nvidia/bin") / name
    found = str(default) if default.exists() else shutil.which(name)
    require(found is not None, f"{name} not found")
    return str(found)


def encode(path: Path, source: str) -> None:
    subprocess.run(
        [tool("ffmpeg"), "-v", "error", "-y", "-f", "lavfi", "-i", source, "-frames:v", "20",
         "-c:v", "mpeg4", "-pix_fmt", "yuv420p", str(path)],
        check=True,
    )


def run_cli(mp4: Path, out: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPTS / "external_video_sanity.py"), str(mp4), str(out)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )


def main() -> int:
    require(validate_gui_ptp_recording.video_content_sanity is external_video_sanity.video_content_sanity,
            "the GUI validator must use the shared video check, not a copy")
    with tempfile.TemporaryDirectory(prefix="orange_video_sanity_test_") as tmp:
        root = Path(tmp)
        good, black = root / "good.mp4", root / "black.mp4"
        encode(good, "testsrc=size=64x64:rate=10")
        encode(black, "color=c=black:size=64x64:rate=10")

        result = run_cli(good, root / "good.json")
        written = json.loads((root / "good.json").read_text())
        require(result.returncode == 0, f"a normal clip passes: {result.stderr}")
        require(written["schema_version"] == 1 and written["content_checked"] is True
                and written["status"] == "pass" and written["sampled_frame_count"] == 5,
                f"pass result: {written}")
        require({"min", "max", "white_fraction_gt247"} <= set(written["sampled_frames"][0]),
                "per-frame measurements are kept")

        result = run_cli(black, root / "black.json")
        written = json.loads((root / "black.json").read_text())
        require(result.returncode == 1 and written["status"] == "black_frame"
                and written["content_valid"] is False, f"a black clip fails: {written}")

        result = run_cli(root / "missing.mp4", root / "missing.json")
        written = json.loads((root / "missing.json").read_text())
        require(result.returncode == 1 and written["status"] == "missing_video"
                and written["sampled_frames"] == [] and "missing_video" in result.stderr,
                f"a missing clip fails with a written result: {written}")

        direct = external_video_sanity.video_content_sanity(
            good, tool("ffprobe"), tool("ffmpeg"), max_black_fraction=0.98, min_stddev=500.0)
        require(direct["status"] == "flat_frame", "thresholds are the caller's to set")
    print("external_video_sanity_tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
