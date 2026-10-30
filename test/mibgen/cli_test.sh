#!/usr/bin/env bash
# The snmpwrap-mibgen command line: usage, error exit codes, output files and incremental writes.
#   cli_test.sh <snmpwrap-mibgen> <test/mibs dir> <scratch dir>
set -u
GEN=$1
MIBS=$2
OUT=$3
rm -rf "$OUT" && mkdir -p "$OUT"

failures=0
check() {  # check <description> <command...>
    local what=$1
    shift
    if "$@"; then echo "ok:   $what"; else echo "FAIL: $what"; failures=$((failures + 1)); fi
}
exit_code() {  # exit_code <expected> <command...>: runs the command, output to $OUT/log
    local want=$1
    shift
    "$@" >"$OUT/log" 2>&1
    [ $? -eq "$want" ]
}

check "no arguments -> exit 2" exit_code 2 "$GEN"
check "usage is printed" grep -q "usage: snmpwrap-mibgen" "$OUT/log"
check "unknown option -> exit 2" exit_code 2 "$GEN" --bogus --module X --out "$OUT" x.txt
check "missing --out -> exit 2" exit_code 2 "$GEN" --module SNMPWRAPPER-TEST-MIB "$MIBS/SNMPWRAPPER-TEST-MIB.txt"

check "broken MIB -> exit 1" exit_code 1 "$GEN" --module BROKEN-SYNTAX-MIB --out "$OUT/broken" "$MIBS/broken/BROKEN-SYNTAX-MIB.txt"
check "broken MIB: Net-SNMP diagnostics" grep -q "BROKEN-SYNTAX-MIB.txt" "$OUT/log"
check "refused module -> exit 1" exit_code 1 "$GEN" --module MEMBER-CLASH-MIB --out "$OUT/clash" "$MIBS/mibgen/MEMBER-CLASH-MIB.txt"
check "refused module: reason" grep -q "both map to the C++ member" "$OUT/log"
check "nothing written for a refused module" test ! -e "$OUT/clash/member_clash_mib.hpp"

gen_ok() { "$GEN" --module SNMPWRAPPER-TEST-MIB --out "$OUT/gen" "$MIBS/SNMPWRAPPER-TEST-MIB.txt" >"$OUT/log" 2>&1; }
check "valid MIB -> exit 0" gen_ok
check "header written" test -s "$OUT/gen/snmpwrapper_test_mib.hpp"
check "source written" test -s "$OUT/gen/snmpwrapper_test_mib.cpp"
check "--name sets the file name" exit_code 0 "$GEN" --module SNMPWRAPPER-TEST-MIB --name other --out "$OUT/gen" "$MIBS/SNMPWRAPPER-TEST-MIB.txt"
check "--name file exists" test -s "$OUT/gen/other.hpp"

# unchanged input: the files are not rewritten (keeps builds incremental)
touch -d "2000-01-01" "$OUT/gen/snmpwrapper_test_mib.hpp" "$OUT/gen/snmpwrapper_test_mib.cpp"
old=$(stat -c %Y "$OUT/gen/snmpwrapper_test_mib.hpp" "$OUT/gen/snmpwrapper_test_mib.cpp")
gen_ok
check "unchanged output is not rewritten" test "$(stat -c %Y "$OUT/gen/snmpwrapper_test_mib.hpp" "$OUT/gen/snmpwrapper_test_mib.cpp")" = "$old"
# changed content is rewritten
echo "// stale" >>"$OUT/gen/snmpwrapper_test_mib.hpp"
gen_ok
check "changed output is rewritten" bash -c "! grep -q '// stale' '$OUT/gen/snmpwrapper_test_mib.hpp'"

echo "$failures failure(s)"
[ "$failures" -eq 0 ]
