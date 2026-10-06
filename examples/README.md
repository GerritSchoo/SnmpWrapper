# Examples

| Program | What it shows |
|---|---|
| [apps/agent_app](apps/agent_app) | An application that publishes its data via SNMP: fills the generated `Data` of [MY-APP-MIB](apps/mibs/MY-APP-MIB.txt), serves it with `DataAgent`, reacts to changes (`onSet`), updates values from its own thread and sends a notification. |
| [apps/client_app](apps/client_app) | An application that reads and changes that data with the generated `Remote`: scalars, a table, enum values, and the three kinds of errors. |
| [client_cli.cpp](client_cli.cpp) | Command line tool (`get`, `getnext`, `getbulk`, `walk`, `set`) for v1, v2c and v3 with plain OIDs; with `-m <mib>` names and MIB-typed values. |

`agent_app` and `client_app` are **stand-alone CMake projects** – copy one as the skeleton of your own program. They also
build as part of snmpwrap (binaries in `build/<preset>/examples/apps/…`), and the test `apps` runs them against a private snmpd.

## Run them

```sh
scripts/demo.sh                     # starts snmpd, agent_app and client_app; Enter stops everything
```

or by hand, in three terminals (see [docs/GUIDE.md](../docs/GUIDE.md#3-step-by-step-an-agent-and-a-client)):

```sh
snmpd -f -C -c snmpd.conf -Lo                                        # with 'master agentx', 'agentXSocket tcp:127.0.0.1:705'
build/debug/examples/apps/agent_app/agent_app tcp:127.0.0.1:705
build/debug/examples/apps/client_app/client_app 127.0.0.1:161 private
```

## Build them on their own (against an installed snmpwrap)

```sh
cmake --install build/debug --prefix $HOME/snmpwrap-install
cmake -S examples/apps/agent_app -B build-agent -DCMAKE_PREFIX_PATH="$HOME/snmpwrap-install;$HOME/netsnmp"
cmake --build build-agent
```
