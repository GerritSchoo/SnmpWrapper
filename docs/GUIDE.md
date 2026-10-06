# snmpwrap – User Guide

snmpwrap is a C++17 library on top of **Net-SNMP 5.9**. You describe your data once, in a MIB file; the build turns
it into C++ types. Your **agent** fills a plain C++ structure and publishes it via SNMP; your **client** reads and
changes that data on a remote device with the same nesting. No OIDs, Net-SNMP structures or callbacks per object in
your code.

```
                    MY-APP-MIB.txt  (the single source of truth)
                          │  build time: snmpwrap_add_mib()
                          ▼
                    my_app_mib.hpp / .cpp
                 ┌────────┴─────────┐
   agent:  Data + DataAgent    client:  Remote
   data.appSensors.appLimit = 30;      remote.appSensors.appLimit.set(35);
```

The complete, tested example is [examples/apps](../examples/apps): `agent_app` and `client_app`.

---

## Contents

1. [How it works](#1-how-it-works)
2. [Adding snmpwrap to your project](#2-adding-snmpwrap-to-your-project)
3. [Step by step: an agent and a client](#3-step-by-step-an-agent-and-a-client)
4. [The agent: `Data` and `DataAgent`](#4-the-agent-data-and-dataagent)
5. [The client: `Remote`](#5-the-client-remote)
6. [Notifications](#6-notifications)
7. [Main loop, threads and shutdown](#7-main-loop-threads-and-shutdown)
8. [snmpd configuration and SNMP versions](#8-snmpd-configuration-and-snmp-versions)
9. [Troubleshooting](#9-troubleshooting)
10. [Error codes](#10-error-codes)
- [Appendix A: without a MIB (core API)](#appendix-a-without-a-mib-core-api)

---

## 1. How it works

The agent is an **AgentX subagent**. The standard Net-SNMP daemon `snmpd` (the *master agent*) listens on UDP 161,
handles SNMP versions, communities, SNMPv3 users and access control, and forwards the requests for your OIDs to your
process:

```
 SNMP manager                snmpd (master agent)                 your process
 (snmpget, NMS, client_app)                                       ┌──────────────────────────┐
        │  SNMP v1/v2c/v3 (UDP 161)     │     AgentX (TCP or      │ snmpwrap::Agent          │
        ├──────────────────────────────►│     Unix socket)        │   └─ DataAgent           │
        │◄──────────────────────────────┤◄───────────────────────►│        └─ your Data      │
                                                                  └──────────────────────────┘
```

Your code only provides values; snmpd does all protocol work, and the standard MIBs (system, interfaces, …) keep
working next to your subtree. The client side needs no snmpd: it talks to any SNMP agent directly.

What the build generates from the MIB (namespace = module name in snake_case, e.g. `my_app_mib`):

| MIB | Agent side | Client side |
|---|---|---|
| module | `struct Data` | `class Remote` |
| group (OBJECT IDENTIFIER) | nested struct: `data.appSensors` | nested: `remote.appSensors` |
| scalar | member: `data.appName` | `remote.appName.get()` / `.set(v)` |
| table | rows by index: `data.appSensors.appSensorTable[1]` | `remote.appSensors.appSensorTable[1]`, `.read()` |
| column | member of the row: `[1].appSensorName` | `[1].appSensorName.get()` / `.set(v)` |
| named numbers | `enum class AppSensorMode { on, off }`, `toString()` | same |
| notification | `sendAppLimitExceeded(agent, …)` | – |

Names are the **full MIB names**. SMI requires them to be unique in a module, so they never clash; C++ keywords get a
trailing `_`.

---

## 2. Adding snmpwrap to your project

### Requirements

* Linux (tested on Ubuntu 22.04 / WSL2), a C++17 compiler (GCC 11), CMake ≥ 3.16
* Net-SNMP 5.9.x with development files and tools:

```sh
sudo apt install build-essential cmake libsnmp-dev snmpd snmp
```

### Build, test and install snmpwrap

```sh
cmake --preset debug && cmake --build --preset debug     # or: cmake -S . -B build && cmake --build build
ctest --preset debug                                     # unit tests + integration against a private snmpd
cmake --install build/debug --prefix $HOME/snmpwrap-install
```

Net-SNMP in a custom prefix: add it to `CMAKE_PREFIX_PATH` (CMake looks for `net-snmp-config`).

### Your CMakeLists.txt

The same file works for an agent and for a client; this is [examples/apps/agent_app/CMakeLists.txt](../examples/apps/agent_app/CMakeLists.txt):

```cmake
cmake_minimum_required(VERSION 3.16)
project(agent_app CXX)

find_package(snmpwrap REQUIRED)             # snmpwrap::snmpwrap and snmpwrap_add_mib()

add_library(my_app_mib STATIC)              # MIB -> C++ at build time, again whenever the MIB changes
snmpwrap_add_mib(my_app_mib MODULE MY-APP-MIB MIB ${CMAKE_CURRENT_SOURCE_DIR}/../mibs/MY-APP-MIB.txt)
# MIBs your MIB imports from (besides Net-SNMP's standard MIBs):  MIB_DIRS <dir>...

add_executable(agent_app main.cpp)
target_link_libraries(agent_app PRIVATE my_app_mib)   # brings snmpwrap::snmpwrap along
```

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/snmpwrap-install;$HOME/netsnmp"
cmake --build build
```

Instead of installing you can use `add_subdirectory(path/to/SnmpWrapper)`; `snmpwrap_add_mib()` is available then
as well. In your sources you include only the generated header (`#include "my_app_mib.hpp"`); Net-SNMP headers never
leak into your code.

**The MIB file:** the extension does not matter (`.txt`, `.mib`, none); what counts is the module name inside
(`MY-APP-MIB DEFINITIONS ::= BEGIN`). Use your own IANA enterprise number instead of the placeholder `99999`.

**MIBs that import from other MIBs** work as in any Net-SNMP tool – the MIB is read by Net-SNMP's own parser:

* Standard MIBs (`SNMPv2-SMI`, `SNMPv2-TC`, `IF-MIB`, …) are found in Net-SNMP's MIB directory automatically.
* Your own MIBs that the main MIB imports from (shared types, a common root) go into a directory you pass with
  `MIB_DIRS` – Net-SNMP finds them there by their module name, whatever the file is called:
  `snmpwrap_add_mib(my_app_mib MODULE MY-APP-MIB MIB mibs/MY-APP-MIB.txt MIB_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/mibs)`
* Types defined elsewhere (TEXTUAL-CONVENTIONs) are resolved completely: their ranges and SIZEs are checked, their named
  numbers become an `enum class` named after the type (e.g. `SensorKind`, `TruthValue`).
* Generated are the objects of the module named in `MODULE`. Objects of the imported MIBs are not part of its `Data`;
  generate a second library with `snmpwrap_add_mib` for every module whose objects you serve.

---

## 3. Step by step: an agent and a client

**1. Describe the data** – [examples/apps/mibs/MY-APP-MIB.txt](../examples/apps/mibs/MY-APP-MIB.txt):

```
myAppMIB                        enterprises.99999.200
├── appName                     DisplayString (SIZE (1..32)), read-write
└── appSensors                  group
    ├── appLimit                Integer32 (0..100), read-write
    └── appSensorTable          INDEX { appSensorIndex }
        ├── appSensorName       read-only
        ├── appSensorTemperature read-only
        └── appSensorMode       INTEGER { on(1), off(2) }, read-write
appLimitExceeded                NOTIFICATION-TYPE OBJECTS { appSensorName, appSensorTemperature }
```

**2. CMake** – as in [section 2](#your-cmakeliststxt).

**3. The agent** – [examples/apps/agent_app/main.cpp](../examples/apps/agent_app/main.cpp), the essential lines:

```cpp
#include "my_app_mib.hpp"
namespace mib = my_app_mib;

snmpwrap::AgentConfig config;
config.agentxSocket = "tcp:127.0.0.1:705";             // must match 'agentXSocket' in snmpd.conf
snmpwrap::Agent agent(config);

mib::Data data;                                         // plain C++, nested like the MIB
data.appName = "my-app";
data.appSensors.appLimit = 30;
data.appSensors.appSensorTable[1] = {"cpu", 20, mib::AppSensorMode::on};

mib::DataAgent adapter(agent, data);                    // publishes every object of the MIB
adapter.onSet([&](const std::string& object, const snmpwrap::Oid& index) {
    std::cout << object << " was changed by a manager\n";
});

while (!g_stop && agent.poll()) {                       // answers requests, returns at least once per second
    auto guard = adapter.lock();                        // only needed if other threads use `data`
    data.appSensors.appSensorTable[1].appSensorTemperature = readSensor();
}
```

**4. The client** – [examples/apps/client_app/main.cpp](../examples/apps/client_app/main.cpp):

```cpp
#include "my_app_mib.hpp"
namespace mib = my_app_mib;

snmpwrap::SessionConfig cfg;
cfg.peer = "127.0.0.1:161";
cfg.community = "private";
snmpwrap::Client session(cfg);
mib::Remote remote(session);

std::cout << remote.appName.get() << "\n";
remote.appSensors.appLimit.set(35);
for (const auto& [index, row] : remote.appSensors.appSensorTable.read())
    std::cout << index.appSensorIndex << ": " << row.appSensorName << " " << row.appSensorTemperature << " C\n";
remote.appSensors.appSensorTable[3].appSensorMode.set(mib::AppSensorMode::on);
```

**5. Run it** – three terminals, or `scripts/demo.sh` which does all of it:

```sh
snmpd -f -C -c snmpd.conf -Lo                                   # see section 8 for snmpd.conf
build/debug/examples/apps/agent_app/agent_app tcp:127.0.0.1:705
build/debug/examples/apps/client_app/client_app 127.0.0.1:161 private
```

The agent must print `NET-SNMP version 5.9.1 AgentX subagent connected` – then it is registered at snmpd.
Plain Net-SNMP tools work as well: `snmpwalk -v2c -c public -M +examples/apps/mibs -m +MY-APP-MIB localhost MY-APP-MIB::myAppMIB`.

---

## 4. The agent: `Data` and `DataAgent`

### Filling the data

`Data` is an ordinary value type – you can create, copy and change it without any agent:

```cpp
mib::Data data;
data.appName = "my-app";                                              // scalar
data.appSensors.appSensorTable[1].appSensorName = "cpu";              // table[number] creates the row
data.appSensors.appSensorTable[2] = {"board", 20, mib::AppSensorMode::on};   // whole row (columns in MIB order)
data.appSensors.appSensorTable.erase(mib::AppSensorEntryIndex{2});    // remove a row
```

Tables are `std::map`s ordered like SNMP orders the rows. A table with a composite index takes the index struct:
`table[{3, "port"}]` or `table[mib::XyzEntryIndex{3, "port"}]`.

### What the adapter does

`mib::DataAgent adapter(agent, data)` registers every object of the MIB. Then:

* **GET / GETNEXT / walk** read `data`.
* **SET** from a manager is checked against the MIB first – type, range, SIZE, named numbers – and only then written
  into `data`. Several values in one request are all-or-nothing. Read-only objects cannot be set.
* **RowStatus tables** (if the MIB has them): managers create rows with createAndGo / createAndWait and delete them with
  destroy; the rows appear in / disappear from the table in `data`. A row created by a manager stays `notReady` until all
  required columns were supplied.

`data` and the agent must live longer than the adapter.

### Reacting to changes

| Hook | Called | Use it to |
|---|---|---|
| `adapter.onSet(f)` | after a manager wrote a value (MIB object name, row index or empty), the new value is already in `data` | react to a change; **throw `snmpwrap::SetError` to refuse it** – the old value comes back and the manager gets the error |
| `adapter.onGet(f)` | before a manager reads a value | values computed on demand (counters, uptime, live readings): write them into `data` |

```cpp
adapter.onSet([&](const std::string& object, const snmpwrap::Oid& index) {
    if (object == "appLimit" && data.appSensors.appLimit < 10)          // an extra rule on top of the MIB
        throw snmpwrap::SetError(snmpwrap::ErrorStatus::WrongValue, "limit below 10 is not allowed");
});
```

Both hooks run in the agent loop with the adapter's mutex held: read and write `data` directly, never call
`adapter.lock()` inside them. `onSet` runs per value; a request with several values may still fail later and be rolled
back. For work that must not be undone (switching hardware, …) put an entry into a queue in the hook and handle it in
your main loop after `poll()`. The destruction of a row cannot be refused.

### Checking values you set yourself

Assigning a member is plain C++ and **not** checked. `data.validate()` checks every value against the MIB and returns
one text per violation – call it after filling the structure:

```cpp
for (const std::string& problem : data.validate())
    std::cerr << problem << "\n";      // e.g. "appSensors.appLimit: value must be in 0..100"
```

It also finds values you forgot: an empty `appName` violates `SIZE (1..32)`.

### Limits

* SNMP has no floating point type: use `Integer32` with an agreed scale (e.g. tenths of a degree) or a string.
* All rows are kept in memory.
* Values that change on every read need `onGet`; the structure only holds what you put into it.

---

## 5. The client: `Remote`

`mib::Remote remote(session)` mirrors `Data` for a device on the network. Nothing is sent until you call `get()`,
`set()`, `read()`, `create()` or `destroy()`; every call is one SNMP request.

| Call | Does |
|---|---|
| `remote.appName.get()` | GET of a scalar, returns the C++ type (`std::string`, `std::int32_t`, enum, …) |
| `remote.appName.set(v)` | SET; checked against the MIB **before** sending (`snmpwrap::SetError`) |
| `remote.appSensors.appSensorTable[1].appSensorMode.get()` / `.set(v)` | one cell |
| `remote.appSensors.appSensorTable[1].read()` | all columns of one row (`AppSensorEntry`) |
| `remote.appSensors.appSensorTable.read()` | the whole table, `std::map<Index, Entry>` |
| `table.create(index, values, activate)` / `table.destroy(index)` | RowStatus tables only |

`session` is a `snmpwrap::Client`; it must outlive `remote`. Use one session per thread.

### Connecting (v1, v2c, v3)

```cpp
snmpwrap::SessionConfig cfg;
cfg.peer = "192.168.1.10";                  // port 161 by default; "host:port" or "tcp:host:port"
cfg.community = "public";                   // v2c (default); cfg.version = SessionConfig::Version::V1 for v1
cfg.timeout = std::chrono::milliseconds(2000);
cfg.retries = 1;

// SNMPv3
cfg.version = snmpwrap::SessionConfig::Version::V3;
cfg.user = "admin";
cfg.securityLevel = snmpwrap::SessionConfig::SecurityLevel::AuthPriv;
cfg.authProtocol = snmpwrap::SessionConfig::AuthProtocol::SHA1;   cfg.authPassphrase = "authpass123";
cfg.privProtocol = snmpwrap::SessionConfig::PrivProtocol::AES128; cfg.privPassphrase = "privpass123";
// also: AuthProtocol MD5 / SHA224..SHA512 (need Net-SNMP with OpenSSL), PrivProtocol DES / AES192 / AES256
// (need --enable-blumenthal-aes); an unavailable protocol is reported as an error
```

### Errors

| Exception | Meaning |
|---|---|
| `snmpwrap::SetError` | the value violates the MIB – nothing was sent |
| `snmpwrap::ResponseError` | the agent refused (`status()`, e.g. `WrongValue`; `index()` = varbind, 1-based) |
| `snmpwrap::TransportError` | no answer (wrong address, firewall, **wrong community** – it is silently dropped), SNMPv3 failure |
| `snmpwrap::Error` | e.g. the object or row does not exist on the agent |

### Without a MIB

`snmpwrap::Client` alone works with plain OIDs on any device:
`session.get(oid)`, `session.getNext(oid)`, `session.getBulk(oids, 0, 20)`, `session.set(oid, value)`,
`session.set({{oid1, v1}, {oid2, v2}})` (one atomic request), `session.walk(root)` (GETBULK on v2c/v3). On v2c/v3 a missing
object is not an exception but a value: `vb.value.isException()`. The tool [examples/client_cli.cpp](../examples/client_cli.cpp)
shows all of it:

```sh
client_cli -v 2c -c public 192.168.1.10 walk 1.3.6.1.2.1.1
client_cli -v 3 -u admin -l authPriv -a SHA -A authpass123 -x AES -X privpass123 192.168.1.10 get 1.3.6.1.2.1.1.1.0
client_cli -m examples/apps/mibs/MY-APP-MIB.txt -c private localhost set appSensors.appLimit.0 = 40   # names from a MIB
```

---

## 6. Notifications

Each `NOTIFICATION-TYPE` of the MIB becomes a typed function; table columns in `OBJECTS` add the row index:

```cpp
mib::sendAppLimitExceeded(agent, row.appSensorName, row.appSensorTemperature, index);
```

`sysUpTime.0` and `snmpTrapOID.0` are added automatically. snmpd forwards the notification to every configured destination:

```
trapsink   192.168.1.50 public        # SNMPv1 trap (converted automatically)
trap2sink  192.168.1.50 public        # SNMPv2c trap
informsink 192.168.1.50 public        # SNMPv2c inform (acknowledged)
trapsess   -v3 -u trapuser -l authNoPriv -a SHA -A secret123 192.168.1.50
```

Call it from the agent loop (not from another thread); it is dropped while the agent is not connected to snmpd.

---

## 7. Main loop, threads and shutdown

```cpp
std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }              // the handler only sets a flag
// std::signal(SIGINT, onSignal); std::signal(SIGTERM, onSignal);

while (!g_stop && agent.poll()) {                  // waits for requests, at most about 1 s
    // your periodic work, sending notifications, ...
}
```

Rules (Net-SNMP keeps process-wide state):

* **One `Agent` per process.** `poll()`, the adapter's hooks and `send…()` belong to the thread running the loop.
* **Other threads** may change `data` only while holding `adapter.lock()` (a `std::unique_lock`, released at the end of its
  scope). Never keep it across `poll()`. They never call snmpwrap functions themselves – they set a flag or queue an event,
  and the loop sends the notification (see `agent_app`).
* **Agent and client in one program:** create the `Agent` **first** (its constructor throws otherwise), use clients only from
  the agent's thread, and never query your own agent's OIDs from that thread – the thread waits for an answer only it
  could give (timeout). An agent and a client are best two programs, as in `examples/apps`.
* `poll(false)` returns immediately, for integration into an existing event loop. `agent.stop()` ends `poll()` / `run()`
  from another thread.
* If snmpd restarts, the agent reconnects by itself (AgentX ping every `AgentConfig::pingIntervalSec` seconds).

---

## 8. snmpd configuration and SNMP versions

For the agent, SNMP versions are purely a matter of snmpd – your code is the same for all of them:

```
# /etc/snmp/snmpd.conf  (or a private file: snmpd -f -C -c my.conf, no root needed for ports > 1024)
master agentx
agentXSocket tcp:127.0.0.1:705            # = AgentConfig::agentxSocket

rocommunity public  192.168.1.0/24       # v1 / v2c read
rwcommunity private 127.0.0.1            # v1 / v2c read-write

createUser monitor SHA "authpass123"                     # v3, passphrases: at least 8 characters
createUser admin   SHA "authpass123" AES "privpass123"
rouser monitor auth
rwuser admin   priv
```

| Topic | SNMPv1 | SNMPv2c / v3 |
|---|---|---|
| missing object / instance | error `noSuchName` | `noSuchObject` / `noSuchInstance` per value |
| SET errors | mapped to `badValue`, `noSuchName`, `genErr` | full set (`wrongValue`, `notWritable`, …) |
| `Counter64` | invisible | normal |
| GETBULK | – | available |

---

## 9. Troubleshooting

**`No Such Object available on this agent at this OID`.** The agent is not registered: is it running and did it print
`AgentX subagent connected`? Does snmpd have `master agentx`, and does `agentXSocket` match `AgentConfig::agentxSocket`?
With the default Unix socket `/var/agentx/master` the agent needs permissions – use `tcp:127.0.0.1:705` on both sides.
Do the snmpd views include your subtree?

**`No Such Instance`.** The object exists, this instance does not: wrong row index, or a scalar without `.0`.

**SET answers `noAccess`.** The community / v3 user has no write access in snmpd.conf (`rwcommunity`). This comes from snmpd,
not from your code. **`notWritable`:** the object is read-only in the MIB. **`wrongType`:** the manager sent another type
(`snmpset … u 5` for an Integer32 – use `i`). **`wrongValue` / `wrongLength`:** outside the MIB's range / SIZE, or refused by
your `onSet`.

**Timeout in the client.** Wrong host/port, firewall, or a wrong community (silently dropped).

**`snmpwrap::Agent must be created before the first snmpwrap::Client`.** Create the Agent first ([section 7](#7-main-loop-threads-and-shutdown)).

**SNMPv3 with encryption fails.** Check with the Net-SNMP tools first (`snmpget -v3 -l authPriv …`); if they fail too, the
problem is the Net-SNMP build or the user configuration.

**`loading MIB failed: … At line N`.** Syntax error in the MIB; a frequent cause is `--` inside a comment line (ends the comment).
**`Cannot find module (XYZ-MIB)`.** An imported MIB is not on the search path: `MIB_DIRS` in `snmpwrap_add_mib`.
**Build errors after changing the MIB** are intended: the generated types changed – adapt the code the compiler points at.
Never edit generated files; they are overwritten.

**Two objects map to the same C++ member.** The generator stops and names both MIB objects; rename one in the MIB.

---

## 10. Error codes

Use them in `snmpwrap::SetError(snmpwrap::ErrorStatus::…, "reason")`; the numbers are the values on the wire.

| `ErrorStatus` | No. | When | v1 managers see |
|---|---|---|---|
| `WrongType` | 7 | wrong value type (checked automatically) | `badValue` |
| `WrongLength` | 8 | string too long / short (automatic from SIZE) | `badValue` |
| `WrongValue` | 10 | out of range, unknown named number, or your own rule | `badValue` |
| `NoCreation` | 11 | instance does not exist and cannot be created (automatic) | `noSuchName` |
| `InconsistentValue` | 12 | valid in general, but not right now | `badValue` |
| `ResourceUnavailable` | 13 | no memory / no free slot | `genErr` |
| `CommitFailed` | 14 | writing failed – the request is rolled back | `genErr` |
| `NotWritable` | 17 | read-only object (automatic) | `noSuchName` |
| `InconsistentName` | 18 | row does not exist and this request cannot create it (automatic) | `noSuchName` |
| `GenErr` | 5 | anything else | `genErr` |

---

## Appendix A: without a MIB (core API)

Everything above is built on a small, MIB-independent core that stays available for special cases – e.g. data whose OIDs
are only known at run time, or a proxy to another process. The reference is in the headers:

| Header | Contents |
|---|---|
| `snmpwrap/agent.hpp` | `Agent`, `AgentConfig` |
| `snmpwrap/mib.hpp` | `Mib` (scalars, tables, RowStatus by OID), `Handler`, `SetTransaction` |
| `snmpwrap/client.hpp` | `Client`, `SessionConfig` |
| `snmpwrap/oid.hpp`, `value.hpp`, `index.hpp`, `error.hpp` | `Oid`, `Value`/`Type`/`VarBind`, index encoding, exceptions |
| `snmpwrap/mib_model.hpp` | `MibModel` – reads MIB files (used by the generator and `client_cli -m`) |

```cpp
snmpwrap::Mib& mib = agent.addMib(snmpwrap::Oid::parse("1.3.6.1.4.1.99999"));
int limit = 50;
mib.scalar({1, 4}, {snmpwrap::Type::Integer,
    [&] { return snmpwrap::Value::integer(limit); },                       // get
    [&](const snmpwrap::Value& v) { limit = v.asInt(); },                  // set (optional)
    [](const snmpwrap::Value& v) {                                         // validate (optional)
        if (v.asInt() < 1 || v.asInt() > 100) throw snmpwrap::SetError(snmpwrap::ErrorStatus::WrongValue);
    }});
// tables: mib.table(rel, TableDef{...}); any data source: agent.addHandler(root, std::make_shared<MyHandler>())
```

`Mib` implements the SNMP semantics (GETNEXT order, atomic SET with rollback, RowStatus) for both paths – the generated
code uses exactly this layer. A complete agent written this way is [test/agents/test_agent.cpp](../test/agents/test_agent.cpp).
