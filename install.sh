#!/bin/sh
# install.sh - download a prebuilt remote-command server binary and install it.
#
# You MUST choose where the binary goes, so you always know what to delete later:
#   --path <dir>      install into a directory you control (created if missing)
#   --default_path    install into the built-in default ($HOME/.local/bin)
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/bn3monkey/remote-command/main/install.sh | sh -s -- --default_path
#   curl -fsSL https://raw.githubusercontent.com/bn3monkey/remote-command/main/install.sh | sh -s -- --path /opt/remote-command
#
# Options:
#   --path <dir>        install directory (mutually exclusive with --default_path)
#   --default_path      use the default install directory
#   --repo <owner/repo> GitHub repository           (default: bn3monkey/remote-command)
#   --version <tag>     release tag, e.g. v1.0.0     (default: latest)
#   -h, --help          show this help
#
# Supported platforms: linux / macOS, x86_64 (amd64) only.
set -eu

DEFAULT_INSTALL_DIR="$HOME/.local/bin"
BIN_NAME="remote-command-server"

REPO="${REMOTE_COMMAND_REPO:-bn3monkey/remote-command}"
VERSION="${REMOTE_COMMAND_VERSION:-latest}"
INSTALL_DIR=""
USE_DEFAULT=0

usage() {
  sed -n '2,20p' "$0" 2>/dev/null || cat <<EOF
Usage: install.sh (--path <dir> | --default_path) [--repo owner/repo] [--version tag]
EOF
}

# --- parse arguments -------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --path)
      shift
      [ $# -gt 0 ] || { echo "error: --path requires a directory argument" >&2; exit 1; }
      INSTALL_DIR="$1" ;;
    --path=*)          INSTALL_DIR="${1#--path=}" ;;
    --default_path|--default-path) USE_DEFAULT=1 ;;
    --repo)            shift; [ $# -gt 0 ] || { echo "error: --repo requires an argument" >&2; exit 1; }; REPO="$1" ;;
    --repo=*)          REPO="${1#--repo=}" ;;
    --version)         shift; [ $# -gt 0 ] || { echo "error: --version requires an argument" >&2; exit 1; }; VERSION="$1" ;;
    --version=*)       VERSION="${1#--version=}" ;;
    -h|--help)         usage; exit 0 ;;
    *) echo "error: unknown option '$1'" >&2; usage; exit 1 ;;
  esac
  shift
done

# --- resolve install directory (a choice is mandatory) ---------------------
if [ -n "$INSTALL_DIR" ] && [ "$USE_DEFAULT" -eq 1 ]; then
  echo "error: --path and --default_path are mutually exclusive" >&2
  exit 1
fi
if [ "$USE_DEFAULT" -eq 1 ]; then
  INSTALL_DIR="$DEFAULT_INSTALL_DIR"
fi
if [ -z "$INSTALL_DIR" ]; then
  echo "error: choose an install location: pass --path <dir> or --default_path" >&2
  echo "       (default would be: $DEFAULT_INSTALL_DIR)" >&2
  exit 1
fi

# --- detect platform -------------------------------------------------------
os="$(uname -s)"
arch="$(uname -m)"

ext=""
case "$os" in
  Linux)  plat=linux ;;
  MINGW*|MSYS*|CYGWIN*) plat=windows; ext=".exe" ;;  # Git Bash / MSYS on Windows
  Darwin)
    echo "error: no prebuilt macOS binary is published; build from source instead." >&2
    echo "       see the 'Building the Server Binary' section in the README." >&2
    exit 1 ;;
  *) echo "error: unsupported OS '$os' (supported: Linux, Windows via MSYS/MinGW)" >&2; exit 1 ;;
esac

case "$arch" in
  x86_64|amd64) cpu=x86_64 ;;
  *) echo "error: unsupported architecture '$arch' (only x86_64/amd64 is supported)" >&2; exit 1 ;;
esac

asset="remote_command_server-${plat}-${cpu}${ext}"
BIN_NAME="${BIN_NAME}${ext}"

if [ "$VERSION" = "latest" ]; then
  base="https://github.com/${REPO}/releases/latest/download"
else
  base="https://github.com/${REPO}/releases/download/${VERSION}"
fi

# --- download --------------------------------------------------------------
# The download dir lives INSIDE the install path (".dl"), not in /tmp. This
# avoids per-environment /tmp differences (private tmpfs, PrivateTmp, mktemp
# quirks) that were deleting the temp dir mid-run. We remove it up front (in
# case a previous run left it behind) and again when we are done.
mkdir -p "$INSTALL_DIR"
DL="${INSTALL_DIR}/.dl"
rm -rf "$DL"
mkdir -p "$DL"

echo "Downloading ${asset} (${VERSION}) from ${REPO} ..."
curl -fSL "${base}/${asset}"        -o "${DL}/${asset}"
curl -fSL "${base}/${asset}.sha256" -o "${DL}/${asset}.sha256"

# --- verify checksum -------------------------------------------------------
# Compare hashes directly (no subshell / no cd). The .sha256 file is
# "<hash>  <filename>"; we only need the hash field.
echo "Verifying checksum ..."
expected="$(awk '{print $1}' "${DL}/${asset}.sha256")"
if command -v sha256sum >/dev/null 2>&1; then
  actual="$(sha256sum "${DL}/${asset}" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
  actual="$(shasum -a 256 "${DL}/${asset}" | awk '{print $1}')"
else
  echo "error: neither 'sha256sum' nor 'shasum' is available to verify the download" >&2
  rm -rf "$DL"
  exit 1
fi
if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
  echo "error: checksum mismatch for ${asset}" >&2
  echo "       expected: ${expected:-<empty>}" >&2
  echo "       actual:   ${actual:-<empty>}" >&2
  rm -rf "$DL"
  exit 1
fi

# --- install ---------------------------------------------------------------
if command -v install >/dev/null 2>&1; then
  install -m 0755 "${DL}/${asset}" "${INSTALL_DIR}/${BIN_NAME}"
else
  cp "${DL}/${asset}" "${INSTALL_DIR}/${BIN_NAME}"
  chmod 0755 "${INSTALL_DIR}/${BIN_NAME}"
fi

# download dir no longer needed
rm -rf "$DL"

echo "Installed: ${INSTALL_DIR}/${BIN_NAME}"
echo "To uninstall:  rm \"${INSTALL_DIR}/${BIN_NAME}\""

case ":$PATH:" in
  *":$INSTALL_DIR:"*) ;;
  *) echo "NOTE: '${INSTALL_DIR}' is not on your PATH. Add it, e.g.:"
     echo "      export PATH=\"${INSTALL_DIR}:\$PATH\"" ;;
esac

echo "Run with:  ${INSTALL_DIR}/${BIN_NAME} [discovery_port] [command_port] [stream_port] [working_dir]"
