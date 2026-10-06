#!/usr/bin/env bash
# Starts a private snmpd (no root needed, UDP 127.0.0.1:11161, AgentX tcp:127.0.0.1:11162), the example
# agent_app, runs client_app against it and keeps the agent running; stops everything at the end.
#
#   scripts/demo.sh [preset]          preset: debug (default), release, shared
#
# While it runs you can query the agent from a second terminal, e.g.
#   snmpwalk -v2c -c public -M +examples/apps/mibs -m +MY-APP-MIB 127.0.0.1:11161 MY-APP-MIB::myAppMIB
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
PRESET=${1:-debug}
APPS="$SNMPWRAP_ROOT/build/$PRESET/examples/apps"
AGENT="$APPS/agent_app/agent_app"
CLIENT="$APPS/client_app/client_app"
[ -x "$AGENT" ] && [ -x "$CLIENT" ] || { echo "agent_app / client_app not found - run scripts/build.sh $PRESET first"; exit 1; }

WORK=$(mktemp -d)
PIDS=()
cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

cat > "$WORK/snmpd.conf" <<EOF2
agentaddress udp:127.0.0.1:11161
rocommunity public 127.0.0.1
rwcommunity private 127.0.0.1
createUser admin SHA authpass123
rwuser admin auth
master agentx
agentXSocket tcp:127.0.0.1:11162
trap2sink 127.0.0.1:11163 public
trap2sink 127.0.0.1:11164 public
EOF2
echo "authCommunity log public" > "$WORK/snmptrapd.conf"

export SNMPCONFPATH="$WORK/none" SNMP_PERSISTENT_DIR="$WORK/persist-tools"
snmptrapd -f -C -c "$WORK/snmptrapd.conf" -Lf "$WORK/traps.log" udp:127.0.0.1:11163 &
PIDS+=($!)
SNMP_PERSISTENT_DIR="$WORK/persist-snmpd" snmpd -f -C -r -c "$WORK/snmpd.conf" -Lf "$WORK/snmpd.log" &
PIDS+=($!)
sleep 1

"$AGENT" tcp:127.0.0.1:11162 &
PIDS+=($!)
sleep 1.5

echo "snmpd on udp:127.0.0.1:11161 (communities public / private, SNMPv3 user admin / authpass123)"
echo "################ client_app"
"$CLIENT" 127.0.0.1:11161 private udp:127.0.0.1:11164
echo
echo "agent_app keeps running - query it from another terminal, e.g.:"
echo "  snmpwalk -v2c -c public -M +$SNMPWRAP_ROOT/examples/apps/mibs -m +MY-APP-MIB 127.0.0.1:11161 MY-APP-MIB::myAppMIB"
echo "Press Enter to stop (received notifications are shown then)."
read -r _
echo "################ notifications received by snmptrapd"
grep -i -A1 trap "$WORK/traps.log" 2>/dev/null || echo "(none)"
