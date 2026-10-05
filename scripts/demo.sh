#!/usr/bin/env bash
# Starts a private snmpd (no root needed, UDP 127.0.0.1:11161, AgentX tcp:127.0.0.1:11162),
# an agent, and runs a client against it; stops everything at the end.
#
#   scripts/demo.sh [agent] [client] [preset]
#     agent:  mib_agent (default) | simple_agent | test_agent
#     client: mib_client (default) | simple_client | none
#
# While it runs you can query the agent from a second terminal, e.g.
#   snmpwalk -v2c -c public 127.0.0.1:11161 .1.3.6.1.4.1.99999
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
AGENT=${1:-mib_agent}
CLIENT=${2:-mib_client}
PRESET=${3:-debug}
BIN="$SNMPWRAP_ROOT/build/$PRESET/examples"
[ -x "$BIN/$AGENT" ] || { echo "$BIN/$AGENT not found - run scripts/build.sh $PRESET first"; exit 1; }

WORK=$(mktemp -d)
PIDS=()
cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

cat > "$WORK/snmpd.conf" <<EOF
agentaddress udp:127.0.0.1:11161
rocommunity public 127.0.0.1
rwcommunity private 127.0.0.1
createUser admin SHA authpass123
rwuser admin auth
master agentx
agentXSocket tcp:127.0.0.1:11162
trap2sink 127.0.0.1:11163 public
EOF
echo "authCommunity log public" > "$WORK/snmptrapd.conf"

export SNMPCONFPATH="$WORK/none" SNMP_PERSISTENT_DIR="$WORK/persist-tools"
snmptrapd -f -C -c "$WORK/snmptrapd.conf" -Lf "$WORK/traps.log" udp:127.0.0.1:11163 &
PIDS+=($!)
SNMP_PERSISTENT_DIR="$WORK/persist-snmpd" snmpd -f -C -r -c "$WORK/snmpd.conf" -Lf "$WORK/snmpd.log" &
PIDS+=($!)
sleep 1

AGENT_ARGS=(tcp:127.0.0.1:11162)
[ "$AGENT" = test_agent ] && AGENT_ARGS=(--socket tcp:127.0.0.1:11162 --trap-every 10)
"$BIN/$AGENT" "${AGENT_ARGS[@]}" &
PIDS+=($!)
sleep 1.5

echo "snmpd on udp:127.0.0.1:11161 (communities public / private, SNMPv3 user admin / authpass123)"
if [ "$CLIENT" != none ]; then
    echo "################ $CLIENT"
    "$BIN/$CLIENT" 127.0.0.1:11161
fi
echo
echo "Agent keeps running - query it from another terminal, e.g.:"
echo "  snmpwalk -v2c -c public -M +$SNMPWRAP_ROOT/mibs -m ALL 127.0.0.1:11161 .1.3.6.1.4.1.99999"
echo "Press Enter to stop (received notifications are shown then)."
read -r _
echo "################ notifications received by snmptrapd"
cat "$WORK/traps.log" 2>/dev/null | grep -i trap || echo "(none)"
