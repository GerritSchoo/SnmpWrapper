#!/usr/bin/env bash
# End-to-end test of the example applications: unprivileged snmpd + examples/apps/agent_app + client_app.
#
#   run_apps.sh <agent_app> <client_app> <net-snmp prefix>
set -u

AGENT_BIN=$1
CLIENT_BIN=$2
PREFIX=$3
export PATH="$PREFIX/bin:$PREFIX/sbin:$PATH"
export LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
for tool in snmpd snmpget; do
    command -v "$tool" >/dev/null || { echo "SKIP: $tool not found"; exit 77; }
done

PORT=${SNMPWRAP_APPS_PORT:-11171}
XPORT=$((PORT + 1))
WORK=$(mktemp -d)
PIDS=()
cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

cat > "$WORK/snmpd.conf" <<CONF
agentaddress udp:127.0.0.1:$PORT
rocommunity public 127.0.0.1
rwcommunity private 127.0.0.1
master agentx
agentXSocket tcp:127.0.0.1:$XPORT
CONF
export SNMPCONFPATH="$WORK/none" SNMP_PERSISTENT_DIR="$WORK/persist"
snmpd -f -C -r -c "$WORK/snmpd.conf" -Lf "$WORK/snmpd.log" >/dev/null 2>&1 &
PIDS+=($!)
TARGET=127.0.0.1:$PORT

"$AGENT_BIN" "tcp:127.0.0.1:$XPORT" >"$WORK/agent.out" 2>&1 &
PIDS+=($!)
ready() { snmpget -v2c -c public -t 1 -r 0 -Oqv "$TARGET" .1.3.6.1.4.1.99999.200.1.0 2>/dev/null | grep -qv -E 'No Such|^$'; }
for _ in $(seq 1 100); do ready && break; sleep 0.2; done
ready || { echo "agent_app did not register"; cat "$WORK/agent.out"; exit 1; }

fails=0
check() { case "$2" in *"$3"*) echo "  ok   $1" ;; *) echo "  FAIL $1 (expected: $3)"; fails=$((fails + 1)) ;; esac; }

out=$("$CLIENT_BIN" "$TARGET" private 2>&1); rc=$?
printf '%s\n' "$out"
[ $rc -eq 0 ] && echo "  ok   client_app exit code 0" || { echo "  FAIL client_app exit code $rc"; fails=$((fails + 1)); }
check "GET scalar"                  "$out" "appName  = my-app"
check "table read"                  "$out" "sensor 2: board"
check "row access"                  "$out" "sensor 1 is cpu"
check "read() into Data"            "$out" "snapshot: my-app, limit 30, 3 sensors"
check "SET scalar in a group"       "$out" "appLimit is now 35"
check "SET enum cell"               "$out" "sensor 3 is now on"
check "MIB check before sending"    "$out" "rejected before sending"
check "agent's own rule (onSet)"    "$out" "agent refused"
sleep 0.5
check "agent saw the change"        "$(cat "$WORK/agent.out")" "appLimit was changed by a manager"
check "no invalid initial values"   "$(grep -c 'invalid value' "$WORK/agent.out")" "0"

echo "$fails failure(s)"
[ $fails -eq 0 ]
