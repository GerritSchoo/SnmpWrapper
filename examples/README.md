# Examples

| Program | What it shows |
|---|---|
| [simple_agent.cpp](simple_agent.cpp) | A small agent built with the core API: scalars (read-only and read-write with validation), a table, a notification, its own main loop. **Start here.** |
| [simple_client.cpp](simple_client.cpp) | A small client: GET, walking a table, atomic multi-value SET, rejected SET, missing instance, SNMPv2c and SNMPv3. **Start here.** |
| [mib_agent.cpp](mib_agent.cpp) | The same device built **from its MIB** ([SNMPWRAPPER-DEMO-MIB](../mibs/SNMPWRAPPER-DEMO-MIB.txt)): CMake generates the code, the program only implements the generated `Instrumentation` interface. **Recommended for products.** |
| [mib_client.cpp](mib_client.cpp) | Client using the MIB: the generated typed client, plus names / readable output with `MibModel` at run time. |
| [test_agent.cpp](test_agent.cpp) | Implements the complete example MIB `mibs/SNMPWRAPPER-TEST-MIB.txt` with the core API: RowStatus, composite indexes, large tables, Counter64. Used by the integration test. |
| [client_cli.cpp](client_cli.cpp) | Command line client (`get`, `getnext`, `getbulk`, `walk`, `set`) for v1, v2c and v3; with `-m <mib>` names and MIB-typed values. |

All programs are built together with the library (`cmake --build build`) and end up in `build/examples/`.
`simple_*` and `mib_*` use the same OIDs, so every client works with every agent.

## Running simple_agent and simple_client

The agent is an AgentX subagent, so `snmpd` has to run as master agent.

**1. Configure snmpd** – add to `/etc/snmp/snmpd.conf` and restart it (`sudo systemctl restart snmpd`):

```
master agentx
agentXSocket tcp:127.0.0.1:705
rocommunity public  127.0.0.1
rwcommunity private 127.0.0.1
# optional, for the SNMPv3 variant of simple_client:
createUser admin SHA authpass123
rwuser admin auth
# optional, to receive the alarm notification:
trap2sink 127.0.0.1 public
```

**2. Start the agent** (terminal 1):

```sh
$ build/examples/simple_agent tcp:127.0.0.1:705
NET-SNMP version 5.9.1 AgentX subagent connected
simple_agent: serving 1.3.6.1.4.1.99999.100 via tcp:127.0.0.1:705 (Ctrl+C to stop)
```

**3. Run the client** (terminal 2):

```sh
$ build/examples/simple_client 127.0.0.1:161          # SNMPv2c, communities public / private

=== GET scalars ===
1.3.6.1.4.1.99999.100.1.0 = OctetString: "demo-device"
1.3.6.1.4.1.99999.100.2.0 = TimeTicks: 151
1.3.6.1.4.1.99999.100.3.0 = Integer: 40

=== WALK sensorTable ===
index  name      value   enabled
1      cpu       33 C    yes
2      board     30 C    yes
3      psu       30 C    no

=== SET alarmThreshold = 30 and disable sensor 2 (one atomic request) ===
alarmThreshold is now 30
sensor 2 enabled:     2 (2 = false)

=== SET alarmThreshold = 150 (out of range) ===
rejected by the agent: agent returned error: wrongValue (The set value is illegal or unsupported in some way) (error-status 10, varbind 1)
alarmThreshold is still 30

=== SET deviceName to a number (wrong type) ===
rejected by the agent: agent returned error: wrongType (The set datatype does not match the data type the agent expects)

=== GET sensorName.9 (no such row) ===
1.3.6.1.4.1.99999.100.4.1.2.9: NoSuchInstance

=== done (changes restored) ===
```

SNMPv3 instead of v2c:

```sh
$ build/examples/simple_client 127.0.0.1:161 --v3 admin authpass123
```

**4. Look at the same data with the Net-SNMP tools:**

```sh
$ snmpwalk -v2c -c public -On localhost .1.3.6.1.4.1.99999.100
.1.3.6.1.4.1.99999.100.1.0 = STRING: "demo-device"
.1.3.6.1.4.1.99999.100.2.0 = Timeticks: (153) 0:00:01.53
.1.3.6.1.4.1.99999.100.3.0 = INTEGER: 40
.1.3.6.1.4.1.99999.100.4.1.2.1 = STRING: "cpu"
.1.3.6.1.4.1.99999.100.4.1.2.2 = STRING: "board"
.1.3.6.1.4.1.99999.100.4.1.2.3 = STRING: "psu"
.1.3.6.1.4.1.99999.100.4.1.3.1 = INTEGER: 33
...
$ snmpset -v2c -c private localhost .1.3.6.1.4.1.99999.100.3.0 i 30     # lower the alarm threshold
```

With a threshold below the sensor values, the agent sends a `sensorAlarm` notification every
10 seconds for each enabled sensor that is too warm; with `trap2sink` configured, `snmptrapd`
shows it:

```
snmpTrapOID.0 = OID: enterprises.99999.100.0.1   enterprises.99999.100.4.1.2.1 = STRING: "cpu"   enterprises.99999.100.4.1.3.1 = INTEGER: 33
```

The output above was recorded with Net-SNMP 5.9.1 on Ubuntu 22.04; uptime and sensor values differ on every run.
For a full explanation of the API see [docs/GUIDE.md](../docs/GUIDE.md).

## Running mib_agent and mib_client (code generated from the MIB)

`mib_agent` serves exactly the data of `simple_agent`, but everything about it comes from
[mibs/SNMPWRAPPER-DEMO-MIB.txt](../mibs/SNMPWRAPPER-DEMO-MIB.txt). The build generates the code into
[generated/snmpwrapper_demo_mib/](../generated/snmpwrapper_demo_mib) (library `snmpwrapper_demo_mib`,
defined in [generated/CMakeLists.txt](../generated/CMakeLists.txt)); both programs just link it:

```cmake
target_link_libraries(mib_agent  PRIVATE snmpwrapper_demo_mib)
target_link_libraries(mib_client PRIVATE snmpwrapper_demo_mib)
```

Open `generated/snmpwrapper_demo_mib/snmpwrapper_demo_mib.hpp` to see the interface `mib_agent.cpp` implements.

With snmpd configured as above:

```sh
$ build/examples/mib_agent tcp:127.0.0.1:705          # terminal 1
mib_agent: serving SNMPWRAPPER-DEMO-MIB (1.3.6.1.4.1.99999.100) via tcp:127.0.0.1:705 (Ctrl+C to stop)
NET-SNMP version 5.9.1 AgentX subagent connected

$ build/examples/mib_client 127.0.0.1:161             # terminal 2

=== typed access (generated client) ===
deviceName     = demo-device
alarmThreshold = 40 degrees Celsius

index  name    value   enabled
1      cpu     33 C    true
2      board   30 C    true
3      psu     30 C    false

=== typed SET ===
alarmThreshold is now 30
rejected locally by the MIB check: alarmThreshold: value must be in 0..100
sensor 3 enabled: true

=== generic walk, formatted with the MIB ===
deviceName.0 = "demo-device"
deviceUptime.0 = (152) 0:00:01.52
alarmThreshold.0 = 40 degrees Celsius
sensorName.1 = "cpu"
...
sensorEnabled.3 = false(2)

=== names instead of OIDs ===
sensorName.2 -> 1.3.6.1.4.1.99999.100.4.1.2.2 = "board"
set alarmThreshold.0 = 35 -> alarmThreshold.0 = 35 degrees Celsius
```

The Net-SNMP tools show the same data with names when they are given the MIB:

```sh
$ snmpwalk -v2c -c public -M +mibs -m +SNMPWRAPPER-DEMO-MIB localhost SNMPWRAPPER-DEMO-MIB::sensorTable
SNMPWRAPPER-DEMO-MIB::sensorName.1 = STRING: cpu
...
SNMPWRAPPER-DEMO-MIB::sensorEnabled.3 = INTEGER: false(2)
```

Edit the MIB (for example add a column to `sensorTable`) and rebuild: the generated interface
changes and the compiler shows where `mib_agent.cpp` has to be extended.
