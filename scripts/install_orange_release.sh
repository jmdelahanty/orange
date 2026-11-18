#!/usr/bin/env bash
# Install a staged Orange release under /opt/orange (plan
# docs/opt_install_plan_2026-11-14.md, step d). Jeremy runs it with sudo.
#
#   sudo scripts/install_orange_release.sh --release-dir targets/releases/<tag> [--link-recorder]
#   sudo scripts/install_orange_release.sh --rollback      # swap current and previous
#   scripts/install_orange_release.sh --list               # no sudo needed
#   scripts/install_orange_release.sh --verify [<tag>]     # check SHA256SUMS (default: current)
#
# Install, from a folder made by scripts/build_orange_release.sh:
#   - refuses while an orange, orange_client or recorder process runs (send
#     HOLD to the other sessions first), when the staged SHA256SUMS do not
#     match, when the local tag does not name the staged commit, or when the
#     release is already installed (releases are immutable; a fix is a new tag);
#   - copies it to <prefix>/releases/<tag>.new, fills installed_at/installed_by
#     in INSTALLED.json, rewrites SHA256SUMS over every file, makes it
#     root:root and read-only, renames it to <prefix>/releases/<tag>;
#   - points `previous` at the old `current` and flips `current` to the new
#     release (each link replaced by an atomic rename);
#   - --link-recorder: makes <prefix>/bin/external_recorder_ipc_probe_native
#     (the path the app config and specs name) a link to
#     current/bin/external_recorder_ipc_probe_native, keeping a regular file
#     found there as <name>.pre-release.<time>. Every Orange, dev tree
#     included, then records with the installed release's recorder.
# Nothing is deleted. Prints the summary to send to the other sessions.
#
# --prefix <dir> and --no-root are for tests: install into a scratch folder
# as the current user (ownership left as is).
set -euo pipefail

PREFIX="/opt/orange"
ACTION=""
RELEASE_DIR=""
VERIFY_TAG=""
LINK_RECORDER=0
NO_ROOT=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --release-dir) ACTION=install; RELEASE_DIR="${2:?--release-dir needs a path}"; shift 2 ;;
    --rollback) ACTION=rollback; shift ;;
    --list) ACTION=list; shift ;;
    --verify) ACTION=verify; if [[ $# -gt 1 && "$2" != --* ]]; then VERIFY_TAG="$2"; shift; fi; shift ;;
    --link-recorder) LINK_RECORDER=1; shift ;;
    --prefix) PREFIX="${2:?--prefix needs a path}"; shift 2 ;;
    --no-root) NO_ROOT=1; shift ;;
    -h|--help) sed -n 2,29p "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$ACTION" ]] || { sed -n 2,29p "$0" >&2; exit 2; }

die() { echo "[install-release] $*" >&2; exit 1; }
RELEASES="$PREFIX/releases"

need_root() {
  if [[ "$NO_ROOT" == 1 ]]; then return; fi
  [[ "${EUID}" -eq 0 ]] || die "run with sudo (or --no-root with a test --prefix)"
}

refuse_if_recording() {
  local ancestors=" " pid=$$ match
  while [[ "$pid" -gt 1 ]]; do
    ancestors+="$pid "
    pid="$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ')"
    [[ -n "$pid" ]] || break
  done
  for match in $(pgrep -f "bin/orange_client|bin/orange( |$)|targets/release/orange_client|targets/release/orange( |$)|external_recorder_ipc_probe" 2>/dev/null); do
    [[ "$ancestors" == *" $match "* ]] && continue
    die "an Orange recording process is running (pid $match); install later"
  done
}

link_target() { [[ -L "$1" ]] && readlink "$1" || true; }

# Replace link $1 so it points at $2, atomically.
set_link() {
  local tmp
  tmp="$(dirname "$1")/.$(basename "$1").new.$$"
  ln -sfn "$2" "$tmp"
  mv -T "$tmp" "$1"
}

summary() {
  local tag="$1" dir="$RELEASES/$1"
  local commit
  commit="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["source"]["commit"])' "$dir/INSTALLED.json")"
  echo "[install-release] current=$(link_target "$PREFIX/current") previous=$(link_target "$PREFIX/previous")"
  echo "[install-release] summary for the other sessions:"
  echo "  Orange release $tag installed at $dir ($(date -u +%Y-%m-%dT%H:%M:%SZ))"
  echo "  commit $commit"
  echo "  orange        sha256 $(sha256sum "$dir/bin/orange" | cut -d' ' -f1)"
  echo "  orange_client sha256 $(sha256sum "$dir/bin/orange_client" | cut -d' ' -f1)"
  echo "  recorder      sha256 $(sha256sum "$dir/bin/external_recorder_ipc_probe_native" | cut -d' ' -f1)"
}

case "$ACTION" in
  list)
    cur="$(basename "$(link_target "$PREFIX/current")" 2>/dev/null || true)"
    prev="$(basename "$(link_target "$PREFIX/previous")" 2>/dev/null || true)"
    [[ -d "$RELEASES" ]] || { echo "no releases under $RELEASES"; exit 0; }
    for d in "$RELEASES"/*/; do
      [[ -d "$d" ]] || continue
      t="$(basename "$d")"
      [[ "$t" == *.new ]] && continue
      mark="  "
      [[ "$t" == "$cur" ]] && mark="* "
      [[ "$t" == "$prev" ]] && mark="- "
      info="$(python3 -c 'import json,sys;d=json.load(open(sys.argv[1]));print(d["source"]["commit"][:10], d.get("installed_at"), d.get("installed_by"))' "$d/INSTALLED.json" 2>/dev/null || echo "INSTALLED.json unreadable")"
      echo "$mark$t  $info"
    done
    echo "(* current, - previous)"
    exit 0
    ;;
  verify)
    t="${VERIFY_TAG:-$(basename "$(link_target "$PREFIX/current")")}"
    [[ -n "$t" && -d "$RELEASES/$t" ]] || die "no release to verify: ${t:-<none>}"
    ( cd "$RELEASES/$t" && sha256sum --quiet -c SHA256SUMS ) || die "$t: SHA256SUMS mismatch"
    # Anything SHA256SUMS does not list (a __pycache__, an imgui.ini) was
    # written into the release after the install.
    extra="$(cd "$RELEASES/$t" && LC_ALL=C comm -23 \
      <(find . -mindepth 1 \( -type f -o -type l \) -printf '%P\n' | grep -vxE 'INSTALLED.json|SHA256SUMS' | LC_ALL=C sort) \
      <(sed -E 's/^[0-9a-f]{64}  //' SHA256SUMS | LC_ALL=C sort))"
    [[ -z "$extra" ]] || { echo "$extra" | sed 's/^/  not in SHA256SUMS: /' >&2; die "$t: files were added after the install"; }
    echo "[install-release] $t: every file matches SHA256SUMS and nothing was added"
    exit 0
    ;;
  rollback)
    need_root
    refuse_if_recording
    cur="$(link_target "$PREFIX/current")"
    prev="$(link_target "$PREFIX/previous")"
    [[ -n "$cur" && -n "$prev" ]] || die "rollback needs both current and previous links"
    [[ -d "$PREFIX/$prev" || -d "$prev" ]] || die "previous release missing: $prev"
    set_link "$PREFIX/current" "$prev"
    set_link "$PREFIX/previous" "$cur"
    echo "[install-release] rolled back: current=$prev previous=$cur"
    summary "$(basename "$prev")"
    exit 0
    ;;
esac

# --- install --------------------------------------------------------------------
need_root
refuse_if_recording
SRC="$(realpath -e "$RELEASE_DIR")" || die "release dir not found: $RELEASE_DIR"
TAG="$(basename "$SRC")"
[[ "$TAG" =~ ^orange-v[0-9]{4}\.[0-9]{2}\.[0-9]{2}(\.[0-9]+)?$ ]] || die "not a release folder name: $TAG"
[[ -f "$SRC/INSTALLED.json" && -f "$SRC/SHA256SUMS" ]] || die "$SRC is not a staged release (INSTALLED.json, SHA256SUMS)"
( cd "$SRC" && sha256sum --quiet -c SHA256SUMS ) || die "staged files do not match their SHA256SUMS"
for b in orange orange_client external_recorder_ipc_probe_native; do
  [[ -x "$SRC/bin/$b" ]] || die "missing bin/$b"
done
ON_FORK="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["source"].get("on_fork"))' "$SRC/INSTALLED.json")"
[[ "$ON_FORK" == True || "$NO_ROOT" == 1 ]] || die "$TAG was built from a commit not on the fork (test release); refusing"
STAGED_COMMIT="$(python3 -c 'import json,sys;d=json.load(open(sys.argv[1]));assert d["release"]==sys.argv[2];print(d["source"]["commit"])' "$SRC/INSTALLED.json" "$TAG")" \
  || die "INSTALLED.json does not describe release $TAG"
# The local tag (made by the build script) must name the staged commit. Git runs
# as the invoking user: root may not read the developer's checkout config.
REPO="$(realpath -m "$SRC/../../..")"
GIT=(git -C "$REPO")
if [[ "${EUID}" -eq 0 && -n "${SUDO_USER:-}" ]]; then GIT=(sudo -u "$SUDO_USER" git -C "$REPO"); fi
TAG_COMMIT="$("${GIT[@]}" rev-parse -q --verify "refs/tags/$TAG^{commit}" 2>/dev/null || true)"
[[ "$TAG_COMMIT" == "$STAGED_COMMIT" ]] || die "tag $TAG names '${TAG_COMMIT:-nothing}', staged commit is $STAGED_COMMIT"
DEST="$RELEASES/$TAG"
[[ ! -e "$DEST" ]] || die "$TAG is already installed (releases are immutable; build a new tag)"

mkdir -p "$RELEASES"
NEW="$DEST.new"
rm -rf "$NEW"
cp -a "$SRC" "$NEW"
INSTALLED_BY="${SUDO_USER:-$(id -un)}"
python3 - "$NEW/INSTALLED.json" "$INSTALLED_BY" <<'EOF'
import json, sys, datetime
path, who = sys.argv[1], sys.argv[2]
doc = json.load(open(path))
doc["installed_at"] = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
doc["installed_by"] = who
open(path, "w").write(json.dumps(doc, indent=2, sort_keys=True) + "\n")
EOF
( cd "$NEW" && find . -type f ! -name SHA256SUMS -printf '%P\n' | LC_ALL=C sort \
    | xargs -d '\n' sha256sum > SHA256SUMS )
if [[ "$NO_ROOT" != 1 ]]; then chown -R root:root "$NEW"; fi
chmod -R a-w,go-w "$NEW"
mv -T "$NEW" "$DEST"

OLD_CURRENT="$(link_target "$PREFIX/current")"
if [[ -n "$OLD_CURRENT" ]]; then set_link "$PREFIX/previous" "$OLD_CURRENT"; fi
set_link "$PREFIX/current" "releases/$TAG"

if [[ "$LINK_RECORDER" == 1 ]]; then
  mkdir -p "$PREFIX/bin"
  REC="$PREFIX/bin/external_recorder_ipc_probe_native"
  if [[ -e "$REC" && ! -L "$REC" ]]; then
    mv "$REC" "$REC.pre-release.$(date -u +%Y%m%dT%H%M%SZ)"
  fi
  set_link "$REC" "../current/bin/external_recorder_ipc_probe_native"
  echo "[install-release] $REC -> ../current/bin/external_recorder_ipc_probe_native"
fi

( cd "$DEST" && sha256sum --quiet -c SHA256SUMS ) || die "installed files do not match SHA256SUMS"
echo "[install-release] installed $DEST"
summary "$TAG"
