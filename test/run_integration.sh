#!/usr/bin/env bash
# End-to-end test: unprivileged snmpd (master agentx) + examples/test_agent + net-snmp tools + snmpwrap::Client.
# The same core checks run for SNMPv1, v2c and v3 (noAuthNoPriv, authNoPriv, authPriv).
#
#   run_integration.sh <test_agent> <client_cli> <net-snmp prefix> <mibs dir>
set -u

AGENT_BIN=$1
CLIENT_BIN=$2
PREFIX=$3
MIB_DIR=$4

export PATH="$PREFIX/bin:$PREFIX/sbin:$PATH"
export LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

for tool in snmpd snmpget snmpset snmpwalk snmpbulkwalk snmpgetnext snmptranslate snmptrapd; do
    command -v "$tool" >/dev/null || { echo "SKIP: $tool not found"; exit 77; }
done

PORT=${SNMPWRAP_TEST_PORT:-11161}
XPORT=$((PORT + 1))
TPORT=$((PORT + 2))
BIG_ROWS=3000
WORK=$(mktemp -d)
PIDS=()
cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
}
trap cleanup EXIT

AUTHPASS=authpass123
PRIVPASS=privpass123

cat > "$WORK/snmpd.conf" <<EOF
agentaddress udp:127.0.0.1:$PORT
rocommunity public 127.0.0.1
rwcommunity private 127.0.0.1
createUser noauth
createUser authuser SHA $AUTHPASS
createUser privuser SHA $AUTHPASS AES $PRIVPASS
rwuser noauth noauth
rwuser authuser auth
rwuser privuser priv
master agentx
agentXSocket tcp:127.0.0.1:$XPORT
trap2sink 127.0.0.1:$TPORT public
trapsink 127.0.0.1:$TPORT public
EOF
cat > "$WORK/snmptrapd.conf" <<EOF
authCommunity log public
EOF

# do not read any user / system configuration of net-snmp tools
export SNMPCONFPATH="$WORK/none"
export SNMP_PERSISTENT_DIR="$WORK/persist-client"
mkdir -p "$WORK/persist-client" "$WORK/persist-snmpd"

TARGET=127.0.0.1:$PORT
ROOT=.1.3.6.1.4.1.99999          # snmpWrapperTestMIB (leading dot: net-snmp tools)
ROOTN=1.3.6.1.4.1.99999          # same, as printed by snmpwrap::Oid::str()

fails=0
pass() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; shift; for l in "$@"; do echo "       $l"; done; fails=$((fails + 1)); }
expect_eq() { # desc actual expected
    if [ "$2" = "$3" ]; then pass "$1"; else fail "$1" "expected: $3" "actual:   $2"; fi
}
expect_contains() { # desc text needle
    case "$2" in *"$3"*) pass "$1" ;; *) fail "$1" "expected to contain: $3" "actual: $2" ;; esac
}
lines() { printf '%s\n' "$1" | grep -c .; }

# --- start daemons --------------------------------------------------------------------------------
snmptrapd -f -C -c "$WORK/snmptrapd.conf" -Lf "$WORK/traps.log" "udp:127.0.0.1:$TPORT" >"$WORK/trapd.out" 2>&1 &
PIDS+=($!)
SNMP_PERSISTENT_DIR="$WORK/persist-snmpd" snmpd -f -C -r -c "$WORK/snmpd.conf" -Lf "$WORK/snmpd.log" -p "$WORK/snmpd.pid" \
    >"$WORK/snmpd.out" 2>&1 &
PIDS+=($!)

probe() { snmpget -v2c -c public -t 1 -r 0 "$TARGET" .1.3.6.1.2.1.1.1.0 >/dev/null 2>&1; }
for _ in $(seq 1 50); do probe && break; sleep 0.2; done
probe || { echo "snmpd did not come up"; cat "$WORK/snmpd.out" "$WORK/snmpd.log" 2>/dev/null; exit 1; }

"$AGENT_BIN" --socket "tcp:127.0.0.1:$XPORT" --trap-every 3 --big-table $BIG_ROWS >"$WORK/agent.out" 2>&1 &
AGENT_PID=$!
PIDS+=($AGENT_PID)

# wait until the subagent is connected AND has registered its objects
# (snmpget exits 0 for "No Such Object" as well, so look at the answer itself)
agent_ready() { snmpget -v2c -c public -t 1 -r 0 -Oqv "$TARGET" $ROOT.1.1.0 2>/dev/null | grep -qv -E 'No Such|^$'; }
for _ in $(seq 1 100); do agent_ready && break; sleep 0.2; done
agent_ready || { echo "agent did not register its objects"; cat "$WORK/agent.out"; exit 1; }

# =============================================================================================
# Version matrix: the same core checks for every protocol version / security level
# =============================================================================================
TA=()   # tool arguments of the current mode
CA=()   # client_cli arguments of the current mode
tool() { local cmd=$1; shift; "$cmd" "${TA[@]}" -On "$@"; }
cli()  { "$CLIENT_BIN" "${CA[@]}" "$@"; }

# mode <name> <idx> <tool args> -- <cli args> -- <wrongValue> <wrongType> <notWritable> <noCreation> <commitFailed> <cli code wrongValue> <cli code commitFailed>
mode() {
    local name=$1 idx=$2; shift 2
    local ta=() ca=() rest=() cur=ta
    for a in "$@"; do
        if [ "$a" = "--" ]; then
            case $cur in ta) cur=ca ;; ca) cur=rest ;; esac
            continue
        fi
        case $cur in ta) ta+=("$a") ;; ca) ca+=("$a") ;; rest) rest+=("$a") ;; esac
    done
    TA=("${ta[@]}"); CA=("${ca[@]}")
    local E_WV=${rest[0]} E_WT=${rest[1]} E_NW=${rest[2]} E_NC=${rest[3]} E_CF=${rest[4]} C_WV=${rest[5]} C_CF=${rest[6]}
    local limit=$((40 + idx)) row=$((100 + idx))
    local v1=0; [ "$name" = "v1" ] && v1=1

    echo "== [$name] reads"
    expect_eq "get table cell"    "$(tool snmpget -Oqv "$TARGET" $ROOT.2.1.2.1 2>&1)" '"eth0"'
    expect_eq "get table cell (gauge)" "$(tool snmpget -Oqv "$TARGET" $ROOT.2.1.3.2 2>&1)" "200"
    expect_eq "getnext scalar -> scalar" "$(tool snmpgetnext -Oqn "$TARGET" $ROOT.1.4.0 2>&1 | cut -d' ' -f1)" "$ROOT.1.5.0"
    expect_eq "getnext scalar -> table"  "$(tool snmpgetnext -Oqn "$TARGET" $ROOT.1.6.0 2>&1 | cut -d' ' -f1)" "$ROOT.2.1.2.1"
    # v1 does not know Counter64: the agent skips swtBigCounter in GETNEXT
    expect_eq "getnext skips Counter64 in v1 only" "$(tool snmpgetnext -Oqn "$TARGET" $ROOT.1.5.0 2>&1 | cut -d' ' -f1)"         "$([ $v1 = 1 ] && echo $ROOT.2.1.2.1 || echo $ROOT.1.6.0)"
    w=$(tool snmpwalk -Oqn "$TARGET" $ROOT.2 2>&1)
    expect_eq "walk table: 3 rows x 3 columns" "$(lines "$w")" 9
    expect_eq "walk table: last OID" "$(printf '%s\n' "$w" | tail -1 | cut -d' ' -f1)" "$ROOT.2.1.4.3"
    expect_eq "walk multi-index table: 3 rows" "$(lines "$(tool snmpwalk -Oqn "$TARGET" $ROOT.5 2>&1)")" 3
    expect_eq "get multi-index cell" \
        "$(tool snmpget -Oqv "$TARGET" $ROOT.5.1.4.10.0.0.1.80.119.101.98 2>&1)" "3"
    c64=$(tool snmpget "$TARGET" $ROOT.1.6.0 2>&1)
    if [ $v1 = 1 ]; then expect_contains "Counter64 is not visible in v1" "$c64" "noSuchName"
    else expect_contains "get Counter64" "$c64" "4294967297"; fi
    expect_contains "get unknown object -> noSuchObject" "$(tool snmpget "$TARGET" $ROOT.9.9.0 2>&1)" \
        "$([ $v1 = 1 ] && echo noSuchName || echo 'No Such Object')"
    expect_contains "get missing row -> noSuchInstance" "$(tool snmpget "$TARGET" $ROOT.2.1.2.9 2>&1)" \
        "$([ $v1 = 1 ] && echo noSuchName || echo 'No Such Instance')"

    echo "== [$name] reads via snmpwrap::Client"
    expect_eq "client get" "$(cli "$TARGET" get $ROOTN.2.1.2.1 2>&1)" "$ROOTN.2.1.2.1 = OctetString: \"eth0\""
    expect_eq "client walk table (GETBULK or GETNEXT): 9 lines" "$(lines "$(cli "$TARGET" walk $ROOTN.2 2>&1)")" 9
    expect_eq "client getnext" "$(cli "$TARGET" getnext $ROOTN.1.6.0 2>&1 | cut -d' ' -f1)" "$ROOTN.2.1.2.1"
    if [ $v1 = 1 ]; then
        expect_contains "client Counter64 in v1 -> agent error (noSuchName=2)" "$(cli "$TARGET" get $ROOTN.1.6.0 2>&1)" "agent error 2"
        expect_contains "client getbulk in v1 is refused" "$(cli "$TARGET" getbulk $ROOTN.2 2>&1)" "GETBULK is not available"
    else
        expect_contains "client Counter64" "$(cli "$TARGET" get $ROOTN.1.6.0 2>&1)" "Counter64: 4294967297"
        expect_eq "client getbulk returns 5 varbinds" "$(lines "$(cli "$TARGET" getbulk $ROOTN.2 2>&1)")" 5
        expect_contains "client get missing -> NoSuchInstance" "$(cli "$TARGET" get $ROOTN.2.1.2.9 2>&1)" "NoSuchInstance"
        expect_contains "client get unknown -> NoSuchObject" "$(cli "$TARGET" get $ROOTN.9.9.0 2>&1)" "NoSuchObject"
    fi

    echo "== [$name] writes"
    tool snmpset "$TARGET" $ROOT.1.4.0 i $limit >/dev/null 2>&1
    expect_eq "set swtLimit=$limit" "$(tool snmpget -Oqv "$TARGET" $ROOT.1.4.0 2>&1)" "$limit"
    expect_contains "out-of-range value rejected ($E_WV)" "$(tool snmpset "$TARGET" $ROOT.1.4.0 i 500 2>&1)" "$E_WV"
    expect_contains "wrong type rejected ($E_WT)" "$(tool snmpset "$TARGET" $ROOT.1.4.0 s hello 2>&1)" "$E_WT"
    expect_contains "read-only object rejected ($E_NW)" "$(tool snmpset "$TARGET" $ROOT.1.2.0 u 5 2>&1)" "$E_NW"
    expect_contains "cell of missing row rejected ($E_NC)" "$(tool snmpset "$TARGET" $ROOT.2.1.2.99 s x 2>&1)" "$E_NC"
    expect_eq "value unchanged after rejected sets" "$(tool snmpget -Oqv "$TARGET" $ROOT.1.4.0 2>&1)" "$limit"

    # multi-varbind SET: second varbind fails in the apply phase -> the first one is rolled back
    e=$(tool snmpset "$TARGET" $ROOT.1.4.0 i 33 $ROOT.2.1.2.1 s commitfail 2>&1)
    expect_contains "multi-SET with apply failure reports $E_CF" "$e" "$E_CF"
    expect_eq "multi-SET rolled back (swtLimit)" "$(tool snmpget -Oqv "$TARGET" $ROOT.1.4.0 2>&1)" "$limit"
    expect_eq "multi-SET rolled back (cell)"     "$(tool snmpget -Oqv "$TARGET" $ROOT.2.1.2.1 2>&1)" '"eth0"'
    # validation failure of the 2nd varbind: nothing is applied
    tool snmpset "$TARGET" $ROOT.1.4.0 i 34 $ROOT.1.4.0 s oops >/dev/null 2>&1
    expect_eq "multi-SET with invalid varbind changes nothing" "$(tool snmpget -Oqv "$TARGET" $ROOT.1.4.0 2>&1)" "$limit"
    tool snmpset "$TARGET" $ROOT.1.4.0 i $((limit + 1)) $ROOT.2.1.3.3 u 55 >/dev/null 2>&1
    expect_eq "valid multi-SET applies both" \
        "$(tool snmpget -Oqv "$TARGET" $ROOT.1.4.0 $ROOT.2.1.3.3 2>&1 | tr '\n' ' ')" "$((limit + 1)) 55 "
    tool snmpset "$TARGET" $ROOT.2.1.3.3 u 5 >/dev/null 2>&1

    # RowStatus: create, read back, destroy
    tool snmpset "$TARGET" $ROOT.4.1.2.$row s "row-$name" $ROOT.4.1.4.$row i 4 >/dev/null 2>&1
    expect_eq "createAndGo: status active(1)" "$(tool snmpget -Oqv "$TARGET" $ROOT.4.1.4.$row 2>&1)" 1
    expect_eq "createAndGo: name stored"      "$(tool snmpget -Oqv "$TARGET" $ROOT.4.1.2.$row 2>&1)" "\"row-$name\""
    tool snmpset "$TARGET" $ROOT.4.1.4.$row i 6 >/dev/null 2>&1
    expect_contains "destroy: row gone" "$(tool snmpget "$TARGET" $ROOT.4.1.4.$row 2>&1)" \
        "$([ $v1 = 1 ] && echo noSuchName || echo 'No Such Instance')"

    echo "== [$name] writes via snmpwrap::Client"
    expect_eq "client set" "$(cli "$TARGET" set $ROOTN.1.4.0 i $((limit + 2)) 2>&1)" "OK"
    expect_eq "client set visible to snmpget" "$(tool snmpget -Oqv "$TARGET" $ROOT.1.4.0 2>&1)" "$((limit + 2))"
    expect_contains "client rejected set -> agent error $C_WV" "$(cli "$TARGET" set $ROOTN.1.4.0 i 1000 2>&1)" "agent error $C_WV"
    e=$(cli "$TARGET" set $ROOTN.1.4.0 i 35 $ROOTN.2.1.2.1 s commitfail 2>&1)
    expect_contains "client multi-SET apply failure -> agent error $C_CF (varbind 2)" "$e" "agent error $C_CF (varbind 2)"
    expect_eq "client multi-SET rolled back" "$(tool snmpget -Oqv "$TARGET" $ROOT.1.4.0 2>&1)" "$((limit + 2))"
    expect_eq "client createAndGo" \
        "$(cli "$TARGET" set $ROOTN.4.1.2.$row s "c-$name" $ROOTN.4.1.3.$row u 12 $ROOTN.4.1.4.$row i 4 2>&1)" "OK"
    expect_eq "client created row readable" "$(tool snmpget -Oqv "$TARGET" $ROOT.4.1.2.$row $ROOT.4.1.3.$row $ROOT.4.1.4.$row 2>&1 | tr '\n' ' ')" "\"c-$name\" 12 1 "
    expect_eq "client destroy" "$(cli "$TARGET" set $ROOTN.4.1.4.$row i 6 2>&1)" "OK"
    expect_eq "row table empty again" "$(lines "$(tool snmpwalk -Oqn "$TARGET" $ROOT.4 2>&1 | grep -v 'No Such')")" 0
}

mode v1       1 -v1 -c private                                                          -- -v 1 -c private -- badValue badValue noSuchName noSuchName genError 3 5
mode v2c      2 -v2c -c private                                                         -- -v 2c -c private -- wrongValue wrongType notWritable noCreation commitFailed 10 14
mode v3-noauth 3 -v3 -l noAuthNoPriv -u noauth                                          -- -v 3 -u noauth -l noAuthNoPriv -- wrongValue wrongType notWritable noCreation commitFailed 10 14
mode v3-auth   4 -v3 -l authNoPriv -u authuser -a SHA -A $AUTHPASS                      -- -v 3 -u authuser -l authNoPriv -a SHA -A $AUTHPASS -- wrongValue wrongType notWritable noCreation commitFailed 10 14
# authPriv depends on the privacy algorithms compiled into the installed Net-SNMP. Probe with the
# Net-SNMP tools themselves; if THEY cannot talk authPriv either, this is not a wrapper problem -> SKIP.
skipped=0
if snmpget -v3 -l authPriv -u privuser -a SHA -A $AUTHPASS -x AES -X $PRIVPASS -t 2 -r 0 "$TARGET" $ROOT.1.1.0 >/dev/null 2>&1; then
    mode v3-priv   5 -v3 -l authPriv -u privuser -a SHA -A $AUTHPASS -x AES -X $PRIVPASS    -- -v 3 -u privuser -l authPriv -a SHA -A $AUTHPASS -x AES -X $PRIVPASS -- wrongValue wrongType notWritable noCreation commitFailed 10 14
else
    echo "== [v3-priv] SKIPPED: the Net-SNMP tools themselves get no authPriv/AES answer from snmpd"
    echo "   (installed Net-SNMP: $(net-snmp-config --configure-options | tr ' ' '
' | grep -i ssl)); wrapper not involved"
    skipped=1
fi

# =============================================================================================
# Details that do not depend on the protocol version (SNMPv2c)
# =============================================================================================
TA=(-v2c -c private)
CA=(-v 2c -c private)

echo "== MIB file"
expect_eq "MIB parses, swtEntryName resolves" \
    "$(snmptranslate -M "+$MIB_DIR" -m +SNMPWRAPPER-TEST-MIB -On SNMPWRAPPER-TEST-MIB::swtEntryName 2>&1)" "$ROOT.2.1.2"
expect_eq "MIB parses, swtRowStatus resolves" \
    "$(snmptranslate -M "+$MIB_DIR" -m +SNMPWRAPPER-TEST-MIB -On SNMPWRAPPER-TEST-MIB::swtRowStatus 2>&1)" "$ROOT.4.1.4"
RO=(-v2c -c public -M "+$MIB_DIR" -m +SNMPWRAPPER-TEST-MIB)
expect_eq "walk with symbolic names (composite index with IMPLIED string)" \
    "$(snmpget "${RO[@]}" -Oqv "$TARGET" "SNMPWRAPPER-TEST-MIB::swtConnState.10.0.0.1.80.'web'" 2>&1)" "established"

echo "== scalars and whole-subtree walks"
c1=$(tool snmpget -Oqv "$TARGET" $ROOT.1.2.0 2>&1)
c2=$(tool snmpget -Oqv "$TARGET" $ROOT.1.2.0 2>&1)
[ "$c2" -eq $((c1 + 1)) ] 2>/dev/null && pass "counter increments per read ($c1 -> $c2)" || fail "counter increments" "c1=$c1 c2=$c2"
expect_contains "swtUptime is TimeTicks" "$(tool snmpget "$TARGET" $ROOT.1.5.0 2>&1)" "Timeticks"
full=$(tool snmpwalk -Oqn "$TARGET" $ROOT 2>&1 | grep -v -E '\.99999\.3\.')
expect_eq "full walk: 6 scalars + 9 + 3 + 2 x $BIG_ROWS" "$(lines "$full")" $((6 + 9 + 3 + 2 * BIG_ROWS))
expect_eq "full walk is in strictly ascending OID order" \
    "$(printf '%s\n' "$full" | cut -d' ' -f1 | sort -c -V -u 2>&1 && echo sorted)" "sorted"

echo "== large table (nextRow/hasRow, $BIG_ROWS rows)"
bulk=$(tool snmpbulkwalk -Oqn -Cr25 "$TARGET" $ROOT.6 2>&1)
expect_eq "bulkwalk returns every row" "$(lines "$bulk")" $((2 * BIG_ROWS))
expect_eq "bulkwalk: first" "$(printf '%s\n' "$bulk" | head -1)" "$ROOT.6.1.2.2 1"
expect_eq "bulkwalk: last"  "$(printf '%s\n' "$bulk" | tail -1)" "$ROOT.6.1.3.$((2 * BIG_ROWS)) $((2 * BIG_ROWS))"
expect_eq "get middle row"  "$(tool snmpget -Oqv "$TARGET" $ROOT.6.1.2.2000 2>&1)" 1000
expect_contains "odd index does not exist" "$(tool snmpget "$TARGET" $ROOT.6.1.2.2001 2>&1)" "No Such Instance"
expect_eq "getnext between rows" "$(tool snmpgetnext -Oqn "$TARGET" $ROOT.6.1.2.2001 2>&1 | cut -d' ' -f1)" "$ROOT.6.1.2.2002"
expect_eq "client walk of the big table" "$(lines "$(cli "$TARGET" walk $ROOTN.6 2>&1)")" $((2 * BIG_ROWS))

echo "== RowStatus state machine"
S=$ROOT.4.1.4; N=$ROOT.4.1.2; V=$ROOT.4.1.3
expect_contains "createAndGo without the required column -> inconsistentValue" \
    "$(tool snmpset "$TARGET" $V.20 u 5 $S.20 i 4 2>&1)" "inconsistentValue"
expect_contains "  ... and the error names the RowStatus varbind" \
    "$(tool snmpset "$TARGET" $V.20 u 5 $S.20 i 4 2>&1)" "Failed object: $S.20"
expect_contains "  ... nothing was created" "$(tool snmpget "$TARGET" $S.20 2>&1)" "No Such Instance"
expect_contains "cell of a non-existing row without create -> inconsistentName" "$(tool snmpset "$TARGET" $N.20 s x 2>&1)" "inconsistentName"
expect_contains "active on a non-existing row -> inconsistentValue" "$(tool snmpset "$TARGET" $S.20 i 1 2>&1)" "inconsistentValue"
expect_contains "invalid RowStatus value -> wrongValue" "$(tool snmpset "$TARGET" $S.20 i 9 2>&1)" "wrongValue"
expect_contains "notReady is not settable -> wrongValue" "$(tool snmpset "$TARGET" $S.20 i 3 2>&1)" "wrongValue"
tool snmpset "$TARGET" $S.20 i 5 >/dev/null 2>&1
expect_eq "createAndWait without data -> notReady(3)" "$(tool snmpget -Oqv "$TARGET" $S.20 2>&1)" 3
expect_contains "activating an incomplete row -> inconsistentValue" "$(tool snmpset "$TARGET" $S.20 i 1 2>&1)" "inconsistentValue"
expect_eq "  ... status unchanged" "$(tool snmpget -Oqv "$TARGET" $S.20 2>&1)" 3
tool snmpset "$TARGET" $N.20 s twenty >/dev/null 2>&1
expect_eq "supplying the name promotes to notInService(2)" "$(tool snmpget -Oqv "$TARGET" $S.20 2>&1)" 2
tool snmpset "$TARGET" $S.20 i 1 >/dev/null 2>&1
expect_eq "activate -> active(1)" "$(tool snmpget -Oqv "$TARGET" $S.20 2>&1)" 1
expect_contains "createAndGo on an existing row -> inconsistentValue" "$(tool snmpset "$TARGET" $N.20 s y $S.20 i 4 2>&1)" "inconsistentValue"
expect_contains "column validator also applies on create -> wrongValue" \
    "$(tool snmpset "$TARGET" $N.21 s x $V.21 u 5000 $S.21 i 4 2>&1)" "wrongValue"
expect_contains "  ... error index names the value column" \
    "$(tool snmpset "$TARGET" $N.21 s x $V.21 u 5000 $S.21 i 4 2>&1)" "Failed object: $V.21"
# two rows in one request, rows interleaved in the table walk
tool snmpset "$TARGET" $N.31 s b $S.31 i 4 $N.30 s a $S.30 i 4 >/dev/null 2>&1
expect_eq "two rows created by one request, walk is ordered" \
    "$(tool snmpwalk -Oqv "$TARGET" $N 2>&1 | tr '\n' ' ')" "\"twenty\" \"a\" \"b\" "
# rollback of a create: a later varbind fails in the apply phase
e=$(tool snmpset "$TARGET" $N.32 s c $S.32 i 4 $N.30 s commitfail 2>&1)
expect_contains "create + failing apply -> commitFailed" "$e" "commitFailed"
expect_contains "  ... the created row was rolled back" "$(tool snmpget "$TARGET" $S.32 2>&1)" "No Such Instance"
expect_eq "  ... the other row is unchanged" "$(tool snmpget -Oqv "$TARGET" $N.30 2>&1)" '"a"'
# a destroy is not executed when the request fails elsewhere
e=$(tool snmpset "$TARGET" $S.30 i 6 $N.31 s commitfail 2>&1)
expect_contains "destroy + failing apply -> commitFailed" "$e" "commitFailed"
expect_eq "  ... the row survived" "$(tool snmpget -Oqv "$TARGET" $S.30 2>&1)" 1
tool snmpset "$TARGET" $S.20 i 6 $S.30 i 6 $S.31 i 6 >/dev/null 2>&1
expect_eq "destroy of three rows in one request" "$(lines "$(tool snmpwalk -Oqn "$TARGET" $ROOT.4 2>&1 | grep -v 'No Such')")" 0
expect_eq "destroy of a non-existing row is a no-op" "$(tool snmpset "$TARGET" $S.77 i 6 >/dev/null 2>&1; echo $?)" 0

echo "== composite-index table"
C=$ROOT.5.1.4
expect_eq "walk order follows the OID order of the index" \
    "$(tool snmpwalk -Oqn "$TARGET" $ROOT.5 2>&1 | cut -d' ' -f1 | tr '\n' ' ')" \
    "$C.10.0.0.1.80.119.101.98 $C.10.0.0.1.443.116.108.115 $C.10.0.0.2.22.115.115.104 "
tool snmpset "$TARGET" $C.10.0.0.2.22.115.115.104 i 1 >/dev/null 2>&1
expect_eq "set state" "$(tool snmpget -Oqv "$TARGET" $C.10.0.0.2.22.115.115.104 2>&1)" 1
tool snmpset "$TARGET" $C.10.0.0.2.22.115.115.104 i 3 >/dev/null 2>&1
expect_contains "invalid state -> wrongValue" "$(tool snmpset "$TARGET" $C.10.0.0.2.22.115.115.104 i 9 2>&1)" "wrongValue"
expect_contains "no such connection -> noCreation" "$(tool snmpset "$TARGET" $C.10.0.0.9.1.97 i 1 2>&1)" "noCreation"
expect_contains "malformed index (trailing sub-id) does not exist" "$(tool snmpget "$TARGET" $C.10.0.0.2.22.115.115.104.300 2>&1)" "No Such Instance"

echo "== client with MIB (client_cli -m): names, formatted values, types from the MIB"
MCLI=("$CLIENT_BIN" -v 2c -c private -m "$MIB_DIR/SNMPWRAPPER-TEST-MIB.txt")
expect_eq "get by name, enum label in output" "$("${MCLI[@]}" "$TARGET" get swtEntryStatus.1 2>&1)" "swtEntryStatus.1 = up(2)"
expect_eq "TimeTicks / DisplayString formatting" "$("${MCLI[@]}" "$TARGET" get swtEntryName.1 2>&1)" 'swtEntryName.1 = "eth0"'
expect_contains "walk shows names and enum labels" "$("${MCLI[@]}" "$TARGET" walk swtConnTable 2>&1)" \
    "swtConnState.10.0.0.1.80.119.101.98 = established(3)"
expect_eq "symbolic composite index" "$("${MCLI[@]}" "$TARGET" get "swtConnState.10.0.0.1.80.'web'" 2>&1)" \
    "swtConnState.10.0.0.1.80.119.101.98 = established(3)"
expect_eq "set with '=' (types from the MIB, enum label as value)" \
    "$("${MCLI[@]}" "$TARGET" set swtRowName.40 = cli swtRowStatus.40 = createAndGo 2>&1)" "OK"
expect_eq "  ... row created" "$("${MCLI[@]}" "$TARGET" get swtRowStatus.40 2>&1)" "swtRowStatus.40 = active(1)"
expect_eq "  ... and destroyed by label" "$("${MCLI[@]}" "$TARGET" set swtRowStatus.40 = destroy 2>&1)" "OK"
expect_contains "MIB range checked before sending" "$("${MCLI[@]}" "$TARGET" set swtLimit.0 = 500 2>&1)" "must be in 1..100"
expect_contains "unknown name is reported" "$("${MCLI[@]}" "$TARGET" get swtNoSuchThing.0 2>&1)" "unknown MIB object"

echo "== SNMPv3 failures are reported as transport errors"
expect_contains "wrong authentication passphrase" \
    "$("$CLIENT_BIN" -v 3 -u authuser -l authNoPriv -a SHA -A wrongpass999 -t 800 -r 0 "$TARGET" get $ROOTN.1.1.0 2>&1)" "transport error"
expect_contains "unknown user" \
    "$("$CLIENT_BIN" -v 3 -u nobody -l noAuthNoPriv -t 800 -r 0 "$TARGET" get $ROOTN.1.1.0 2>&1)" "transport error"
expect_contains "security level above what the user has" \
    "$("$CLIENT_BIN" -v 3 -u noauth -l authPriv -a SHA -A $AUTHPASS -x AES -X $PRIVPASS -t 800 -r 0 "$TARGET" get $ROOTN.1.1.0 2>&1)" "transport error"
expect_contains "wrong community (v2c)" \
    "$("$CLIENT_BIN" -v 2c -c nope -t 500 -r 0 "$TARGET" get $ROOTN.1.1.0 2>&1)" "timed out"
expect_contains "read-only community cannot SET" \
    "$("$CLIENT_BIN" -v 2c -c public "$TARGET" set $ROOTN.1.4.0 i 11 2>&1)" "agent error"
expect_contains "timeout is reported" \
    "$("$CLIENT_BIN" -c public -t 500 -r 0 127.0.0.1:$((PORT + 50)) get $ROOTN.1.1.0 2>&1)" "timed out"

# optional extra program (e.g. the generated typed client); it prints "  ok ..." / "  FAIL ..." lines
if [ -n "${EXTRA_CHECK:-}" ]; then
    echo "== extra check: $(basename "$EXTRA_CHECK")"
    extra=$("$EXTRA_CHECK" "$TARGET" 2>&1)
    printf '%s\n' "$extra"
    fails=$((fails + $(printf '%s\n' "$extra" | grep -c '  FAIL ')))
    printf '%s\n' "$extra" | grep -q '  ok ' || { echo "  FAIL extra check produced no results"; fails=$((fails + 1)); }
fi

echo "== notifications"
for _ in $(seq 1 40); do
    grep -q "SNMP v1" "$WORK/traps.log" 2>/dev/null && grep -q "snmpTrapOID" "$WORK/traps.log" 2>/dev/null && break
    sleep 0.25
done
v2trap=$(grep -c "99999.3.0.1" "$WORK/traps.log" 2>/dev/null)
[ "${v2trap:-0}" -ge 1 ] && pass "SNMPv2c notification swtAlarm received (trap2sink)" \
    || fail "SNMPv2c notification swtAlarm received" "traps.log:" "$(cat "$WORK/traps.log" 2>/dev/null)"
grep -q 'hello world\|snmpwrap' "$WORK/traps.log" && pass "notification carries the varbinds" || fail "notification varbinds" "$(cat "$WORK/traps.log")"
grep -q "SNMP v1" "$WORK/traps.log" && pass "SNMPv1 trap received (trapsink)" \
    || fail "SNMPv1 trap received" "$(cat "$WORK/traps.log" 2>/dev/null)"

echo "== shutdown"
kill -TERM "$AGENT_PID" 2>/dev/null
for _ in $(seq 1 20); do kill -0 "$AGENT_PID" 2>/dev/null || break; sleep 0.25; done
kill -0 "$AGENT_PID" 2>/dev/null && fail "agent stops on SIGTERM" || pass "agent stops on SIGTERM"

if [ "$fails" -ne 0 ]; then
    echo "$fails check(s) failed. agent output:"; cat "$WORK/agent.out"
    echo "snmpd log:"; cat "$WORK/snmpd.log" "$WORK/snmpd.out" 2>/dev/null | head -30
    KEEP_WORK=1; echo "work dir kept: $WORK"
    exit 1
fi
[ "$skipped" = 1 ] && echo "all integration checks passed (v3 authPriv SKIPPED, see above)" || echo "all integration checks passed"
