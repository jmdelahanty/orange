#!/usr/bin/env bash
# Pre-check a recording folder with the transfer sealer in structure-only mode,
# so Orange knows BEFORE the operator's transfer (rsync_move.sh /
# transfer_as_domain_user.sh, profile parent-recording-v2) that the folder will
# seal and that Palette's intake will accept its layout. Read-only: no video
# bytes are read, every other artifact is hashed and video sizes compared.
#
# Pin (2026-10-10): the versioned system install of citrus-recording-transfer
# (repository JohnsonLabJanelia/pancake-plate), /opt/recording-transfer/<version>/,
# installed by Pancake Batter (pancakebatter/recording_transfer_install.py).
# Verified before use: INSTALLED.json names the pinned version and wheel
# sha256; every installed package file matches the wheel's RECORD hashes; the
# install is root-owned and not group/world writable. A mismatch refuses: ask
# Citrus/Pancake Batter before re-pinning.
#   3.1.0 (default): tag recording-transfer-v3.1.0 (456d7fe); admits Citrus
#     receipt v2 citrus_artifacts and Orange's collection revision chain;
#     envelope schema 4865374c (agent-contracts citrus-recording-transfer-3.1).
#   3.0.0 (rollback): tag recording-transfer-v3.0.0 (citrus 334f53a), the
#     wheel rebuilt byte-identical in pancake-plate.
#
# Usage: scripts/check_recording_transfer_source.sh <recording_folder>
#        SEALER_VERSION=3.0.0 to use the rollback pin
#        RECORDING_TRANSFER_ROOT=/opt/recording-transfer (default)
set -euo pipefail
[ $# -eq 1 ] || { sed -n 2,24p "$0"; exit 2; }
FOLDER="$1"
VERSION="${SEALER_VERSION:-3.1.0}"
BASE="${RECORDING_TRANSFER_ROOT:-/opt/recording-transfer}"
declare -A WHEEL_SHA256=(
  ["3.1.0"]="145a9b866a95cf36f98c2165867516fb1ea4f18bf8f3e86059574cfb6f977665"
  ["3.0.0"]="eb12e5df5435eb5ca11907cd5a0ceba9092b17bcc9d17f28a757539fadbdf1c9"
)
[ -n "${WHEEL_SHA256[$VERSION]:-}" ] || { echo "no pin for sealer version $VERSION" >&2; exit 2; }
[ -d "$FOLDER" ] || { echo "not a directory: $FOLDER" >&2; exit 2; }
INSTALL="$BASE/$VERSION"
CLI="$INSTALL/bin/citrus-recording-transfer"
[ -x "$CLI" ] || { echo "sealer $VERSION is not installed at $INSTALL" >&2; exit 2; }

"$INSTALL/bin/python3" -I - "$INSTALL" "$VERSION" "${WHEEL_SHA256[$VERSION]}" <<'PY'
import base64, csv, hashlib, json, stat, sys
from pathlib import Path
install, version, wheel_sha = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
def refuse(msg):
    print(f"sealer install check failed: {msg}; ask Citrus/Pancake Batter before re-pinning", file=sys.stderr)
    sys.exit(2)
meta = json.loads((install / "INSTALLED.json").read_text())
if meta.get("package") != "citrus-recording-transfer" or meta.get("version") != version \
        or meta.get("wheel_sha256") != wheel_sha:
    refuse(f"INSTALLED.json does not record citrus-recording-transfer {version} with wheel {wheel_sha[:12]}")
for path in [install, install / "bin", install / "INSTALLED.json", install / "bin" / "citrus-recording-transfer"]:
    st = path.lstat()
    if st.st_uid != 0 or st.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        refuse(f"{path} is not root-owned or is group/world writable")
dist = list(install.glob("lib/python3*/site-packages/citrus_recording_transfer-*.dist-info"))
if len(dist) != 1 or not dist[0].name.startswith(f"citrus_recording_transfer-{version}."):
    refuse(f"expected one citrus_recording_transfer-{version} dist-info, found {[d.name for d in dist]}")
site = dist[0].parent
checked = 0
with open(dist[0] / "RECORD", newline="") as handle:
    for name, digest, _size in csv.reader(handle):
        if not digest:
            continue  # RECORD itself, and bytecode written at install
        algorithm, _, expected = digest.partition("=")
        if algorithm != "sha256":
            refuse(f"RECORD entry {name} uses {algorithm}")
        target = (site / name).resolve()
        if not str(target).startswith(str(install.resolve())):
            continue  # entries outside the venv are not the sealer's code
        actual = base64.urlsafe_b64encode(hashlib.sha256(target.read_bytes()).digest()).rstrip(b"=").decode()
        if actual != expected:
            refuse(f"installed file {name} differs from the wheel RECORD")
        checked += 1
if checked == 0:
    refuse("RECORD lists no hashed files")
print(f"sealer: citrus-recording-transfer {version} at {install} "
      f"(wheel {wheel_sha[:12]}…, {checked} installed files match the wheel RECORD)")
PY
echo "checking source structure $FOLDER (no video bytes read; no transfer, no writes)"
# Success prints "Source structure admissible: ...".
"$CLI" --source "$FOLDER" --check-structure-only
