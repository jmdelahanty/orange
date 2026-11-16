#!/usr/bin/env bash
# Verify Orange's MetaZebrobot consumer expectations against the owner's pin.
#
# Fetches metazebrobot-consumers/{consumers.json,verify_consumers.py} from
# agent-contracts at a pinned commit (PR 140 merge by default) and runs the
# verifier for the orange consumer against the pinned consumer_openapi.json.
# With --live <base_url> it also checks the running service (/version digest
# and /openapi.json). Stdlib Python only; needs gh auth (or GITHUB_TOKEN) and network.
#
# Usage: scripts/check_metazebrobot_consumer_pin.sh [--live http://host[:port]] [--contracts-commit <sha>]
set -euo pipefail

CONTRACTS_REPO="jmdelahanty/agent-contracts"
CONTRACTS_COMMIT="bfdae22898e84678fd3b553ef9e3b1f1d71a43b8"   # agent-contracts PR 140 merge, 2026-11-16 (metazebrobot 7cedf98d)
CONSUMERS_SHA256="364c583184763966f65d61d55cc6df7b81f4fb37f39e10de729eb6ed0a43a7de"
LIVE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --live) LIVE="$2"; shift 2 ;;
        --contracts-commit) CONTRACTS_COMMIT="$2"; CONSUMERS_SHA256=""; shift 2 ;;
        -h|--help) sed -n 2,11p "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# agent-contracts is private: fetch through the GitHub API (gh auth, else GITHUB_TOKEN).
fetch() {  # <path in repo> <out>
    local api="repos/${CONTRACTS_REPO}/contents/$1?ref=${CONTRACTS_COMMIT}"
    if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
        gh api "$api" -H "Accept: application/vnd.github.raw" > "$2"
    elif [ -n "${GITHUB_TOKEN:-}" ]; then
        curl -fsSL --max-time 30 -H "Authorization: Bearer ${GITHUB_TOKEN}" \
            -H "Accept: application/vnd.github.raw" "https://api.github.com/${api}" -o "$2"
    else
        echo "need gh auth or GITHUB_TOKEN to read ${CONTRACTS_REPO}" >&2; exit 2
    fi
}
for f in consumers.json verify_consumers.py; do
    fetch "metazebrobot-consumers/${f}" "${work}/${f}"
done
if [ -n "$CONSUMERS_SHA256" ]; then
    echo "${CONSUMERS_SHA256}  ${work}/consumers.json" | sha256sum -c - >/dev/null \
        || { echo "consumers.json at ${CONTRACTS_COMMIT} does not match the recorded digest" >&2; exit 1; }
fi
echo "agent-contracts ${CONTRACTS_COMMIT} metazebrobot-consumers (consumers.json sha256 ${CONSUMERS_SHA256:-unpinned})"
args=(--spec "${work}/consumers.json" --consumer orange)
[ -n "$LIVE" ] && args+=(--live "$LIVE")
python3 "${work}/verify_consumers.py" "${args[@]}"
