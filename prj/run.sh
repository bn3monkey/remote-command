#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
APP="$BUILD_DIR/remote_command_server_app"

# 1. Configure if build directory doesn't exist
if [ ! -d "$BUILD_DIR" ]; then
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR"
fi

# 2. Build if app doesn't exist
if [ ! -f "$APP" ]; then
    cmake --build "$BUILD_DIR"
fi

# 3. Run
exec "$APP"
