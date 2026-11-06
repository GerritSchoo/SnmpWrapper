#!/usr/bin/env bash
# Build + run all tests.  scripts/test.sh [preset] [ctest args...]
#   scripts/test.sh                      all tests (unit tests, then integration tests against a private snmpd)
#   scripts/test.sh debug -L unit        only the unit tests (no snmpd, no network)
#   scripts/test.sh debug -L integration only the integration tests
#   scripts/test.sh debug -R mib_model   only tests matching a regex
set -e
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
PRESET=${1:-debug}
shift || true
cmake --preset "$PRESET" >/dev/null
cmake --build --preset "$PRESET"
ctest --preset "$PRESET" "$@"
