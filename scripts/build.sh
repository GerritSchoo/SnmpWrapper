#!/usr/bin/env bash
# Configure + build.      scripts/build.sh [preset]      preset: debug (default), release, shared
set -e
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
PRESET=${1:-debug}
cmake --preset "$PRESET"
cmake --build --preset "$PRESET"
