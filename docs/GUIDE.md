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

**Looking for what exists?** [Section 11](#11-everything-at-a-glance) lists every function of the agent and the client
side on one page, plus an "I want to …" table. The headers have the details of each one.

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
11. [Everything at a glance](#11-everything-at-a-glance)
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
| notification | `sendAppLimitExceeded(agent, …)` | `notifications.onAppLimitExceeded(…)` |
| the whole MIB | – | `remote.read()` → `Data`, `remote.send(data)` |

**Which direction?** The MIB decides, per object, with `MAX-ACCESS`:

| In the MIB | Information flows | Agent | Client |
|---|---|---|---|
| `read-only` (status, measurements) | agent → client, the client asks | writes it into `data` | `get()` only – `set()` does not exist (compile error) |
| `read-write`, `read-create` (settings, commands) | client → agent | receives it: `onSet` (check), messages (react) | `get()`, `set()`, `send()` |
| `NOTIFICATION-TYPE` (events) | agent → client, pushed | `send<Name>()` | `Notifications::on<Name>()` |

Names are the **full MIB names**. SMI requires them to be unique in a module, so they never clash; C++ keywords get a
trailing `_`.

---

## 2. Adding snmpwrap to your project

### Requirements

* Linux (tested on Ubuntu 22.04 / WSL2), a C++17 compiler (GCC 11), CMake ≥ 3.16
* Net-SNMP 5.9.x with development files and tools:

```sh
sudo apt install build-essential cmake libsnmp-dev snmpd snmp
sudo apt install libgtest-dev libgmock-dev    # only for snmpwrap's own tests (off: -DBUILD_TESTING=OFF)
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
adapter.onAppSensors([](const mib::Data::AppSensorsGroup& settings) {    // a manager changed this subtree
    std::cout << "new limit " << settings.appLimit << "\n";
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

mib::Data::AppSensorsGroup settings;                    // a whole subtree as one message
settings.appLimit = 25;
remote.appSensors.send(settings);

snmpwrap::NotificationReceiver receiver("udp:0.0.0.0:1162");   // events from the agent
mib::Notifications notifications(receiver);
notifications.onAppLimitExceeded([](const mib::AppLimitExceeded& n) { std::cout << n.appSensorName << " too warm\n"; });
receiver.poll();
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

`data` and the agent must live longer than the adapter. Destroying the adapter while the agent keeps running is fine:
its subtree disappears ([section 7](#ending-an-adapter-at-run-time)).

### Reacting to changes

A manager can change a single value, several values, a table row or a whole subtree in one request. The adapter reports
it on every level; pick the one that matches what belongs together in your MIB:

| Hook | Called | Use it to |
|---|---|---|
| `adapter.onSet(f)` | while the request is written, per value: `f(mib::Object, index)` | **check** a value: throw `snmpwrap::SetError` to refuse the whole request (the old values come back without calling `onSet` again) |
| `adapter.onChanged(f)` | **after the commit**, per changed value: `f(mib::Object, index)` | react to single values |
| `adapter.on<Table>Row(f)` | after the commit, per changed or created row: `f(index, row)` | a row is one message (e.g. one command) |
| `adapter.on<Group>(f)` | after the commit, once if anything in the subtree changed: `f(const Data::<Group>&)` | a subtree is one message (e.g. a settings block) |
| `adapter.on<Module>(f)` | after the commit, once if anything changed: `f(const Data&)` | the whole MIB |
| `adapter.onGet(f)` | before a manager reads a value: `f(mib::Object, index)` | values computed on demand (counters, uptime, live readings) |

```cpp
adapter.onSet([&](mib::Object object, const snmpwrap::Oid&) {                       // check
    if (object == mib::Object::appLimit && data.appSensors.appLimit < 10)
        throw snmpwrap::SetError(snmpwrap::ErrorStatus::WrongValue, "limit below 10 is not allowed");
});
adapter.onAppSensors([](const mib::Data::AppSensorsGroup& settings) {               // subtree message
    std::cout << "settings: limit " << settings.appLimit << "\n";
});
adapter.onAppSensorTableRow([](const mib::AppSensorEntryIndex& i, const mib::AppSensorEntry& row) {   // row message
    std::cout << "sensor " << i.appSensorIndex << " " << mib::toString(row.appSensorMode) << "\n";
});
adapter.onChanged([](mib::Object object, const snmpwrap::Oid& index) {              // every single value
    switch (object) {
        case mib::Object::appName: /* ... */ break;
        default: break;
    }
});
```

Rules:

* `mib::Object` names every object of the MIB – a `switch` instead of string comparisons, so a typo is a compile error.
* The message hooks run **once per request and only after it was committed**: a refused or rolled-back request
  reports nothing, and they see all values of the request at once.
* All hooks run in the agent loop with the adapter's mutex held: read and write `data` directly, never call
  `adapter.lock()` inside them. Exceptions from the message hooks are ignored (the request is already committed).
* Row creation through RowStatus is reported as the RowStatus object plus a row message; a destroyed row is reported as the
  RowStatus object and in its subtree, but not as a row.

### Which row? The index in the hooks

`onSet`, `onChanged` and `onGet` get the object and, for a table cell, the row index as an OID (empty for scalars).
Every table has an index struct that turns it back into values:

```cpp
adapter.onChanged([&](mib::Object object, const snmpwrap::Oid& index) {
    if (object == mib::Object::appSensorMode) {
        const auto row = mib::AppSensorEntryIndex::fromOid(index);   // std::optional: which sensor?
        std::cout << "sensor " << row->appSensorIndex << " is now "
                  << data.appSensors.appSensorTable.at(*row).appSensorMode << "\n";
    }
});
```

`XyzEntryIndex` has one member per INDEX column, `toOid()` / `fromOid()` (`std::nullopt` for a malformed OID) and SNMP
ordering. The row hooks (`on<Table>Row`) get the decoded index directly. The object OIDs themselves are in
`mib::oids::` (`mib::oids::appSensorMode`, `mib::oids::root`, …), e.g. to compare a notification's `trapOid`.

### Checking values you set yourself

Assigning a member is plain C++ and **not** checked. `data.validate()` checks every value against the MIB and returns
one text per violation – call it after filling the structure:

```cpp
for (const std::string& problem : data.validate())
    std::cerr << problem << "\n";      // e.g. "appSensors.appLimit: value must be in 0..100"
```

It also finds values you forgot: an empty `appName` violates `SIZE (1..32)`.

### Printing the data

`operator<<` writes the whole structure or any part of it – for logging and debugging. Values come in MIB order,
one `name = value` line each, nested groups and table rows indented:

```cpp
std::cout << data;                                    // everything
std::cout << data.appSensors;                         // one group
std::cout << data.appSensors.appSensorTable;          // one table: [index], then the columns
std::cout << data.appSensors.appSensorTable[1];       // one row
std::cout << index << " " << mib::AppSensorMode::on;  // a row index ("1"), an enum ("on(1)")
```

```
appName = "my-app"
appSensors
  appLimit = 30
  appSensorTable (2 rows)
    [1]
      appSensorName = "cpu"
      appSensorTemperature = 20
      appSensorMode = on(1)
    [2]
      ...
```

Enums show label and number, texts are quoted (UTF-8 is kept), strings with control characters and Opaque values are
written in hex (`0x0a1b`), BITS as the names of the set bits (`{red, blue}`), RowStatus as `active(1)` etc. The row
types and indexes have `operator<<` as well, so the maps returned by `Client` print the same way.

### Limits

* SNMP has no floating point type: use `Integer32` with an agreed scale (e.g. tenths of a degree) or a string.
* All rows are kept in memory.
* Values that change on every read need `onGet`; the structure only holds what you put into it.

### Data that does not live in memory: `Instrumentation`

`Data` + `DataAgent` keep every value in a structure. When the values live elsewhere – a database, hardware registers, a
table too big to copy – implement the generated interface `mib::Instrumentation` instead: one virtual function per
object, called for every request, and register it with `mib::registerMib(agent, impl)`:

```cpp
class SensorBackend : public mib::Instrumentation {
public:
    std::string appName() override { return name_; }
    void setAppName(const std::string& v) override { name_ = v; }       // the value is already checked against the MIB
    std::int32_t appLimit() override { return limit_; }
    void setAppLimit(std::int32_t v) override { limit_ = v; }
    void validateAppLimit(std::int32_t v) override {                     // optional extra rule before anything is written
        if (v < 10) throw snmpwrap::SetError(snmpwrap::ErrorStatus::WrongValue, "limit below 10");
    }
    std::vector<mib::AppSensorEntryIndex> appSensorTableRows() override { /* the row indexes, any order */ }
    std::string appSensorName(const mib::AppSensorEntryIndex& i) override { /* ... */ }
    std::int32_t appSensorTemperature(const mib::AppSensorEntryIndex& i) override { return readHardware(i.appSensorIndex); }
    mib::AppSensorMode appSensorMode(const mib::AppSensorEntryIndex& i) override { /* ... */ }
    void setAppSensorMode(const mib::AppSensorEntryIndex& i, mib::AppSensorMode v) override { /* ... */ }
};

SensorBackend backend;                                    // must outlive the agent
mib::registerMib(agent, backend);                         // or mib::bind(someMib, backend)
```

* Per object: a getter, a setter for writable objects and an optional `validate<Object>()`. Throw `snmpwrap::SetError`
  from a setter or validator to refuse the request; it is rolled back as a whole.
* Per table: `<table>Rows()`. `<table>Next(after)` and `<table>Has(index)` have default implementations that scan
  `<table>Rows()` – override them for big tables, so a walk does not enumerate the whole table for every step.
* RowStatus tables add `create<Entry>(index, values)`, `destroy<Entry>(index)`, the RowStatus getter / setter and an
  optional `<entry>Complete(index)`.
* There are no hooks and no messages: your setters see every change directly. `Data` + `DataAgent` is itself just one
  implementation of this interface.

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
| `remote.read()` | **everything** in one walk, as the same `Data` structure the agent uses |
| `remote.appSensors.send(msg)` | a subtree as one message: all its writable values in **one** request (all or nothing) |
| `remote.appSensors.appSensorTable[1].send(row)` | a row as one message |
| `remote.send(data)` | all writable values of the MIB in one request |
| `remote.change().set(remote.a, 1).set(remote.t[2].c, 5).send()` | any selection of values in one request |
| `table.create(index, values, activate)` / `table.destroy(index)` | RowStatus tables only |

`session` is a `snmpwrap::Client`; it must outlive `remote`. Use one session per thread. One request with several
values is atomic at the agent: if one value is refused, none is written, and `ResponseError::index()` names the refused one.

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

### Under `Remote`: the flat `mib::Client`

`Remote` is built on a flat typed class, `mib::Client`: one function per object, e.g. `appName()`, `setAppName(v)`,
`appSensorTable()` (the whole table as a map), `appSensorMode(index)`, `setAppSensorMode(index, v)`. Its `make<Object>(v)`
returns the checked `snmpwrap::VarBind` without sending anything – the same thing `remote.appName.varbind(v)` does, for
building requests yourself. Use whichever reads better; both check against the MIB.

### Without a MIB

`snmpwrap::Client` alone works with plain OIDs on any device:
`session.get(oid)`, `session.get({oid1, oid2})`, `session.getNext(oid)`, `session.getBulk(oids, 0, 20)`, `session.set(oid, value)`,
`session.set({{oid1, v1}, {oid2, v2}})` (one atomic request), `session.walk(root)` (GETBULK on v2c/v3) and
`session.walk(root, [](const snmpwrap::VarBind& vb) { ...; return true; })` (return `false` to stop early). Values are made
with `snmpwrap::Value::integer(5)`, `::string("x")`, `::gauge(7)`, … and printed with `value.str()`. On v2c/v3 a missing
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

### Receiving them (client side)

```cpp
snmpwrap::NotificationReceiver receiver("udp:0.0.0.0:1162");    // where snmpd sends them (port 162 needs root)
receiver.acceptCommunities({"public"});                         // the community of trap2sink / informsink
mib::Notifications notifications(receiver);                     // generated: typed handlers
notifications.onAppLimitExceeded([](const mib::AppLimitExceeded& n) {
    std::cout << n.appSensorName << " " << n.appSensorTemperature << " C, sensor " << n.appSensorEntryIndex.appSensorIndex
              << " (from " << n.raw.source << ")\n";
});
notifications.onOther([](const snmpwrap::Notification& n) { /* notifications of other MIBs */ });

while (running) receiver.poll();                                // or receiver.run() until receiver.stop()
```

In snmpd.conf: `trap2sink <address> public` (SNMPv2c), `trapsink` (SNMPv1, converted back automatically) or
`informsink` (acknowledged by the receiver). Every notification has `raw` with sender, community, version and all varbinds.
Without `acceptCommunities(...)` the receiver takes notifications from anyone who can reach its address; with it, others
are dropped (and not acknowledged).
SNMPv3 notifications are not supported by the receiver. `snmpwrap::NotificationReceiver` alone works without a MIB
(`onNotification(...)` with `trapOid` and varbinds).

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

`agent.poll()` connects on its first call (or call `agent.start()` yourself); `agent.run()` is the same loop without your
own code, until `agent.stop()`. The settings of the agent:

| `snmpwrap::AgentConfig` | Default | Meaning |
|---|---|---|
| `name` | `"snmpwrap-agent"` | name in Net-SNMP's log messages; Net-SNMP also reads `<name>.conf` if it exists |
| `agentxSocket` | `"tcp:127.0.0.1:705"` | where snmpd listens for subagents – must match `agentXSocket` in snmpd.conf |
| `pingIntervalSec` | `15` | AgentX ping; after a snmpd restart the agent reconnects within this time (0 = off) |
| `logToStderr` | `true` | Net-SNMP's messages (e.g. "AgentX subagent connected") on stderr |
| `ignoreSigpipe` | `true` | ignore SIGPIPE, so that a vanishing snmpd cannot kill the process (process-wide) |

Rules (Net-SNMP keeps process-wide state):

* **One `Agent` per process.** `poll()`, the adapter's hooks and `send…()` belong to the thread running the loop.
* **Other threads** may change `data` only while holding `adapter.lock()` (a `std::unique_lock`, released at the end of its
  scope). Never keep it across `poll()`. They never call snmpwrap functions themselves – they set a flag or queue an event,
  and the loop sends the notification (see `agent_app`).
* **Agent and client in one program** works, with a few rules – see [below](#agent-and-client-in-one-program).
* `poll(false)` returns immediately, for integration into an existing event loop. `agent.stop()` ends `poll()` / `run()`
  from another thread.
* If snmpd restarts, the agent reconnects by itself (AgentX ping every `AgentConfig::pingIntervalSec` seconds).

### Ending an adapter at run time

Reconfiguration, a module that is switched off: destroy the `DataAgent` on the loop thread and keep polling. Its subtree
disappears from snmpd (managers get noSuchObject), and nothing touches its `data` any more – a SET that is still in
progress then fails cleanly. The `Data` and the `Agent` must outlive the adapter; a new `DataAgent` for the same MIB can
follow right away. Without the generated code: `agent.removeMib(mib)` / `agent.removeHandler(root)`.

### Agent and client in one program

A typical case is a gateway: it reads values from other devices and serves them itself.

```cpp
snmpwrap::Agent agent;                          // 1. the Agent first
mib::Data data;
mib::DataAgent adapter(agent, data);

snmpwrap::SessionConfig cfg;                    // 2. then clients - to OTHER devices
cfg.peer = "192.168.1.20";
cfg.timeout = std::chrono::milliseconds(500);   // a dead device blocks the loop for timeout * (retries + 1)
cfg.retries = 0;
snmpwrap::Client device(cfg);
other_mib::Remote remote(device);

while (agent.poll()) {
    try {
        data.status.temperature = remote.sensors.temperature.get();   // serve what the device reported
    } catch (const snmpwrap::TransportError&) {
        // device unreachable: keep the last value, keep answering our own managers
    }
}
```

1. **The `Agent` first**, before the first `Client` and the first `NotificationReceiver` – its constructor throws
   otherwise (Net-SNMP is initialized once per process, and only an agent can set it up as an AgentX subagent).
2. **One thread:** clients run in the thread that calls `poll()`.
3. **Ask other agents, never your own OIDs through snmpd:** the thread would wait for an answer only it could give (timeout).
4. **A client call blocks the loop** until the answer or the timeout – meanwhile the agent answers nobody. Keep `timeout`
   and `retries` small for clients in the agent loop.
5. **After `~Agent`** do not use the clients any more: its destructor shuts Net-SNMP down for the whole process.

When none of this fits, use two programs, as in `examples/apps`.

---

### Testing your application: the interfaces to mock

The network sits behind three small interfaces with pure virtual functions. Your code depends on them; your tests pass
mocks (written by hand or with gmock). The generated code only uses these interfaces:

| Interface (`snmpwrap::`) | Pure virtual functions | Real implementation | Used by |
|---|---|---|---|
| `Session` (`session.hpp`) | `get(oids)`, `getNext`, `getBulk`, `set(varbinds)`, `walk(root, callback)` | `Client` | `Remote` |
| `NotificationSender` (`notification.hpp`) | `sendTrap(trapOid, vars)` | `Agent` | `send<Name>()` |
| `NotificationSource` (`notification.hpp`) | `onNotification(handler)` | `NotificationReceiver` | `Notifications` |

```cpp
class MockSession : public snmpwrap::Session {          // e.g. with gmock
public:
    MOCK_METHOD(std::vector<snmpwrap::VarBind>, get, (const std::vector<snmpwrap::Oid>&), (override));
    MOCK_METHOD(snmpwrap::VarBind, getNext, (const snmpwrap::Oid&), (override));
    MOCK_METHOD(std::vector<snmpwrap::VarBind>, getBulk, (const std::vector<snmpwrap::Oid>&, int, int), (override));
    MOCK_METHOD(void, set, (const std::vector<snmpwrap::VarBind>&), (override));
    MOCK_METHOD(void, walk, (const snmpwrap::Oid&, const std::function<bool(const snmpwrap::VarBind&)>&), (override));
};

MockSession session;
mib::Remote remote(session);                            // the real generated code, on top of the mock
EXPECT_CALL(session, set(...));
remote.appSensors.appLimit.set(40);                     // MIB check, OID and value come from the generated code
```

The agent side needs no interface: `Data` is a plain struct, and `DataAgent(snmpwrap::Mib&, Data&)` works without an
`Agent` and without snmpd – drive it in a test through `mib.get()`, `mib.getNext()` and `mib.prepare()` (the same calls
the agent makes), and your hooks run exactly as in production. snmpwrap's own tests do both:
[test/generated/data_agent_test.cpp](../test/generated/data_agent_test.cpp) (agent side) and
[test/generated/remote_test.cpp](../test/generated/remote_test.cpp) (`Remote` on a gmock session).

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

## 11. Everything at a glance

`mib::` is the generated namespace (your module in snake_case); `<Object>`, `<Table>`, `<Group>` stand for the MIB names
(e.g. `appSensorMode`, `appSensorTable`, `appSensors`), `<Entry>` for the table's entry type (`AppSensorEntry`).

### I want to …

| … | Use |
|---|---|
| serve values that live in my program | `mib::Data` + `mib::DataAgent adapter(agent, data)` ([4](#4-the-agent-data-and-dataagent)) |
| serve values from a database / hardware / huge tables | implement `mib::Instrumentation`, `mib::registerMib(agent, impl)` ([4](#data-that-does-not-live-in-memory-instrumentation)) |
| compute a value only when it is read | `adapter.onGet(f)` |
| refuse a value a manager writes | `onSet(f)` or `validate<Object>()`: throw `snmpwrap::SetError` |
| react when a manager changed something | `onChanged`, `on<Table>Row`, `on<Group>`, `on<ModuleNode>` – after the commit |
| know which row a hook means | `mib::<Entry>Index::fromOid(index)` ([4](#which-row-the-index-in-the-hooks)) |
| check my own values against the MIB | `data.validate()` |
| log the data | `std::cout << data;` (or any group, table, row, index, enum) |
| change data from another thread | `auto guard = adapter.lock();` |
| switch a subtree off at run time | destroy the `DataAgent`, or `agent.removeMib(mib)` ([7](#ending-an-adapter-at-run-time)) |
| send an event | `mib::send<Notification>(agent, …)` |
| read one value from a device | `remote.<group>.<object>.get()` |
| read everything at once | `remote.read()` → `mib::Data` |
| set several values atomically | `remote.<group>.send(msg)`, `remote.<table>[i].send(row)`, `remote.change().set(…).set(…).send()` |
| create / delete a row on a device | `remote.<table>.create(index, values)` / `.destroy(index)` (RowStatus tables) |
| receive events | `snmpwrap::NotificationReceiver` + `mib::Notifications` ([6](#receiving-them-client-side)) |
| talk to a device without a MIB | `snmpwrap::Client`: `get`, `getNext`, `getBulk`, `set`, `walk` |
| test my code without network | mock `snmpwrap::Session` / `NotificationSender` / `NotificationSource` ([testing](#testing-your-application-the-interfaces-to-mock)) |
| be an agent and a client in one program | [section 7](#agent-and-client-in-one-program) |

### Agent side

| Class / function | What it does |
|---|---|
| `snmpwrap::Agent(AgentConfig)` | the AgentX subagent; one per process, created before any `Client` / `NotificationReceiver` |
| `agent.poll(block = true)` | answers pending requests, waits at most about 1 s; `false` once `stop()` was called |
| `agent.run()` / `agent.stop()` | loop until `stop()` (thread- and signal-safe) |
| `agent.start()` | connect to snmpd now (otherwise done by the first `poll()` / `run()` / `sendTrap()`) |
| `agent.addMib(root)` / `agent.addHandler(root, handler)` | register a subtree (core API, [appendix](#appendix-a-without-a-mib-core-api)) |
| `agent.removeMib(mib)` / `agent.removeHandler(root)` | remove a subtree at run time: noSuchObject from then on |
| `agent.sendTrap(trapOid, varbinds)` | a notification by OID (what `send<Notification>()` calls) |
| `mib::Data` | all values, nested like the MIB; tables are `std::map`s: `table[index]` (or a plain number) creates a row, `erase()` removes it |
| `data.validate()` | every value against the MIB: one text per violation |
| `operator<<` | `data`, groups, tables, rows, indexes, enums as text |
| `mib::DataAgent(agent, data)` | serves `data`; destroying it removes the subtree again |
| `mib::DataAgent(mib, data)` | the same on an existing `snmpwrap::Mib` (tests, custom roots) |
| `adapter.lock()` | `std::unique_lock` for other threads that touch `data` |
| `adapter.onSet(f)` / `onGet(f)` | `f(mib::Object, index)`: before writing (may refuse) / before reading |
| `adapter.onChanged(f)` | `f(mib::Object, index)` per changed value, after the commit |
| `adapter.on<Table>Row(f)` | `f(index, row)` per changed or created row, after the commit |
| `adapter.on<Group>(f)` / `on<ModuleNode>(f)` | the subtree / the whole `Data`, once per request, after the commit |
| `mib::Instrumentation` | interface: one getter / setter / `validate<Object>()` per object, `<table>Rows/Next/Has`, RowStatus `create/destroy<Entry>` |
| `mib::registerMib(agent, impl)` / `mib::bind(mib, impl)` | serve an `Instrumentation` |
| `mib::send<Notification>(agent, values…)` | typed notification; table columns add the row index |

### Client side

| Class / function | What it does |
|---|---|
| `snmpwrap::Client(SessionConfig)` | a session to one device: v1 / v2c / v3 (`peer`, `version`, `community`, `user`, `securityLevel`, `auth…`, `priv…`, `contextName`, `timeout`, `retries`) |
| `get(oid)` / `get(oids)`, `getNext(oid)`, `getBulk(oids, nonRepeaters, maxRepetitions)` | read by OID |
| `set(oid, value)` / `set(varbinds)` | write; several values in one atomic request |
| `walk(root)` / `walk(root, callback)` | a whole subtree (GETBULK on v2c/v3); the callback can stop early |
| `mib::Remote(session)` | the device, nested like `Data` |
| `remote.<…>.<object>.get()` / `.set(v)` / `.varbind(v)` | one value; `set` checks against the MIB first; `varbind` only builds the checked varbind |
| `remote.<table>[index]` | one row (or a plain number): cells with `get` / `set`, `read()` all columns, `send(row)` all writable columns, `index()` |
| `remote.<table>.read()` | the whole table as `std::map<Index, Entry>` |
| `remote.<table>.create(index, values, activate = true)` / `.destroy(index)` | RowStatus tables: `values` is `mib::<Entry>Values` (`std::optional` per column) |
| `remote.<group>.send(msg)` / `remote.send(data)` | all writable scalars of a subtree / of the MIB in one request (tables not included) |
| `remote.change().set(target, v)…send()` | any selection of values in one request |
| `remote.read()` | everything in one walk, as `mib::Data` |
| `mib::Client(session)` | the flat layer under `Remote`: `<object>()`, `set<Object>(v)`, `make<Object>(v)`, `<table>()` |
| `snmpwrap::NotificationReceiver(address)` | listens for v1 / v2c traps and informs (acknowledges informs) |
| `receiver.poll(timeout)` / `run()` / `stop()` | process received notifications |
| `receiver.acceptCommunities({…})` / `onNotification(f)` | accept only these communities / untyped handler (`snmpwrap::Notification`) |
| `mib::Notifications(receiver)` | `on<Notification>(f)` with a typed struct (`.raw`: sender, community, version, uptime, varbinds), `onOther(f)` |

### Shared types

| Type | What it offers |
|---|---|
| `mib::oids::<name>` | the OID of every MIB node; `mib::oids::root` = the registered subtree |
| `mib::<Entry>Index` | one member per INDEX column, `toOid()`, `fromOid()`, SNMP order |
| `mib::<Entry>`, `mib::<Entry>Values` | a row (all readable columns); the values for creating a row |
| `enum class` per named-number type | `toString(v)`, `operator<<` (`on(1)`) |
| `mib::Object` | every object with a value, `toString(object)`: the first parameter of the hooks |
| `snmpwrap::Oid` | `Oid::parse("1.3.6")`, `str()`, `+`, `isPrefixOf()`, `suffixOf()`, `size()`, `[i]` |
| `snmpwrap::Value` | `Value::integer/gauge/counter32/counter64/timeTicks/string/opaque/bits/oid/ipAddress/null(…)`, `type()`, `as…()`, `str()`, `isException()` |
| `snmpwrap::Error` | base of all errors; `SetError(status, text)` (refuse / MIB check), `ResponseError` (`status()`, `index()`), `TransportError` |
| `snmpwrap::MibModel` | read MIB files: `load()`, `node()`, `resolve("name.1")`, `format(varbind)`, `parseValue(name, text)`, `validate()` |
| `snmpwrap::Mib`, `Handler` | the core under everything ([appendix](#appendix-a-without-a-mib-core-api)) |

---

## Appendix A: without a MIB (core API)

Everything above is built on a small, MIB-independent core that stays available for special cases – e.g. data whose OIDs
are only known at run time, or a proxy to another process. The reference is in the headers:

| Header | Contents |
|---|---|
| `snmpwrap/agent.hpp` | `Agent`, `AgentConfig` |
| `snmpwrap/mib.hpp` | `Mib` (scalars, tables, RowStatus by OID), `Handler`, `SetTransaction` |
| `snmpwrap/session.hpp` | `Session` – interface of a client session (for mocks) |
| `snmpwrap/client.hpp` | `Client`, `SessionConfig` |
| `snmpwrap/notification.hpp` | `NotificationReceiver`, `Notification`, interfaces `NotificationSender` / `NotificationSource` |
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
