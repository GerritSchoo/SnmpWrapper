#!/usr/bin/env bash
# Common environment for the scripts in this folder (sourced, not executed).
# - cmake / ninja installed with "pip install --user cmake ninja" live in ~/.local/bin
# - a Net-SNMP built from source lives in ~/netsnmp (otherwise the system one from apt is used)

export PATH="$HOME/.local/bin:$PATH"
if [ -x "$HOME/netsnmp/bin/net-snmp-config" ]; then
    export PATH="$HOME/netsnmp/bin:$HOME/netsnmp/sbin:$PATH"
fi

# repository root = parent of this folder
SNMPWRAP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export SNMPWRAP_ROOT
cd "$SNMPWRAP_ROOT" || exit 1

for tool in cmake ninja net-snmp-config; do
    command -v "$tool" >/dev/null || {
        echo "missing: $tool"
        echo "  cmake/ninja:  sudo apt install cmake ninja-build   (or: pip install --user cmake ninja)"
        echo "  net-snmp:     sudo apt install libsnmp-dev snmpd snmp"
        exit 1
    }
done
