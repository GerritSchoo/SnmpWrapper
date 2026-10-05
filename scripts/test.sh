#!/usr/bin/env bash
# Build + run all tests.  scripts/test.sh [preset] [ctest args...]
#   scripts/test.sh                      all tests (unit, MIB, lifecycle, integration against snmpd)
#   scripts/test.sh debug -R mib_model   only tests matching a regex
set -e
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
PRESET=${1:-debug}
shift || true
cmake --preset "$PRESET" >/dev/null
cmake --build --preset "$PRESET"
ctest --preset "$PRESET" "$@"
