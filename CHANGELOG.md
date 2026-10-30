# Changelog

## Unreleased

### Changed
- Tests run on GoogleTest / gmock (system package `libgtest-dev libgmock-dev`), one CTest entry per test, laid out like
  the sources: `test/unit/` (library), `test/generated/` (generated code, with gmock for `Session`,
  `NotificationSender`, `NotificationSource`), `test/mibgen/` (generator and command line, new). `ctest -L unit` runs
  everything without snmpd.
- Tests are switched with CTest's standard `BUILD_TESTING` (`-DBUILD_TESTING=OFF`) instead of `SNMPWRAP_BUILD_TESTS`.
- The generator is built as the internal library `snmpwrap_mibgen_core` plus the `snmpwrap-mibgen` executable.

### Documentation
- Guide: new section 11 "Everything at a glance" – every function of the agent and the client side, plus an
  "I want to …" table. New: `Instrumentation` / `registerMib()`, decoding the row index in the hooks, `AgentConfig`,
  the flat `mib::Client`, "Agent and client in one program" (rules and a gateway example), ending an adapter at run time.
  Fixed a link to a test file that no longer exists.
- The comment at the top of every generated header names all parts (it mentioned only `Instrumentation` and `Client`).

### Added
- `SessionConfig::localAddress`: a client's own source address ("192.168.1.5" or "192.168.1.5:5000", IPv6 as
  "[fe80::1]:5000") for hosts with several interfaces. UDP only (refused with a TCP peer); an address that is not this
  host's, or a port in use, is a `TransportError`. Set for that one `Client` only.
- Guide section 8: where every address and port is set (agent, snmpd, client, receiver), what snmpd.conf must contain and
  what it can contain (access, v3 users, views, notification destinations, AgentX timeouts), running snmpd without root.
- `Agent::removeHandler(root)` / `Agent::removeMib(mib)`: remove a subtree at run time. It is answered as non-existent at
  once; Net-SNMP / snmpd are told right away, or - from inside a callback or while a SET for it is in progress - on the
  next `poll()` once that is finished.
- Preset `asan` (AddressSanitizer + UndefinedBehaviorSanitizer) for all tests; Net-SNMP-internal leaks are suppressed
  in `test/sanitizer/lsan.supp`.
- Preset `coverage` and `scripts/coverage.sh`: line / branch coverage over all tests, HTML report in
  `build/coverage/html/`.

### Fixed
- `DataAgent`: destroying it while the `Agent` keeps polling no longer leads to a use-after-free. The Agent kept the
  registered Mib, whose callbacks pointed into the freed adapter. Now `~DataAgent` removes its subtree from the Agent
  (managers get noSuchObject), and requests that still reach the Mib - a SET in progress, or `DataAgent(Mib&, Data&)` -
  fail with an error instead of touching the adapter or the `Data`.
- `Mib::getNext()` and `Client` leaked an OID when a getter threw (or a received value had an unsupported type):
  GCC < 13 does not destroy the finished members of an aggregate when a later initializer throws (PR 66139).
- Generator: a `--root` / `ROOT` that does not lie above every object of the module (e.g. a group inside it) is
  refused when generating. Before, the code was generated without a message; `registerMib()` / `DataAgent` then threw
  at run time, and `Remote::read()` silently missed the objects outside the root.
- BITS values are sent as OCTET STRING (RFC 2578 7.1.4), no longer as the SMIv1 BIT STRING. Serving a BITS object
  broke the AgentX session (AgentX has no BIT STRING type), and SETs from standard managers (`snmpset … b`) were
  refused with wrongType. Received OCTET STRINGs are accepted wherever BITS are declared (`Value::fits()`).
- `DataAgent`: rolling back a SET request restores the old values without calling `onSet` again. Before, a hook that
  refused the old value left the new value in `Data` although the manager got an error, and a rolled-back row creation
  was reported to `onSet` as a destroy.
- `DataAgent`: columns supplied by a rolled-back request no longer count for the completeness of a RowStatus row
  (an incomplete row could be activated).
- `Client` / `Session`: an answer without varbinds, or with a different number than requested (GET, GETNEXT, SET),
  throws `Error` instead of leading to undefined behaviour.

### Added
- `Value::fits(Type)` – whether a value can stand for an object of a declared type (OctetString for Bits).
- `Mib::onUndo()` – start of the rollback of a SET request.
- Generator: `operator<<` for `Data`, every nested group, table, row, row index and enum – `std::cout << data;` prints
  every value in MIB order, groups and rows indented (enums as `label(number)`, texts quoted, bytes in hex, BITS by name).
- `NotificationReceiver::acceptCommunities()` – drop notifications with other communities (and do not acknowledge
  their informs). Without it, every notification is accepted as before.

## 0.2.0

One way to use the library: the MIB describes the data, the build generates the C++ types.

### Added
- Generator: `struct Data` – the module as one nested C++ structure (groups → nested structs, scalars → members,
  tables → row containers in SNMP order, `table[n]` for single-number indexes), `Data::validate()`.
- Generator: `DataAgent` serves a `Data` through an `Agent`: GET/GETNEXT read it, SET is checked against the MIB and
  writes it, RowStatus rows are created / destroyed in the container; hooks `onSet` (react to or refuse changes) and
  `onGet` (compute values on read); `lock()` for other threads.
- Generator: `Remote` – the same nesting on the client side: `get()` / `set()` per scalar and cell (MIB check before
  sending), `table[index]`, `read()`, `create()` / `destroy()` for RowStatus tables.
- Messages: `DataAgent` reports changes after the commit, once per request – per value (`onChanged`), per row
  (`on<Table>Row`), per subtree (`on<Group>`) and for the whole MIB; `enum class Object` names the objects in the hooks.
  `Remote` sends a subtree, a row or the whole MIB as one request (`send()`), any values together with `change()`,
  and reads everything into a `Data` (`read()`).
- `NotificationReceiver` (v1/v2c traps and informs) and generated typed handlers (`Notifications::on<Name>()`).
- Interfaces with pure virtual functions for mocking in application tests: `Session` (implemented by `Client`),
  `NotificationSender` (`Agent`), `NotificationSource` (`NotificationReceiver`); the generated `Remote`, `send<Name>()`
  and `Notifications` take these interfaces.
- `Mib::onRequestEnd()` – end of a SET request (committed or rolled back).
- `Value::opaque()` / `Value::bits()` with their own types (`Type::Opaque`, `Type::Bits`).
- SNMPv3 client: SHA-224/256/384/512 and AES-192/256 (when Net-SNMP supports them).
- `RowStatusSpec::rejectEditWhileActive` (RFC 2579 rule, off by default).
- Examples `examples/apps/agent_app` and `client_app`, stand-alone CMake projects, run by the new test `apps`.

### Changed
- Opaque and BITS values keep their ASN.1 type; getters must return them with `Value::opaque` / `Value::bits`.
- A row index of type Counter32 / TimeTicks is accepted by `IndexKind::Unsigned`.
- Library sources moved to `src/snmpwrap/`; build output in `build/<preset>/` inside the project.
- Examples and the guide show only the generated `Data` / `Remote` way; the core API is described in an appendix.

### Removed
- `MibBinder` (binding by name at run time) and `MibModel::loadModules()`, `modules()`, `nodes()`.
- Examples `simple_agent`, `simple_client`, `mib_agent`, `mib_client`, the demo MIB and the checked-in `generated/` code.
- Installing the example MIB files.

## 0.1.0
- First version: AgentX subagent (`Agent`, `Mib`, `Handler`), `Client`, `MibModel`, `MibBinder`, generator with
  `Instrumentation` and a typed `Client`.
