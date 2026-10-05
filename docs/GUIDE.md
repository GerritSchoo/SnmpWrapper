# snmpwrap – User Guide

snmpwrap is a C++17 library that wraps the C API of **Net-SNMP 5.9** so you can

* build an **SNMP agent** that publishes your application's data (scalars and tables), and
* write an **SNMP client** (manager) that reads and writes data on other devices,

without touching Net-SNMP's C structures, callbacks, or memory management.

This guide explains how the wrapper works and walks you through every feature with examples.
The API reference is in the header files (`include/snmpwrap/*.hpp`, Doxygen comments).
Ready-to-run programs are described in [examples/README.md](../examples/README.md):
`simple_agent` / `simple_client` (core API) and `mib_agent` / `mib_client` (code generated from a MIB) –
a good starting point before reading on.

---

## Contents

1. [How it works – the big picture](#1-how-it-works--the-big-picture)
2. [Building and adding it to your project](#2-building-and-adding-it-to-your-project)
3. [Basic building blocks: Oid, Value, VarBind](#3-basic-building-blocks-oid-value-varbind)
4. [Your first agent in 5 minutes](#4-your-first-agent-in-5-minutes)
5. [Working from the MIB file (recommended)](#5-working-from-the-mib-file-recommended)
6. [Scalars](#6-scalars)
7. [Tables](#7-tables)
8. [Tables with RowStatus (create / delete rows)](#8-tables-with-rowstatus-create--delete-rows)
9. [Large tables](#9-large-tables)
10. [How SET requests are processed (transactions and rollback)](#10-how-set-requests-are-processed-transactions-and-rollback)
11. [Notifications (traps)](#11-notifications-traps)
12. [The main loop, threads and shutdown](#12-the-main-loop-threads-and-shutdown)
13. [SNMP versions v1, v2c and v3](#13-snmp-versions-v1-v2c-and-v3)
14. [Writing your own Handler](#14-writing-your-own-handler)
15. [The client](#15-the-client)
16. [From a MIB file to code – a worked example](#16-from-a-mib-file-to-code--a-worked-example)
17. [Error codes reference](#17-error-codes-reference)
18. [Troubleshooting](#18-troubleshooting)
19. [Building your own application (step by step)](#19-building-your-own-application-step-by-step)

---

## 1. How it works – the big picture

An SNMP agent built with snmpwrap is an **AgentX subagent**. It does not listen on UDP port 161
itself. Instead, the standard Net-SNMP daemon `snmpd` (the *master agent*) does that and forwards
requests for your OIDs to your process:

```
 SNMP manager                snmpd (master agent)                    your process
 (snmpget, NMS, ...)                                                 ┌────────────────────────────┐
        │  SNMP v1/v2c/v3 (UDP 161)     │        AgentX (TCP or      │ snmpwrap::Agent            │
        ├──────────────────────────────►│        Unix socket)        │   └─ Mib / Handler         │
        │                               ├───────────────────────────►│        └─ your callbacks   │
        │◄──────────────────────────────┤◄───────────────────────────┤             └─ your data   │
                                                                     └────────────────────────────┘
```

This split has big advantages:

* **snmpd handles all the protocol work**: SNMP versions, communities, SNMPv3 users,
  encryption, access control, trap destinations. Your code never sees any of that.
* **Your code only answers questions about OIDs**: "what is the value of OID X?",
  "what comes after OID Y?", "please set OID Z to this value".
* The standard MIBs (system, interfaces, …) keep working; you only add your own subtree.

**MIB-driven, but not MIB-dependent.** At run time an agent only needs OIDs and value types.
You can describe them by hand in code (the core API, works for any MIB), or – recommended – let
the code generator `snmpwrap-mibgen` produce typed C++ code from your MIB file at build time, so
the MIB stays the single source of truth ([section 5](#5-working-from-the-mib-file-recommended)).

Inside the library there are these layers:

| Layer | Class / tool | What it does |
|---|---|---|
| Connection | `Agent` | Initializes Net-SNMP as subagent, connects to snmpd, runs the request loop, routes requests to handlers. |
| Generic model | `Mib` | Implements correct SNMP behaviour for scalars and tables: GET, GETNEXT/GETBULK ordering, SET validation, rollback, RowStatus. |
| MIB layer (optional) | `snmpwrap-mibgen`, `MibModel`, `MibBinder` | Reads MIB files with Net-SNMP's parser; generates typed C++ code on top of `Mib` (build time) or binds by name (run time). |
| Your code | `Instrumentation` (generated), callbacks or `Handler` | Reads and writes your actual data. |

Whichever way you choose, the SNMP semantics live in one place (`Mib`); the generated code and the
run-time binding only connect your data to it.

---

## 2. Building and adding it to your project

### Requirements

* Linux (tested on Ubuntu 22.04 / WSL2)
* A C++17 compiler (tested with GCC 11)
* CMake ≥ 3.16
* Net-SNMP 5.9.x development files and tools (the code generator uses Net-SNMP's MIB parser; no Perl / mib2c needed):

```sh
sudo apt install build-essential cmake libsnmp-dev snmpd snmp
```

### Build and test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The build also produces the code generator `build/snmpwrap-mibgen` and writes the code generated
from the example MIBs into [generated/](../generated) (option `-DSNMPWRAP_BUILD_MIBGEN=OFF` disables
the generator). The tests cover the core API, the MIB layer and – against a real snmpd – three
implementations of the same MIB (hand-written, generated, run-time bound) for SNMPv1, v2c and v3.

If Net-SNMP is installed in a custom prefix, point CMake to it:
`cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/netsnmp` (CMake looks for `net-snmp-config`).

### Use it from your own CMake project

> A complete step-by-step walk-through with a separate agent and client application is in
> [section 19](#19-building-your-own-application-step-by-step).

Option A – as a subdirectory (or with `FetchContent`):

```cmake
add_subdirectory(external/SnmpWrapper)
target_link_libraries(my_app PRIVATE snmpwrap::snmpwrap)
```

Option B – installed:

```sh
cmake --install build --prefix /opt/snmpwrap
```

```cmake
find_package(snmpwrap REQUIRED)            # configure with -DCMAKE_PREFIX_PATH=/opt/snmpwrap
target_link_libraries(my_app PRIVATE snmpwrap::snmpwrap)
```

In both cases `snmpwrap_add_mib()` is available to generate code from your MIB files
([section 5](#5-working-from-the-mib-file-recommended)); the installed package contains the generator.

Your code includes only `snmpwrap/*.hpp`. These headers do **not** include Net-SNMP headers,
so Net-SNMP's macros do not leak into your project.

| Header | Contents |
|---|---|
| `snmpwrap/agent.hpp` | `Agent`, `AgentConfig` (includes everything needed for agents) |
| `snmpwrap/client.hpp` | `Client`, `SessionConfig` |
| `snmpwrap/mib.hpp` | `Mib`, `ScalarDef`, `TableDef`, `RowStatusSpec`, `Handler`, `SetTransaction` |
| `snmpwrap/index.hpp` | `IndexSpec`, `encodeIndex`, `decodeIndex` |
| `snmpwrap/oid.hpp` | `Oid`, `indexInt`, `indexString`, `indexIp` |
| `snmpwrap/value.hpp` | `Value`, `Type`, `VarBind` |
| `snmpwrap/error.hpp` | `Error`, `SetError`, `ResponseError`, `TransportError`, `ErrorStatus` |
| `snmpwrap/mib_model.hpp` | `MibModel`, `MibNode` – MIB files loaded at run time (names, formatting, value parsing) |
| `snmpwrap/mib_binder.hpp` | `MibBinder`, `ScalarBinding`, `TableBinding` – binding data to MIB objects by name |
| `<name>.hpp` (generated) | `oids`, enums, index/row structs, `Instrumentation`, `registerMib`, `send…`, `Client` for one MIB |

---

## 3. Basic building blocks: Oid, Value, VarBind

### Oid

An `Oid` is a sequence of numbers such as `1.3.6.1.4.1.99999.1.4.0`.

```cpp
using namespace snmpwrap;

Oid root = Oid::parse("1.3.6.1.4.1.99999");   // from text
Oid a    = Oid{1, 3, 6, 1};                    // from numbers
Oid inst = root + Oid{1, 4, 0};                // concatenation -> 1.3.6.1.4.1.99999.1.4.0
Oid b    = root + SubId{7};                    // append one number

root.isPrefixOf(inst);    // true: inst lies inside the subtree
root.suffixOf(inst);      // 1.4.0
inst.str();               // "1.3.6.1.4.1.99999.1.4.0"
```

Oids compare in **SNMP order** (number by number, not as text): `1.3.6.1.2 < 1.3.6.1.10`.

### Value

A `Value` is a typed SNMP value. Create it with a factory, read it with the matching accessor:

| SNMP type (MIB `SYNTAX`) | `Type` | create | read |
|---|---|---|---|
| `Integer32`, `INTEGER {…}`, `RowStatus`, `TruthValue` | `Integer` | `Value::integer(-5)` | `asInt()` |
| `Unsigned32`, `Gauge32` | `Gauge32` | `Value::gauge(42)` | `asUInt()` |
| `Counter32` | `Counter32` | `Value::counter32(1000)` | `asUInt()` |
| `Counter64` | `Counter64` | `Value::counter64(1ULL << 40)` | `asUInt64()` |
| `TimeTicks` | `TimeTicks` | `Value::timeTicks(360000)` | `asUInt()` |
| `OCTET STRING`, `DisplayString` | `OctetString` | `Value::string("eth0")` | `asString()` |
| `Opaque` | `Opaque` | `Value::opaque(bytes)` | `asString()` |
| `BITS` | `Bits` | `Value::bits(octets)` | `asString()` |
| `OBJECT IDENTIFIER` | `ObjectId` | `Value::oid(Oid{1,3,6})` | `asOid()` |
| `IpAddress` | `IpAddress` | `Value::ipAddress(10,0,0,1)` | `asIp()` |

```cpp
Value v = Value::gauge(42);
v.type();        // Type::Gauge32
v.asUInt();      // 42
v.asString();    // throws snmpwrap::Error – wrong type
v.str();         // "Gauge32: 42"  (handy for logging)
```

Accessors throw `snmpwrap::Error` on a type mismatch, so mistakes show up immediately.

### VarBind

A `VarBind` is simply an OID together with its value: `VarBind{oid, value}`.
It is used for SET requests, GETNEXT results, walks and notifications.

---

## 4. Your first agent in 5 minutes

### Step 1 – configure snmpd as master agent

Add these lines to `/etc/snmp/snmpd.conf` and restart snmpd:

```
master agentx
agentXSocket tcp:127.0.0.1:705
rocommunity public  127.0.0.1
rwcommunity private 127.0.0.1
```

* `master agentx` turns on AgentX support.
* `agentXSocket` is where subagents connect (the default is the Unix socket `/var/agentx/master`;
  TCP on localhost avoids permission problems).

### Step 2 – write the agent

```cpp
#include <csignal>
#include <snmpwrap/agent.hpp>

using namespace snmpwrap;

static Agent* g_agent = nullptr;

int main() {
    AgentConfig cfg;
    cfg.name = "hello-agent";
    cfg.agentxSocket = "tcp:127.0.0.1:705";

    Agent agent(cfg);
    g_agent = &agent;
    std::signal(SIGINT, [](int) { g_agent->stop(); });

    // Everything below 1.3.6.1.4.1.99999 is ours
    Mib& mib = agent.addMib(Oid::parse("1.3.6.1.4.1.99999"));

    // A read-only scalar: 1.3.6.1.4.1.99999.1.0
    mib.scalar(1, {Type::OctetString, [] { return Value::string("Hello, SNMP!"); }});

    agent.run();   // serve requests until Ctrl+C
}
```

### Step 3 – query it

```sh
$ snmpget -v2c -c public localhost 1.3.6.1.4.1.99999.1.0
SNMPv2-SMI::enterprises.99999.1.0 = STRING: "Hello, SNMP!"
```

> **About the OID 1.3.6.1.4.1.99999:** `1.3.6.1.4.1` is the "enterprises" branch; the next number
> is a Private Enterprise Number assigned by IANA. 99999 is used here only as a placeholder –
> use your organisation's number in real products.

---

## 5. Working from the MIB file (recommended)

In a real product the **MIB file is the interface description**: it defines every OID, type,
access right, value range, enumeration, table index and notification. Instead of repeating all
that by hand in C++, let snmpwrap generate the code from the MIB.

```
 SNMPWRAPPER-DEMO-MIB.txt ──(build time: snmpwrap-mibgen)──► snmpwrapper_demo_mib.hpp / .cpp
                                                                   │
                     your class implements  Instrumentation  ◄─────┘   (agent side)
                     your code calls        Client           ◄─────┘   (client side)
```

There are three MIB-driven building blocks; for agents with a fixed MIB the generator is the
way to go, the other two are for special cases:

| Building block | When | MIB needed at run time? |
|---|---|---|
| **Code generator** `snmpwrap-mibgen` | agents (and typed clients) for *your* MIB – the normal case | no |
| `MibModel` (load at run time) | clients that talk to devices with arbitrary MIBs: names, readable output, input checks | yes |
| `MibBinder` (bind by name at run time) | simulators, test agents, MIBs that are only known at start-up | yes |

### 5.1 Generating code with CMake

```cmake
add_executable(my_agent main.cpp)
snmpwrap_add_mib(my_agent
    MODULE SNMPWRAPPER-DEMO-MIB                      # module name in the MIB file
    MIB    ${CMAKE_CURRENT_SOURCE_DIR}/mibs/SNMPWRAPPER-DEMO-MIB.txt
    # MIB_DIRS <dir>...  directories with imported MIBs (Net-SNMP's standard MIBs are always found)
    # NAME <base>        file / namespace name (default: module name in snake_case)
)
```

* `snmpwrap_add_mib` is available after `find_package(snmpwrap)` or `add_subdirectory(...)`.
* The code is generated into the build directory and **regenerated automatically whenever the
  MIB file changes**. Never edit the generated files.
* The generator needs Net-SNMP on the build machine; the generated code only uses the snmpwrap
  API. The MIB file is **not** needed at run time.
* You can also run the tool by hand:
  `snmpwrap-mibgen --module SNMPWRAPPER-DEMO-MIB --out gen/ mibs/SNMPWRAPPER-DEMO-MIB.txt`

**Recommended project layout – one library per MIB, generated code visible in the source tree:**

```cmake
add_library(my_mib STATIC)
snmpwrap_add_mib(my_mib MODULE MY-MIB MIB ${PROJECT_SOURCE_DIR}/mibs/MY-MIB.txt
                 OUTPUT_DIR ${PROJECT_SOURCE_DIR}/generated/my_mib)
target_link_libraries(my_agent  PRIVATE my_mib)   # include path + snmpwrap come with it
target_link_libraries(my_client PRIVATE my_mib)
```

The code is then generated once, shared by agent and client, and can be read and reviewed in
`generated/my_mib/`. This project does exactly that: see [generated/](../generated) with the code
for both example MIBs.

### 5.2 What gets generated

For the demo MIB (`mibs/SNMPWRAPPER-DEMO-MIB.txt`) the generator creates, in namespace
`snmpwrapper_demo_mib`:

| MIB | Generated C++ |
|---|---|
| every node | `oids::deviceName`, `oids::sensorTable`, … and `oids::root` (registration root) |
| `INTEGER { … }`, TCs like `TruthValue` | `enum class TruthValue { true_ = 1, false_ = 2 }` + `toString()` |
| `INDEX { sensorIndex }` | `struct SensorEntryIndex { std::int32_t sensorIndex; toOid(); fromOid(); }` |
| a table row | `struct SensorEntry { sensorName; sensorValue; sensorEnabled; }` (for the client) |
| scalars / columns | pure virtual getters (and setters for read-write objects) in `class Instrumentation` |
| `SYNTAX` ranges, `SIZE`, enumerations | checks that run before your setter is called (and before the client sends) |
| RowStatus tables | `create…`, `destroy…`, status getter/setter, `…Complete`, a `…Values` struct with DEFVALs |
| `NOTIFICATION-TYPE` | `sendSensorAlarm(agent, sensorName, sensorValue, const SensorEntryIndex&)` |
| everything | `registerMib(agent, impl)` and a typed `class Client` |

MIB types become C++ types like this:

| MIB `SYNTAX` | C++ type |
|---|---|
| `Integer32`, `INTEGER` | `std::int32_t` (or the generated `enum class` if it has named numbers) |
| `RowStatus` | `snmpwrap::RowStatus` |
| `Unsigned32`, `Gauge32`, `Counter32`, `TimeTicks` | `std::uint32_t` |
| `Counter64` | `std::uint64_t` |
| `OCTET STRING`, `DisplayString`, `BITS`, `Opaque` | `std::string` (the `Value` keeps the type: `Value::string`, `Value::bits`, `Value::opaque`) |
| `OBJECT IDENTIFIER` | `snmpwrap::Oid` |
| `IpAddress` | `std::array<std::uint8_t, 4>` |

### 5.3 The agent: implement `Instrumentation`

```cpp
#include "snmpwrapper_demo_mib.hpp"          // generated
using namespace snmpwrapper_demo_mib;

class DemoDevice : public Instrumentation {
public:
    // scalars – one method per MIB object
    std::string   deviceName() override { return name_; }
    void          setDeviceName(const std::string& v) override { name_ = v; }  // SIZE(1..32) already checked
    std::uint32_t deviceUptime() override { return uptimeTicks(); }
    std::int32_t  alarmThreshold() override { return threshold_; }
    void          setAlarmThreshold(std::int32_t v) override { threshold_ = v; }  // range 0..100 already checked

    // sensorTable – typed index, enum for TruthValue
    std::vector<SensorEntryIndex> sensorTableRows() override { return {{1}, {2}, {3}}; }
    std::string  sensorName(const SensorEntryIndex& i) override { return sensors_.at(i.sensorIndex).name; }
    std::int32_t sensorValue(const SensorEntryIndex& i) override { return sensors_.at(i.sensorIndex).value; }
    TruthValue   sensorEnabled(const SensorEntryIndex& i) override { ... }
    void         setSensorEnabled(const SensorEntryIndex& i, TruthValue v) override { ... }
    ...
};

int main() {
    snmpwrap::Agent agent;
    DemoDevice device;
    registerMib(agent, device);                                  // every object of the MIB
    while (agent.poll()) {
        if (tooHot) sendSensorAlarm(agent, "cpu", 85, SensorEntryIndex{1});   // typed notification
    }
}
```

Key points:

* **The compiler checks your code against the MIB.** A new object in the MIB is a new pure virtual
  method – the build fails until you implement it. A changed type changes the method signature.
* **Validation is generated.** Values passed to setters already satisfy the MIB (`SYNTAX` ranges,
  `SIZE`, enumerations). For extra rules override the `validate…` hooks, e.g.
  `void validateAlarmThreshold(std::int32_t v) override { if (v < 20) throw snmpwrap::SetError(snmpwrap::ErrorStatus::InconsistentValue); }`.
* **Errors and rollback** work as described in [section 10](#10-how-set-requests-are-processed-transactions-and-rollback):
  throw `snmpwrap::SetError` from a setter and the whole request is rolled back.
* **Large tables:** `…Rows()` is all you must implement. For big tables additionally override
  `…Next(const snmpwrap::Oid* after)` and `…Has(const Index&)` (see [section 9](#9-large-tables));
  `…Rows()` may then return an empty vector.
* **RowStatus tables** (column with `SYNTAX RowStatus`) get `createXxxEntry(index, values)`,
  `destroyXxxEntry(index)`, a status getter/setter and `xxxEntryComplete(index)`. The state machine
  of [section 8](#8-tables-with-rowstatus-create--delete-rows) is applied automatically. Columns with a
  `DEFVAL` arrive pre-filled in `values`; writable columns *without* `DEFVAL` are required for `createAndGo`.

The complete example is [examples/mib_agent.cpp](../examples/mib_agent.cpp).

### 5.4 The client: typed access

```cpp
snmpwrap::Client session(cfg);
snmpwrapper_demo_mib::Client device(session);

std::string name = device.deviceName();
device.setAlarmThreshold(30);                       // checked against the MIB before it is sent
for (const auto& [index, row] : device.sensorTable())    // std::map<SensorEntryIndex, SensorEntry>
    std::cout << index.sensorIndex << " " << row.sensorName << " " << row.sensorValue << "\n";

// RowStatus tables: create a row with one atomic request
// device.createXxxEntry(XxxEntryIndex{7}, values);  device.destroyXxxEntry(XxxEntryIndex{7});
```

Getters throw `snmpwrap::Error` if the agent has no such object or row; setters throw
`snmpwrap::SetError` for values that violate the MIB – before anything is sent.

### 5.5 Loading a MIB at run time (`MibModel`)

For clients that work with *any* device, load the MIB when the program starts:

```cpp
snmpwrap::MibModel mib = snmpwrap::MibModel::load({"SNMPWRAPPER-DEMO-MIB.txt"}, {"/usr/share/my-mibs"});

snmpwrap::Oid oid = mib.resolve("sensorName.2");            // names instead of OIDs ("MODULE::name" also works)
std::cout << mib.format(session.get(oid)) << "\n";           // sensorName.2 = "board"
snmpwrap::Value v = mib.parseValue("alarmThreshold", "35");   // type and range from the MIB
const snmpwrap::MibNode& n = mib.node("sensorValue");        // type, access, ranges, units, description …
```

`format()` prints enum labels (`sensorEnabled.1 = true(1)`), units (`35 degrees Celsius`),
TimeTicks and strings like the Net-SNMP tools do. Plain OIDs keep working everywhere.
The command line client supports this too: `client_cli -m SNMPWRAPPER-DEMO-MIB.txt localhost walk sensorTable`
and `client_cli -m … localhost set sensorEnabled.3 = false`.

Notes: load MIBs at program start, before creating the first `snmpwrap::Client`, and create an
`snmpwrap::Agent` (if any) before both. Loading errors (syntax errors, missing imports) throw
`snmpwrap::Error` with Net-SNMP's diagnostics (file and line).

### 5.6 Binding at run time (`MibBinder`) – for special cases

If the MIB is only known when the program starts (simulators, test tools), bind data by name:

```cpp
snmpwrap::MibModel model = snmpwrap::MibModel::load({"SNMPWRAPPER-DEMO-MIB.txt"});
snmpwrap::Agent agent;
snmpwrap::MibBinder bind(agent.addMib(model.oid("snmpWrapperDemoMIB")), model);
bind.scalar("alarmThreshold", [&] { return snmpwrap::Value::integer(t); },
                              [&](const snmpwrap::Value& v) { t = v.asInt(); });   // range from the MIB
bind.table("sensorTable", sensorBinding);    // index, columns, RowStatus from the MIB
bind.finish();                               // error if an object of the MIB has no data
```

Type, access, ranges, index and RowStatus come from the MIB, but mistakes (a misspelled name, a
missing setter) are only found when the program starts, and the MIB files must be shipped with
the program. Prefer the generator for products.
`test/agents/runtime_test_agent.cpp` is a complete example.

---

## 6. Scalars

A scalar is an object with exactly one value. By SNMP convention its instance OID ends in `.0`.

```cpp
mib.scalar({1, 4}, def);   // object 1.3.6.1.4.1.99999.1.4  ->  instance 1.3.6.1.4.1.99999.1.4.0
mib.scalar(7, def);        // shortcut for {7}              ->  instance 1.3.6.1.4.1.99999.7.0
```

The first argument is the object's OID **relative to the Mib root**. A `ScalarDef` has four fields:

| field | required | meaning |
|---|---|---|
| `type` | yes | the SNMP type; `get` must return it, SETs with other types are rejected |
| `get` | yes | returns the current value |
| `set` | no | stores a new value. Without it the scalar is read-only |
| `validate` | no | checks a new value *before* anything is written |

### Read-only

```cpp
std::uint32_t requests = 0;
mib.scalar({1, 2}, {Type::Counter32, [&] { return Value::counter32(requests); }});
```

### Read-write with validation

```cpp
int limit = 50;
mib.scalar({1, 4}, {
    Type::Integer,
    [&] { return Value::integer(limit); },                       // get
    [&](const Value& v) { limit = v.asInt(); },                  // set
    [](const Value& v) {                                         // validate
        if (v.asInt() < 1 || v.asInt() > 100)
            throw SetError(ErrorStatus::WrongValue, "limit must be 1..100");
    }});
```

```sh
$ snmpset -v2c -c private localhost 1.3.6.1.4.1.99999.1.4.0 i 75
SNMPv2-SMI::enterprises.99999.1.4.0 = INTEGER: 75
$ snmpset -v2c -c private localhost 1.3.6.1.4.1.99999.1.4.0 i 500
Error in packet.
Reason: wrongValue (The set value is illegal or unsupported in some way)
```

What the wrapper checks for you automatically, before your `validate` runs:

* scalar without `set` → `notWritable`
* value of a different type (e.g. a string for an Integer) → `wrongType`

> **Tip:** put checks in `validate`, not in `set`. `validate` runs before *any* change of the
> request is made, so a rejected request never changes anything. Errors thrown in `set` are also
> handled (the request is rolled back, see [section 10](#10-how-set-requests-are-processed-transactions-and-rollback)),
> but should be reserved for real write failures.

---

## 7. Tables

### How table OIDs are built

In SMIv2 a table has the following structure:

```
myTable        = <rel>                    (SEQUENCE OF MyEntry, not accessible)
  myEntry      = <rel>.1                  (one row, not accessible)
    myColumn   = <rel>.1.<column>         (the column object)
      cell     = <rel>.1.<column>.<index> (one value)
```

So a cell OID is: **table OID + `1` + column number + row index**. A walk returns all cells
**column by column**: first column 2 of all rows, then column 3 of all rows, and so on.

### A simple table

Suppose you have a list of interfaces in your program:

```cpp
struct Port { std::string name; std::uint32_t speed; int status; };
std::map<std::uint32_t, Port> ports = {{1, {"eth0", 1000, 1}}, {2, {"eth1", 100, 2}}};
```

Publish it as table `1.3.6.1.4.1.99999.2` with an Integer index:

```cpp
TableDef t;
t.indexes = {IndexSpec::integer()};                       // INDEX { portIndex }
t.columns = {
    {2, Type::OctetString, Access::ReadWrite},             // portName
    {3, Type::Gauge32,     Access::ReadOnly},              // portSpeed
    {4, Type::Integer,     Access::ReadWrite},             // portStatus
};

// which rows exist?
t.rows = [&] {
    std::vector<Oid> out;
    for (const auto& [idx, p] : ports) out.push_back(indexInt(idx));
    return out;
};

// value of one cell
t.get = [&](const Oid& index, SubId column) {
    const Port& p = ports.at(index[0]);
    switch (column) {
        case 2:  return Value::string(p.name);
        case 3:  return Value::gauge(p.speed);
        default: return Value::integer(p.status);
    }
};

// optional: check new values
t.validate = [](const Oid&, SubId column, const Value& v) {
    if (column == 4 && (v.asInt() < 1 || v.asInt() > 2))
        throw SetError(ErrorStatus::WrongValue, "status is up(1) or down(2)");
};

// optional: write a cell
t.set = [&](const Oid& index, SubId column, const Value& v) {
    Port& p = ports.at(index[0]);
    if (column == 2) p.name = v.asString();
    else             p.status = v.asInt();
};

mib.table(2, std::move(t));
```

```sh
$ snmpwalk -v2c -c public localhost 1.3.6.1.4.1.99999.2
...99999.2.1.2.1 = STRING: "eth0"
...99999.2.1.2.2 = STRING: "eth1"
...99999.2.1.3.1 = Gauge32: 1000
...99999.2.1.3.2 = Gauge32: 100
...99999.2.1.4.1 = INTEGER: 1
...99999.2.1.4.2 = INTEGER: 2
```

Notes:

* **Index columns are not listed in `columns`.** In most MIBs the index column (here
  `portIndex`, column 1) is `not-accessible`; its value is already part of the OID.
* The wrapper sorts rows and columns itself; `rows` may return the indexes in any order.
* `get` is only called for rows that exist and for columns you declared.
* Writing a cell of a row that does not exist is rejected with `noCreation` (unless the table has
  a RowStatus column, see the next section).

### Row indexes

The row index is the part of the OID after the column number. It is an `Oid`, because an index
can consist of several numbers. How index values are turned into numbers is defined by
RFC 2578; snmpwrap implements it:

| MIB index column | `IndexSpec` | value `x` becomes |
|---|---|---|
| `Integer32` | `integer()` | `x` |
| `Unsigned32` | `unsignedInt()` | `x` |
| `OCTET STRING` / `DisplayString` | `string()` | length, then one number per byte: `"ab"` → `2.97.98` |
| `IMPLIED DisplayString` (last column) | `impliedString()` | one number per byte, no length: `"ab"` → `97.98` |
| `OCTET STRING (SIZE(n))` | `fixedString(n)` | exactly n numbers, no length |
| `IpAddress` | `ipAddress()` | four numbers: `10.0.0.1` |
| `OBJECT IDENTIFIER` | `objectId()` | length, then the sub-ids |

For a single number use the shortcut `indexInt(5)`. For several index columns use
`encodeIndex` / `decodeIndex`:

```cpp
// MIB:  INDEX { connAddr, connPort, IMPLIED connTag }
const std::vector<IndexSpec> spec{IndexSpec::ipAddress(), IndexSpec::integer(), IndexSpec::impliedString()};

Oid idx = encodeIndex(spec, {Value::ipAddress(10, 0, 0, 1), Value::integer(80), Value::string("web")});
// idx == 10.0.0.1.80.119.101.98

auto values = decodeIndex(spec, idx);           // std::optional<std::vector<Value>>
if (values) {
    auto ip   = (*values)[0].asIp();
    int  port = (*values)[1].asInt();
    std::string tag = (*values)[2].asString();
}
```

When you set `TableDef::indexes`, the wrapper validates every index in requests: an index that is
malformed (wrong length, trailing numbers, byte > 255, …) is treated as "row does not exist".
A typical pattern is to use the encoded index directly as the key of a `std::map<Oid, Row>`.

---

## 8. Tables with RowStatus (create / delete rows)

Many MIBs let managers **create and delete rows** through a column of type `RowStatus`
(RFC 2579). The rules of RowStatus are surprisingly involved; snmpwrap implements the state
machine for you. You only provide callbacks to store rows:

```cpp
struct Entry { std::string name; std::uint32_t value = 0; RowStatus status = RowStatus::NotReady; };
std::map<std::uint32_t, Entry> entries;

TableDef t;
t.indexes = {IndexSpec::integer()};
t.columns = {{2, Type::OctetString, Access::ReadWrite},      // entryName   (read-create)
             {3, Type::Gauge32,     Access::ReadWrite}};     // entryValue  (read-create)
             // column 4 = entryStatus (RowStatus) is added automatically – do not list it
t.rows = [&] { std::vector<Oid> r; for (auto& [i, e] : entries) r.push_back(Oid{i}); return r; };
t.get  = [&](const Oid& i, SubId c) {
    const Entry& e = entries.at(i[0]);
    return c == 2 ? Value::string(e.name) : Value::gauge(e.value);
};
t.set  = [&](const Oid& i, SubId c, const Value& v) {
    Entry& e = entries.at(i[0]);
    if (c == 2) e.name = v.asString(); else e.value = v.asUInt();
};

RowStatusSpec rs;
rs.column = 4;                           // the RowStatus column
rs.requiredColumns = {2};                // createAndGo must supply a name
rs.create   = [&](const Oid& i, const std::map<SubId, Value>& cols) {
    Entry e;
    if (auto n = cols.find(2); n != cols.end()) e.name = n->second.asString();
    if (auto v = cols.find(3); v != cols.end()) e.value = v->second.asUInt();
    entries[i[0]] = e;
};
rs.destroy  = [&](const Oid& i) { entries.erase(i[0]); };           // must tolerate missing rows
rs.setState = [&](const Oid& i, RowStatus s) { entries.at(i[0]).status = s; };
rs.state    = [&](const Oid& i) { return entries.at(i[0]).status; };
rs.complete = [&](const Oid& i) { return !entries.at(i[0]).name.empty(); };
t.rowStatus = rs;

mib.table(4, std::move(t));
```

### What managers can do

Let `S = ...99999.4.1.4` (status column) and `N = ...99999.4.1.2` (name column).

**Create and activate in one step (createAndGo = 4):**

```sh
snmpset -v2c -c private localhost  N.7 s "backup"  S.7 i 4
```
→ the wrapper calls `create(7, {2: "backup"})`, then `setState(7, Active)`.

**Create first, fill in later (createAndWait = 5):**

```sh
snmpset -v2c -c private localhost  S.8 i 5         # row 8 now exists, status notReady(3)
snmpset -v2c -c private localhost  N.8 s "spare"   # complete -> status becomes notInService(2)
snmpset -v2c -c private localhost  S.8 i 1         # activate -> active(1)
```

**Delete (destroy = 6):**

```sh
snmpset -v2c -c private localhost  S.7 i 6
```
→ the wrapper calls `destroy(7)` – in the commit phase, so it happens only if the whole request succeeds.

### Rules enforced by the wrapper

| Request | Result |
|---|---|
| createAndGo without all `requiredColumns` | `inconsistentValue` (nothing created) |
| createAndGo / createAndWait on an existing row | `inconsistentValue` |
| active / notInService on a non-existing row | `inconsistentValue` |
| active on a row that is not `complete` | `inconsistentValue` |
| set `notReady(3)` or a value outside 1..6 | `wrongValue` |
| write a cell of a non-existing row without create | `inconsistentName` |
| create with a malformed index (if `indexes` is set) | `inconsistentName` |
| destroy of a non-existing row | success, no-op |
| notReady row gets its missing data | becomes `notInService` automatically |
| column of an `active` row changed (only with `RowStatusSpec::rejectEditWhileActive = true`, RFC 2579) | `inconsistentValue`, unless the same request sets `notInService` |

Your `validate` callback is called for the columns of a new row as well, so the same checks apply
when rows are created.

---

## 9. Large tables

With `rows`, the wrapper asks for the list of all rows when it needs to find "the next row". That
is fine for hundreds or a few thousand rows. For big tables, provide **`nextRow`** and **`hasRow`**
instead; then the wrapper never enumerates the table:

```cpp
std::map<std::uint32_t, Sample> samples;        // could be 1 million entries

TableDef t;
t.indexes = {IndexSpec::unsignedInt()};
t.columns = {{2, Type::Gauge32, Access::ReadOnly}};
t.hasRow  = [&](const Oid& i) { return i.size() == 1 && samples.count(i[0]) > 0; };
t.nextRow = [&](const Oid* after) -> std::optional<Oid> {
    auto it = after ? samples.upper_bound((*after)[0]) : samples.begin();
    if (it == samples.end()) return std::nullopt;
    return Oid{it->first};
};
t.get = [&](const Oid& i, SubId) { return Value::gauge(samples.at(i[0]).value); };
mib.table(6, std::move(t));
```

Rules for `nextRow(after)`:

* `after == nullptr` → return the first row.
* otherwise return the first row whose index is **strictly greater** than `*after` (in OID order).
  `*after` need not be an existing row.
* return `std::nullopt` when there are no more rows.
* results must be strictly increasing; the wrapper checks this to protect against endless walks.

With a sorted container (`std::map`, a database index, …) each GETNEXT is then O(log n).

---

## 10. How SET requests are processed (transactions and rollback)

A single SNMP SET request may contain several varbinds, e.g. two scalars and three table cells.
SNMP requires that **either all of them are applied or none**. Net-SNMP processes a SET in
phases; snmpwrap maps them onto a transaction:

```
   ┌──────────────┐   all valid?   ┌──────────────┐   ok    ┌──────────────┐
   │ 1. validate  │ ─────────────► │  2. apply    │ ──────► │  3. commit   │  (destroy rows here)
   │ all varbinds │                │ write values │         └──────────────┘
   └──────┬───────┘                └──────┬───────┘
          │ error                         │ error in a set()/create()
          ▼                               ▼
   request rejected,               4. undo: restore old values,
   NOTHING was changed             delete rows created by this request
```

1. **Validate** – for every varbind: does the object exist, is it writable, is the type right,
   does `validate` accept it, are the RowStatus rules met? The old values are remembered.
   Any error rejects the request; nothing has been changed yet.
2. **Apply** – your `set` and `create` callbacks run.
3. **Commit** – irreversible actions (row deletion) run.
4. **Undo** – only if apply failed: every value written so far is set back to its old value and
   rows created by this request are destroyed again.

Errors are reported **on the varbind that caused them** (the `errindex` field of the response),
so managers can see which value was wrong:

```sh
$ snmpset -v2c -c private localhost  ...99999.1.4.0 i 60   ...99999.1.1.0 i 5
Error in packet.
Reason: wrongType (The set value has the wrong type for the object)
Failed object: ...99999.1.1.0
```

### Failing in `set`

If writing really fails (device busy, disk full, …) throw a `SetError` from `set`:

```cpp
t.set = [&](const Oid& i, SubId c, const Value& v) {
    if (!device.write(i[0], c, v.asUInt()))
        throw SetError(ErrorStatus::CommitFailed, "device write failed");
};
```

The manager receives `commitFailed`, and all other changes of the same request are undone.
Any other exception thrown from a callback is treated like `CommitFailed` (in apply) or
`genErr` (in validate), so a bug in a callback never crashes the agent.

---

## 11. Notifications (traps)

```cpp
const Oid root = Oid::parse("1.3.6.1.4.1.99999");
agent.sendTrap(root + Oid{3, 0, 1},                                   // NOTIFICATION-TYPE OID
               {{root + Oid{1, 1, 0}, Value::string("disk almost full")},
                {root + Oid{1, 4, 0}, Value::integer(95)}});
```

* `sysUpTime.0` and `snmpTrapOID.0` are added automatically.
* The notification goes to snmpd, which sends it to every configured destination:

```
trapsink   192.168.1.50 public        # SNMPv1 trap
trap2sink  192.168.1.50 public        # SNMPv2c trap
informsink 192.168.1.50 public        # SNMPv2c inform (acknowledged)
trapsess   -v3 -u trapuser -l authNoPriv -a SHA -A secret123 192.168.1.50   # SNMPv3
```

For v1 sinks snmpd converts the notification into a v1 trap automatically.

---

## 12. The main loop, threads and shutdown

### Option 1 – `run()`

`agent.run()` processes requests until `agent.stop()` is called. `stop()` is safe to call from a
signal handler or another thread; `run()` returns within about one second.

### Option 2 – your own loop with `poll()`

```cpp
agent.start();                       // connect (also done implicitly by poll)
while (agent.poll()) {               // waits for work, at most ~1 s
    updateMeasurements();
    if (temperature > 80) agent.sendTrap(alarmOid, {...});
}
```

`poll(false)` returns immediately – useful if you already have an event loop that calls it
regularly.

### Threads

Net-SNMP is not thread-safe and keeps global state, so:

* **one `Agent` per process**;
* call `run()`, `poll()`, `addMib()`, `addHandler()` and `sendTrap()` from **one thread**
  (the "agent thread"); all your callbacks are called from that thread as well;
* if other threads change the data your callbacks read, protect it with a mutex:

```cpp
std::mutex m;
int limit = 50;

// worker thread
{ std::lock_guard<std::mutex> l(m); limit = computeLimit(); }

// agent callbacks
mib.scalar({1, 4}, {Type::Integer,
    [&] { std::lock_guard<std::mutex> l(m); return Value::integer(limit); },
    [&](const Value& v) { std::lock_guard<std::mutex> l(m); limit = v.asInt(); }});
```

### Agent and Client in the same program

Create the `Agent` **before** the first `Client`. (Net-SNMP initializes itself only once per
process, and the AgentX connection is set up during that initialization; the Agent constructor
throws a clear error if the order is wrong.) Clients must not be used after the Agent is destroyed.

The recommended order at program start is therefore:

1. `MibModel::load(...)` – only if you load MIBs at run time (generated code needs no MIB file),
2. `Agent agent(config);`
3. `Client client(...)` – if the program also acts as a client.

### Reconnection

If snmpd is restarted, Net-SNMP notices it (AgentX ping every `AgentConfig::pingIntervalSec`
seconds) and reconnects and re-registers the subtrees. This is Net-SNMP's own mechanism; it is not
covered by snmpwrap's automated tests.

---

## 13. SNMP versions v1, v2c and v3

**For the agent, versions are purely a matter of the snmpd configuration** – your code is the same
for all versions:

```
# /etc/snmp/snmpd.conf
master agentx
agentXSocket tcp:127.0.0.1:705

# SNMPv1 / v2c
rocommunity public  192.168.1.0/24
rwcommunity private 127.0.0.1

# SNMPv3 users (passphrases: at least 8 characters)
createUser monitor SHA "authpass123"
createUser admin   SHA "authpass123" AES "privpass123"
rouser monitor auth          # read-only, authentication required
rwuser admin   priv          # read-write, authentication + encryption required
```

Some differences between the versions are visible to managers; the wrapper and snmpd handle
them for you:

| Topic | SNMPv1 | SNMPv2c / v3 |
|---|---|---|
| missing object / instance | error `noSuchName` | per-value `noSuchObject` / `noSuchInstance` |
| SET error codes | only `badValue`, `noSuchName`, `genErr`, … (mapped automatically) | full set: `wrongValue`, `wrongType`, `notWritable`, … |
| `Counter64` values | invisible (skipped in walks, `noSuchName` on GET) | normal |
| GETBULK | not available | available |

---

## 14. Writing your own Handler

`Mib` covers most needs. If your data has an unusual shape (for example, it is served by
another process), implement `Handler` directly:

```cpp
class ProxyHandler : public Handler {
public:
    std::optional<Value> get(const Oid& oid) override {
        return backend.lookup(oid);                  // nullopt if it does not exist
    }
    std::optional<VarBind> getNext(const Oid& after) override {
        return backend.next(after);                  // first OID strictly greater than `after`
    }
    Type missing(const Oid& oid) override {
        return backend.knowsObject(oid) ? Type::NoSuchInstance : Type::NoSuchObject;
    }
    std::unique_ptr<SetTransaction> prepare(const std::vector<VarBind>& sets) override {
        for (std::size_t i = 0; i < sets.size(); ++i)
            if (!backend.writable(sets[i].oid))
                throw SetError(ErrorStatus::NotWritable, "read-only", i);   // i = varbind position
        return std::make_unique<ProxyTxn>(backend, sets);   // implements apply / undo / commit
    }
};

agent.addHandler(Oid::parse("1.3.6.1.4.1.99999.10"), std::make_shared<ProxyHandler>());
```

Rules: `getNext` must return OIDs in increasing order and only OIDs inside the registered
subtree; return `std::nullopt` when the subtree is exhausted. Do not leave `prepare`
unimplemented if the subtree should be writable – the default rejects every SET.

---

## 15. The client

### Connecting

```cpp
#include <snmpwrap/client.hpp>
using namespace snmpwrap;

// SNMPv2c
SessionConfig v2;
v2.peer = "192.168.1.10";            // port 161 by default; "host:port" or "tcp:host:port" also work
v2.community = "public";

// SNMPv1
SessionConfig v1 = v2;
v1.version = SessionConfig::Version::V1;

// SNMPv3 with authentication and encryption
SessionConfig v3;
v3.peer = "192.168.1.10";
v3.version = SessionConfig::Version::V3;
v3.user = "admin";
v3.securityLevel = SessionConfig::SecurityLevel::AuthPriv;
v3.authProtocol = SessionConfig::AuthProtocol::SHA1;    v3.authPassphrase = "authpass123";
v3.privProtocol = SessionConfig::PrivProtocol::AES128;  v3.privPassphrase = "privpass123";
// also available: AuthProtocol::MD5 / SHA224 / SHA256 / SHA384 / SHA512 and
// PrivProtocol::DES / AES192 / AES256 - SHA-2 needs a Net-SNMP built with OpenSSL, AES192/256 needs
// --enable-blumenthal-aes; otherwise the Client throws an Error when it is created / on the first request

v3.timeout = std::chrono::milliseconds(2000);           // per try
v3.retries = 2;

Client client(v3);
```

### Reading

```cpp
VarBind d = client.get(Oid::parse("1.3.6.1.2.1.1.1.0"));          // sysDescr.0
std::cout << d.value.str() << "\n";

auto several = client.get({Oid::parse("1.3.6.1.2.1.1.3.0"),        // one request, several OIDs
                           Oid::parse("1.3.6.1.2.1.1.5.0")});

VarBind next = client.getNext(Oid::parse("1.3.6.1.2.1.1"));

// whole subtree (GETBULK on v2c/v3, GETNEXT on v1)
for (const VarBind& vb : client.walk(Oid::parse("1.3.6.1.2.1.2.2.1.2")))   // ifDescr column
    std::cout << vb.oid.str() << " = " << vb.value.asString() << "\n";

// stop early: return false from the callback
client.walk(Oid::parse("1.3.6.1.2.1"), [](const VarBind& vb) {
    std::cout << vb.oid.str() << "\n";
    return vb.oid.size() < 12;
});
```

For v2c/v3, a missing OID is not an exception – check the value:

```cpp
VarBind vb = client.get(oid);
if (vb.value.isException())      // Type::NoSuchObject / NoSuchInstance / EndOfMibView
    std::cout << "not available: " << toString(vb.value.type()) << "\n";
```

### Writing

```cpp
client.set(Oid::parse("1.3.6.1.2.1.1.6.0"), Value::string("server room 2"));   // sysLocation.0

// several values in ONE atomic request – e.g. create a table row with RowStatus
const Oid tbl = Oid::parse("1.3.6.1.4.1.99999.4");
client.set({{tbl + Oid{1, 2, 7}, Value::string("backup")},
            {tbl + Oid{1, 4, 7}, Value::integer(4)}});      // createAndGo
```

### Error handling

```cpp
try {
    client.set(oid, Value::integer(500));
} catch (const ResponseError& e) {          // agent said no
    std::cerr << "rejected: " << e.what()
              << " status=" << static_cast<int>(e.status())   // e.g. 10 = wrongValue
              << " varbind=" << e.index() << "\n";             // 1-based
} catch (const TransportError& e) {         // no answer / security problem
    std::cerr << "transport: " << e.what() << "\n";
}
```

> A **wrong community** produces a *timeout*, not an error message: agents silently drop requests
> with unknown communities. Wrong SNMPv3 passwords or unknown users are reported as
> `TransportError` with a descriptive message.

The example program `examples/client_cli.cpp` is a small command line client that uses all of
this:

```sh
client_cli -v 3 -u admin -l authPriv -a SHA -A authpass123 -x AES -X privpass123 \
           192.168.1.10 walk 1.3.6.1.2.1.1
# -a MD5|SHA|SHA-224|SHA-256|SHA-384|SHA-512    -x DES|AES|AES-192|AES-256
client_cli -v 2c -c private localhost set 1.3.6.1.4.1.99999.1.4.0 i 75

# with a MIB: names instead of OIDs, readable output, value types taken from the MIB ("=")
client_cli -m mibs/SNMPWRAPPER-TEST-MIB.txt localhost walk swtConnTable
client_cli -m mibs/SNMPWRAPPER-TEST-MIB.txt -c private localhost set swtRowName.5 = demo swtRowStatus.5 = createAndGo
```

Everything shown in this section works with plain OIDs. With a MIB you can additionally use the
generated typed client ([section 5.4](#54-the-client-typed-access)) or `MibModel` for names and
readable output ([section 5.5](#55-loading-a-mib-at-run-time-mibmodel)).

---

## 16. From a MIB file to code – a worked example

> With the code generator ([section 5](#5-working-from-the-mib-file-recommended)) this mapping is done
> for you. This section shows what happens underneath – useful when you use the core API directly
> or want to understand the generated code.

The project contains an example MIB, `mibs/SNMPWRAPPER-TEST-MIB.txt`, and three agents that implement
it: by hand with the core API (`examples/test_agent.cpp`), with the generated code
(`test/agents/generated_test_agent.cpp`) and with run-time binding (`test/agents/runtime_test_agent.cpp`).
Here is how one table of the MIB maps to the core API.

**MIB:**

```
swtRowTable OBJECT-TYPE   SYNTAX SEQUENCE OF SwtRowEntry   ::= { snmpWrapperTestMIB 4 }
swtRowEntry OBJECT-TYPE   INDEX { swtRowIndex }             ::= { swtRowTable 1 }

swtRowIndex  Integer32 (1..2147483647)  not-accessible  ::= { swtRowEntry 1 }
swtRowName   DisplayString (SIZE(0..32)) read-create    ::= { swtRowEntry 2 }
swtRowValue  Gauge32 (0..1000)           read-create    ::= { swtRowEntry 3 }
swtRowStatus RowStatus                   read-create    ::= { swtRowEntry 4 }
```

**Translation, line by line:**

| MIB | Code |
|---|---|
| `snmpWrapperTestMIB` = enterprises.99999 | `agent.addMib(Oid::parse("1.3.6.1.4.1.99999"))` |
| `swtRowTable ::= { … 4 }` | `mib.table(4, …)` |
| `INDEX { swtRowIndex }`, Integer32 | `t.indexes = {IndexSpec::integer()}` – not in `columns` |
| `swtRowName` DisplayString, read-create, column 2 | `{2, Type::OctetString, Access::ReadWrite}` |
| `SIZE(0..32)` | `validate`: reject longer strings with `WrongLength` |
| `swtRowValue` Gauge32 (0..1000), column 3 | `{3, Type::Gauge32, Access::ReadWrite}` + range check in `validate` |
| `swtRowStatus` RowStatus, column 4 | `RowStatusSpec rs; rs.column = 4;` – not in `columns` |

Run it and try it out:

```sh
build/examples/test_agent --socket tcp:127.0.0.1:705
snmpwalk -v2c -c public -M +./mibs -m +SNMPWRAPPER-TEST-MIB localhost SNMPWRAPPER-TEST-MIB::snmpWrapperTestMIB
snmpset  -v2c -c private -M +./mibs -m +SNMPWRAPPER-TEST-MIB localhost \
         SNMPWRAPPER-TEST-MIB::swtRowName.5 s demo  SNMPWRAPPER-TEST-MIB::swtRowStatus.5 i createAndGo
```

(`-M` and `-m` only tell the *tools* where to find the MIB so they can show names – the agent
does not need the MIB file at run time, not even with generated code.)

**General checklist when implementing a MIB:**

1. Find the module's OID → `addMib(root)`.
2. For each scalar: relative OID, `SYNTAX` → `Type`, `MAX-ACCESS` → whether you provide `set`;
   ranges and sizes → `validate`.
3. For each table: table OID, `INDEX` clause → `indexes` (watch for `IMPLIED`), accessible columns →
   `columns`, `RowStatus` column → `rowStatus`.
4. For each `NOTIFICATION-TYPE`: its OID and `OBJECTS` → `sendTrap(oid, {...})`.

---

## 17. Error codes reference

Use these in `SetError(ErrorStatus::…)`. Values are the numbers on the wire.

| `ErrorStatus` | No. | Use it when … | seen by v1 managers as |
|---|---|---|---|
| `WrongType` | 7 | the value has the wrong type (checked automatically) | `badValue` |
| `WrongLength` | 8 | a string is too long / too short | `badValue` |
| `WrongValue` | 10 | a number is out of range, an enum value is unknown | `badValue` |
| `NoCreation` | 11 | the instance does not exist and cannot be created (automatic) | `noSuchName` |
| `InconsistentValue` | 12 | the value is valid in general but not right now | `badValue` |
| `ResourceUnavailable` | 13 | no memory / no free slot | `genErr` |
| `CommitFailed` | 14 | writing failed – the request is rolled back | `genErr` |
| `NotWritable` | 17 | the object is read-only (automatic) | `noSuchName` |
| `InconsistentName` | 18 | the row does not exist and this request cannot create it (automatic) | `noSuchName` |
| `GenErr` | 5 | anything else | `genErr` |

---

## 18. Troubleshooting

**`snmpget` returns "No Such Object available on this agent at this OID".**
* Is the agent running and connected? It logs `NET-SNMP version 5.9.1 AgentX subagent connected`.
* Does snmpd have `master agentx`, and does `agentXSocket` match `AgentConfig::agentxSocket`?
* With the default Unix socket `/var/agentx/master` the agent needs permission to access it –
  use `tcp:127.0.0.1:705` on both sides or adjust `agentXPerms` in snmpd.conf.
* Does the access configuration (`rocommunity … `, views) of snmpd include your subtree?

**"No Such Instance" instead of a value.** The object is known but this instance is not: wrong
row index, or a scalar without the trailing `.0`.

**A SET returns `notWritable`.** The scalar has no `set` callback, the column is `ReadOnly`, or the
table has no `set` callback. Also check that you used the read-write community or v3 user.

**A SET returns `wrongType`.** The manager sent a different type than declared, e.g.
`snmpset … u 5` (Gauge32) for an `Integer` object – use `i`.

**Timeout from the client.** Wrong host/port, firewall, or a **wrong community** (silently dropped).

**Counter64 values are missing in a walk.** The manager uses SNMPv1, which cannot transport
Counter64. Use v2c or v3.

**SNMPv3 with encryption does not work.** Which algorithms are available depends on how Net-SNMP
was built. Distribution packages (built with OpenSSL) support all of them. Check with the
Net-SNMP tools first: if `snmpget -v3 -l authPriv …` fails too, the problem is in the Net-SNMP
installation or the user configuration, not in your code.

**"snmpwrap::Agent must be created before the first snmpwrap::Client".** Create the Agent first
(see [section 12](#12-the-main-loop-threads-and-shutdown)).

**My walk never ends / "nextRow returned … which is not greater than …".** Your `nextRow` callback
(or the generated `…Next()` you overrode) returned an index that is not strictly greater than `after`.

### MIB files and generated code

**"loading MIB failed: … At line N in …".** Net-SNMP's parser found a syntax error; the message names
file and line. A frequent cause: `--` inside a comment line ends the comment (ASN.1 rule), e.g.
`-- see option --verbose` – write `- -verbose` or rephrase.

**"Cannot find module (XYZ-MIB)".** An imported module is not on the search path. Put the MIB next
to the one you load, or pass its directory: `MIB_DIRS` in `snmpwrap_add_mib`, `--mib-dir` for
`snmpwrap-mibgen`, the second argument of `MibModel::load`, `-M` for `client_cli`.

**After changing the MIB the build fails in my agent.** That is intended: the generated
`Instrumentation` changed (e.g. a new column = a new pure virtual method, a changed type = a changed
signature). Implement or adapt the methods the compiler lists.

**My changes in `generated/…` are gone.** Generated files are overwritten whenever the MIB changes –
put your code into your own class that implements `Instrumentation`, never into the generated files.

**"MIB name 'x' is ambiguous; use MODULE::x".** Two loaded modules define the same descriptor;
write `MODULE::x` (e.g. `SNMPWRAPPER-TEST-MIB::swtLimit`).

**"MIB objects without binding below …" (`MibBinder::finish`).** The MIB defines objects you have not
bound yet; bind them, or call `finish(true)` if serving only part of the MIB is intended.

**A DEFVAL is not applied on row creation.** Numbers, enumeration labels, strings, hex / binary strings
(`'00FF'H`, `'0101'B`), IP addresses and OIDs of known names are converted. DEFVALs that cannot be
converted (e.g. an OID value whose name is not loaded, or BITS given as `{ bitA, bitB }`) are skipped;
the generated code contains a comment at that place. In the run-time binder (`MibBinder`) such a column
is treated like one without a default, i.e. it becomes a required column of `createAndGo`.

---

## 19. Building your own application (step by step)

This chapter is a checklist for building a **separate agent application** and a **separate client
application** that use snmpwrap. Both are complete, working programs in
[examples/apps/](../examples/apps): `agent_app` publishes data, `client_app` reads and changes it.
Each one is a stand-alone CMake project – copy a folder and you have the skeleton of your own program.

```
examples/apps/
  mibs/MY-APP-MIB.txt     the interface both programs share
  agent_app/              CMakeLists.txt + main.cpp   (publishes the data)
  client_app/             CMakeLists.txt + main.cpp   (reads / changes the data)
```

### 19.1 Prerequisites (once)

1. Net-SNMP 5.9.x with its development files (`sudo apt install libsnmp-dev snmpd snmp`, or your own
   build in a custom prefix).
2. Build and install snmpwrap:

   ```sh
   cmake --preset debug && cmake --build --preset debug
   cmake --install build/debug --prefix $HOME/snmpwrap-install
   ```

3. A running `snmpd` with the AgentX master enabled – only the agent application needs it
   ([section 1](#1-how-it-works--the-big-picture)). Minimal `snmpd.conf`:

   ```
   agentaddress udp:127.0.0.1:161
   rocommunity public  127.0.0.1
   rwcommunity private 127.0.0.1
   master agentx
   agentXSocket tcp:127.0.0.1:705
   ```

### 19.2 Step 1 – describe your data in a MIB

Everything your application publishes is described once, in a MIB file
([examples/apps/mibs/MY-APP-MIB.txt](../examples/apps/mibs/MY-APP-MIB.txt)): object names, types,
ranges, read-only or read-write, and notifications. Agent and client are generated from the same file, so
they can never disagree.

```
appName        DisplayString (SIZE (1..32))  read-write   { myAppMIB 1 }
appTemperature Integer32 (-50..150)          read-only    { myAppMIB 2 }
appLimit       Integer32 (0..100)            read-write   { myAppMIB 3 }
appLimitExceeded NOTIFICATION-TYPE  OBJECTS { appTemperature }
```

* **Use your own enterprise number** instead of the placeholder `99999`.
* **The file extension does not matter.** `.txt`, `.mib`, `.my` or no extension all work when the file is
  passed to snmpwrap directly (tested with the generator); what counts is the module name inside the file
  (`MY-APP-MIB DEFINITIONS ::= BEGIN`).
* Imported modules (`SNMPv2-SMI`, `SNMPv2-TC`, …) are found in Net-SNMP's own MIB directory. Your own
  imported MIB files go into a directory that you pass with `MIB_DIRS` (see below).

### 19.3 Step 2 – CMake: let the build generate the C++ code

The same `CMakeLists.txt` works for the agent and for the client (only the program name differs):

```cmake
cmake_minimum_required(VERSION 3.16)
project(agent_app CXX)

find_package(snmpwrap REQUIRED)            # provides snmpwrap::snmpwrap and snmpwrap_add_mib()

add_library(my_app_mib STATIC)             # MIB -> C++ at build time, again whenever the MIB changes
snmpwrap_add_mib(my_app_mib MODULE MY-APP-MIB MIB ${CMAKE_CURRENT_SOURCE_DIR}/../mibs/MY-APP-MIB.txt)
# imports from your own MIB files:  snmpwrap_add_mib(... MIB a.txt MIB_DIRS ${CMAKE_SOURCE_DIR}/mibs)

add_executable(agent_app main.cpp)
target_link_libraries(agent_app PRIVATE my_app_mib)    # brings snmpwrap::snmpwrap along
```

Configure it and point CMake to the install prefix of snmpwrap (and to Net-SNMP if it is not in a
system location):

```sh
cmake -S examples/apps/agent_app -B build-agent -DCMAKE_PREFIX_PATH="$HOME/snmpwrap-install;$HOME/netsnmp"
cmake --build build-agent
```

If you embed snmpwrap as a subdirectory instead of installing it, replace `find_package(snmpwrap REQUIRED)`
with `add_subdirectory(path/to/SnmpWrapper)`; `snmpwrap_add_mib()` is available there as well.

The build creates `my_app_mib.hpp/.cpp` (namespace `my_app_mib`, the module name in snake_case). It contains:

| Generated | Use |
|---|---|
| `Instrumentation` | the interface the **agent** implements: `appName()`, `setAppName(...)`, `validateAppLimit(...)`, … |
| `registerMib(agent, impl)` | registers all objects with the master agent |
| `sendAppLimitExceeded(agent, temperature)` | sends the notification, typed |
| `Client` | typed access for the **client**: `appName()`, `setAppLimit(...)`, … |
| `oids::root`, `oids::appLimit`, … | the OIDs, if you ever need them |

### 19.4 Step 3 – the agent application

Full program: [examples/apps/agent_app/main.cpp](../examples/apps/agent_app/main.cpp). The parts you write:

**a) Your data and a class that implements `Instrumentation`.** One method per MIB object. Read methods
are called on every GET / walk. Write methods are called on SNMP SET – **that is your "value changed"
event**; the MIB checks (range, size) have already passed. `validate…` is an optional extra rule.

```cpp
class MyApp : public my_app_mib::Instrumentation {
public:
    std::int32_t appLimit() override { std::lock_guard<std::mutex> l(m_); return limit_; }          // GET
    void setAppLimit(std::int32_t v) override { std::lock_guard<std::mutex> l(m_); limit_ = v; }    // SET
    void validateAppLimit(std::int32_t v) override {                                                 // optional
        if (v < 10) throw snmpwrap::SetError(snmpwrap::ErrorStatus::WrongValue, "limit below 10");
    }
    // ... appName(), setAppName(), appTemperature() ...
private:
    std::mutex m_;       // your other threads and the SNMP callbacks share this data
    std::int32_t limit_ = 30;
};
```

**b) Create the agent, register, run the loop.**

```cpp
snmpwrap::AgentConfig config;
config.name = "agent_app";
config.agentxSocket = "tcp:127.0.0.1:705";      // must match 'agentXSocket' in snmpd.conf

snmpwrap::Agent agent(config);                  // only ONE Agent per process
MyApp app;
my_app_mib::registerMib(agent, app);            // `app` must outlive the agent

while (agent.poll()) {                          // answers SNMP requests; returns at least once per second
    std::int32_t temperature;
    if (app.takeAlarm(temperature))             // set by your own thread
        my_app_mib::sendAppLimitExceeded(agent, temperature);   // push a notification to the managers
}
```

**c) Stop cleanly.** `agent.stop()` is the one call that is safe from a signal handler or another thread:

```cpp
snmpwrap::Agent* g_agent = nullptr;
void onSignal(int) { if (g_agent) g_agent->stop(); }
// g_agent = &agent;  std::signal(SIGINT, onSignal);  std::signal(SIGTERM, onSignal);
```

**Rules for the agent application** (details in [section 12](#12-the-main-loop-threads-and-shutdown)):

* `poll()`, `registerMib()` and `sendTrap()` / `send…()` belong to **one thread**. Your callbacks run on
  that thread too.
* Other threads change the data only under the mutex. They **never** call snmpwrap; they set a flag or put an
  event into a queue, and the loop above sends the notification (that is what `takeAlarm()` does).
* There is no "subscribe" API: the set callback is where you react to a manager's change, the
  notification is how you tell managers about yours.

### 19.5 Step 4 – the client application

Full program: [examples/apps/client_app/main.cpp](../examples/apps/client_app/main.cpp). It does **not**
need `snmpd` on its own side – it only talks to some agent.

```cpp
snmpwrap::SessionConfig cfg;
cfg.peer = "127.0.0.1:161";                  // host[:port] of the device (the snmpd that serves the agent)
cfg.community = "private";                   // SNMPv2c; for SNMPv3 see section 13 / 15
cfg.timeout = std::chrono::milliseconds(2000);
cfg.retries = 1;

snmpwrap::Client session(cfg);               // one Client per thread
my_app_mib::Client app(session);             // typed access, generated from the MIB

std::cout << app.appName() << " " << app.appTemperature() << "\n";   // GET
app.setAppLimit(35);                                                  // SET
for (const snmpwrap::VarBind& vb : session.walk(my_app_mib::oids::root))   // walk with plain OIDs
    std::cout << vb.oid.str() << " = " << vb.value.str() << "\n";
```

Three kinds of errors, each with its own exception type:

| Exception | Meaning | Example in `client_app` |
|---|---|---|
| `snmpwrap::TransportError` | no answer, wrong community / SNMPv3 credentials, network | wrong address or `snmpd` not running |
| `snmpwrap::ResponseError` | the agent answered with an error (`status()`, `index()`) | `setAppLimit(5)` → the agent's rule says ≥ 10 → `wrongValue` |
| `snmpwrap::SetError` | the MIB check failed locally, nothing was sent | `setAppLimit(500)` → outside 0..100 |

### 19.6 Step 5 – run it

Three terminals:

```sh
snmpd -f -C -c snmpd.conf -Lo                      # 1. the master agent (see 19.1)
build-agent/agent_app tcp:127.0.0.1:705            # 2. the agent application
build-client/client_app 127.0.0.1:161 private      # 3. the client application
```

Expected output of the client:

```
appName        = my-app
appTemperature = 23 degrees Celsius
appLimit       = 30 degrees Celsius
appLimit is now 35
rejected before sending: appLimit: value must be in 0..100
agent refused: agent returned error: wrongValue (...) (status 10)

walk of 1.3.6.1.4.1.99999.200:
  1.3.6.1.4.1.99999.200.1.0 = OctetString: "my-app"
  ...
```

To see the notification arrive, add `trap2sink 127.0.0.1:11163 public` to `snmpd.conf` and start
`snmptrapd -f -C -c snmptrapd.conf -Lo udp:127.0.0.1:11163` (with `authCommunity log public` in its config),
then lower the limit (`snmpset … appLimit.0 i 12`). The agent sends `appLimitExceeded` whenever the temperature
is above it.

### 19.7 Common mistakes

* **Agent and client in one program:** create the `Agent` first, and call the client from the **agent's
  thread** only. A client that asks for an OID the same program's agent serves times out, because the thread is
  blocked in the client call and cannot answer the request that `snmpd` forwards to it. Use two
  programs (as here), or query a different device.
* **`No Such Object` for everything:** the agent is not registered. Check that `snmpd.conf` has `master agentx`,
  that `agentXSocket` matches `AgentConfig::agentxSocket`, and that the agent printed
  `AgentX subagent connected`.
* **`noAccess` on SET:** the community has no write permission in `snmpd.conf` (`rwcommunity`). That answer
  comes from `snmpd`, not from your code.
* **Build error "snmpwrap not found":** `CMAKE_PREFIX_PATH` has to contain the snmpwrap install prefix.
* **Several agents on the same OID:** only one program can register a subtree; the second one fails to register.
