# Sourced by scripts/install_orange_*_wrapper.sh. Every Orange sudo wrapper is
# installed twice from the same source (docs/opt_install_plan_2026-11-14.md,
# P3); the installed name sets the wrapper's mode:
#   <bin-dir>/orange-<name>      installed releases only (default /opt/orange/current)
#   <bin-dir>/orange-dev-<name>  the development tree only
# Each gets its own sudoers entry with --install-sudoers.

ORANGE_WRAPPER_BIN_DIR="/usr/local/bin"
ORANGE_WRAPPER_SUDOERS_DIR="/etc/sudoers.d"
ORANGE_WRAPPER_INSTALL_SUDOERS=0
ORANGE_WRAPPER_SUDOERS_USER="jeremy"
ORANGE_WRAPPER_RELEASE_BIN_DIR="/opt/orange/current/bin"
ORANGE_WRAPPER_NO_ROOT=0

# Options every installer takes. Prints nothing; returns 2 on a bad option.
# Sets ORANGE_WRAPPER_ARGS_CONSUMED to the number of words it took (0 if the
# word is not one of these options).
orange_wrapper_install_option() {
  ORANGE_WRAPPER_ARGS_CONSUMED=0
  case "$1" in
    --bin-dir)
      [[ $# -ge 2 ]] || { echo "--bin-dir requires a value." >&2; return 2; }
      ORANGE_WRAPPER_BIN_DIR="$2"; ORANGE_WRAPPER_ARGS_CONSUMED=2 ;;
    --sudoers-dir)
      [[ $# -ge 2 ]] || { echo "--sudoers-dir requires a value." >&2; return 2; }
      ORANGE_WRAPPER_SUDOERS_DIR="$2"; ORANGE_WRAPPER_ARGS_CONSUMED=2 ;;
    --install-sudoers)
      ORANGE_WRAPPER_INSTALL_SUDOERS=1; ORANGE_WRAPPER_ARGS_CONSUMED=1 ;;
    --no-root)
      # Tests: install as the current user into a test --bin-dir/--sudoers-dir
      # (no chown, no visudo).
      ORANGE_WRAPPER_NO_ROOT=1; ORANGE_WRAPPER_ARGS_CONSUMED=1 ;;
  esac
  return 0
}

orange_wrapper_install_options_help() {
  cat <<'EOH'
  --bin-dir <dir>          Install folder. Default /usr/local/bin; installs
                           orange-<name> (releases) and orange-dev-<name> (dev tree).
  --install-sudoers        Install a narrow NOPASSWD sudoers entry for jeremy,
                           one per installed command.
  --sudoers-dir <dir>      Sudoers folder. Default /etc/sudoers.d
  --no-root                Tests only: install as the current user (needs a
                           test --bin-dir; no chown, no visudo).
EOH
}

orange_wrapper_as_root() {
  if [[ "${EUID}" -eq 0 || "$ORANGE_WRAPPER_NO_ROOT" == "1" ]]; then "$@"; else sudo "$@"; fi
}

# orange_wrapper_install <label> <source> <name>
# Installs both commands and, if asked, their sudoers entries.
orange_wrapper_install() {
  local label="$1" source="$2" name="$3" target sudoers line
  [[ -f "$source" ]] || { echo "Wrapper source not found: $source" >&2; return 1; }
  if [[ "$ORANGE_WRAPPER_NO_ROOT" == "1" && "$ORANGE_WRAPPER_BIN_DIR" == "/usr/local/bin" ]]; then
    echo "--no-root needs a test --bin-dir" >&2
    return 2
  fi
  bash -n "$source"
  echo "[$label] source=$source"
  for target in "$ORANGE_WRAPPER_BIN_DIR/orange-$name" "$ORANGE_WRAPPER_BIN_DIR/orange-dev-$name"; do
    echo "[$label] target=$target"
    if [[ "$ORANGE_WRAPPER_NO_ROOT" == "1" ]]; then
      install -m 0755 "$source" "$target"
    else
      orange_wrapper_as_root install -o root -g root -m 0755 "$source" "$target"
    fi
    if [[ "$ORANGE_WRAPPER_INSTALL_SUDOERS" == "1" ]]; then
      sudoers="$ORANGE_WRAPPER_SUDOERS_DIR/$(basename -- "$target")"
      line="$ORANGE_WRAPPER_SUDOERS_USER ALL=(root) NOPASSWD: $target"
      echo "[$label] sudoers=$sudoers"
      printf '%s\n' "$line" | orange_wrapper_as_root tee "$sudoers" >/dev/null
      orange_wrapper_as_root chmod 0440 "$sudoers"
      [[ "$ORANGE_WRAPPER_NO_ROOT" == "1" ]] || orange_wrapper_as_root visudo -cf "$sudoers"
    fi
  done
}

# True when an installed release exists for the release command's checks.
orange_wrapper_release_installed() {
  [[ -d "$ORANGE_WRAPPER_RELEASE_BIN_DIR" ]]
}

# orange_wrapper_require_help <target> <text>...
orange_wrapper_require_help() {
  local target="$1" help required
  shift
  help="$("$target" --help)"
  for required in "$@"; do
    if [[ "$help" != *"$required"* ]]; then
      echo "Installed wrapper help is missing expected text: $required ($target)" >&2
      return 1
    fi
  done
}
