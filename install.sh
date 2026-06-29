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

case "$os" in
  Linux)  plat=linux ;;
  Darwin) plat=macos ;;
  *) echo "error: unsupported OS '$os' (supported: Linux, Darwin)" >&2; exit 1 ;;
esac

case "$arch" in
  x86_64|amd64) cpu=x86_64 ;;
  *) echo "error: unsupported architecture '$arch' (only x86_64/amd64 is supported)" >&2; exit 1 ;;
esac

asset="remote_command_server-${plat}-${cpu}"

if [ "$VERSION" = "latest" ]; then
  base="https://github.com/${REPO}/releases/latest/download"
else
  base="https://github.com/${REPO}/releases/download/${VERSION}"
fi

# --- download --------------------------------------------------------------
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Downloading ${asset} (${VERSION}) from ${REPO} ..."
curl -fSL "${base}/${asset}"        -o "${tmp}/${asset}"
curl -fSL "${base}/${asset}.sha256" -o "${tmp}/${asset}.sha256"

# --- verify checksum -------------------------------------------------------
echo "Verifying checksum ..."
(
  cd "$tmp"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum -c "${asset}.sha256"
  else
    shasum -a 256 -c "${asset}.sha256"
  fi
)

# --- install ---------------------------------------------------------------
mkdir -p "$INSTALL_DIR"
if command -v install >/dev/null 2>&1; then
  install -m 0755 "${tmp}/${asset}" "${INSTALL_DIR}/${BIN_NAME}"
else
  cp "${tmp}/${asset}" "${INSTALL_DIR}/${BIN_NAME}"
  chmod 0755 "${INSTALL_DIR}/${BIN_NAME}"
fi

echo "Installed: ${INSTALL_DIR}/${BIN_NAME}"
echo "To uninstall:  rm \"${INSTALL_DIR}/${BIN_NAME}\""

case ":$PATH:" in
  *":$INSTALL_DIR:"*) ;;
  *) echo "NOTE: '${INSTALL_DIR}' is not on your PATH. Add it, e.g.:"
     echo "      export PATH=\"${INSTALL_DIR}:\$PATH\"" ;;
esac

echo "Run with:  ${INSTALL_DIR}/${BIN_NAME} [discovery_port] [command_port] [stream_port] [working_dir]"
