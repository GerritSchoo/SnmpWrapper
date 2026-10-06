# Changelog

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
