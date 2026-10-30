#!/usr/bin/env bash
# Line / branch coverage of the library and the generator over ALL tests (unit + integration).
#   scripts/coverage.sh [ctest args...]     e.g. scripts/coverage.sh -L unit   (unit tests only)
# Report: build/coverage/html/index.html. Needs lcov (sudo apt install lcov).
set -e
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
command -v lcov >/dev/null || { echo "missing: lcov (sudo apt install lcov)"; exit 1; }

OUT=build/coverage
cmake --preset coverage >/dev/null
cmake --build --preset coverage
lcov --quiet --zerocounters --directory "$OUT"
ctest --preset coverage "$@" || echo "warning: some tests failed - the report covers what ran"

# lcov 1.15 with Perl 5.34 prints "Subroutine ... redefined" warnings: harmless, filtered
lcov --quiet --capture --directory "$OUT" --rc lcov_branch_coverage=1 --output-file "$OUT/all.info" \
     2> >(grep -v "Subroutine .* redefined" >&2)
lcov --quiet --extract "$OUT/all.info" "$SNMPWRAP_ROOT/src/*" "$SNMPWRAP_ROOT/include/*" "$SNMPWRAP_ROOT/tools/*" \
     --rc lcov_branch_coverage=1 --output-file "$OUT/snmpwrap.info"
genhtml --quiet --branch-coverage --title "snmpwrap" --output-directory "$OUT/html" "$OUT/snmpwrap.info"
lcov --summary "$OUT/snmpwrap.info" --rc lcov_branch_coverage=1
echo "report: $OUT/html/index.html"
