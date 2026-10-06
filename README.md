# snmpwrap

C++17-Wrapper um Net-SNMP 5.9.x (reines C): **Agent** (AgentX-Subagent) und ein schlanker **Client**.
Die **MIB-Datei ist die Schnittstellenbeschreibung**: Der Code-Generator `snmpwrap-mibgen` erzeugt daraus beim Build
typisierten C++-Code (Agent-Interface, Registrierung, Notifications, typisierter Client). Wer will, kann OIDs auch
direkt im Code beschreiben (Kern-API) oder eine MIB zur Laufzeit laden.
Net-SNMP selbst wird unverändert verwendet.

**Ausführliche Anleitung (Englisch) mit Beispielen: [docs/GUIDE.md](docs/GUIDE.md).**
Die API-Referenz steht als Doxygen-Kommentare in `include/snmpwrap/*.hpp`.

## Bauen (Linux / WSL)

```sh
sudo apt install build-essential cmake libsnmp-dev snmpd snmp     # Ubuntu 22.04 liefert Net-SNMP 5.9.1
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure                         # Unit-, Lifecycle- + Integrationstest
```
Liegt Net-SNMP in einem eigenen Prefix: `-DCMAKE_PREFIX_PATH=<prefix>` (gefunden wird `net-snmp-config`).

## Selbst bauen und testen (WSL, aus VS Code)

Die Quellen liegen unter Windows (`C:\Users\gerri\Projects\SnmpWrapper`), gebaut wird **in WSL/Ubuntu**:
dort sind Compiler, CMake/Ninja (`~/.local/bin`) und Net-SNMP (`~/netsnmp` bzw. per apt). Ubuntu sieht den
Projektordner als `/mnt/c/Users/gerri/Projects/SnmpWrapper`; die Build-Ausgabe landet in `build/<preset>`
(Linux-Dateisystem, schnell). Die Presets stehen in [CMakePresets.json](CMakePresets.json): `debug`, `release`, `shared`.

**Weg 1 – VS Code mit WSL verbinden (empfohlen):**
1. In VS Code die Extension **WSL** installieren (wird als Empfehlung angezeigt, siehe `.vscode/extensions.json`).
2. `F1` → **„WSL: Reopen Folder in WSL“**. Unten links steht dann „WSL: Ubuntu“ – Terminal, Compiler, CMake und
   IntelliSense sind jetzt die Linux-Versionen. (Alternativ im Ubuntu-Terminal: `cd /mnt/c/Users/gerri/Projects/SnmpWrapper && code .`)
3. Die vorgeschlagenen Extensions **CMake Tools** und **C/C++** „in WSL“ installieren.
4. In der Statusleiste das Preset **„Debug (Linux / WSL)“** wählen, dann **Build** (F7). Tests im Testing-Panel oder per
   „Run CTest“. Zum Debuggen in WSL einmal `sudo apt install gdb`.

**Weg 2 – VS Code bleibt unter Windows, nur die Tasks laufen in WSL:**
`Strg+Umschalt+B` → „snmpwrap: build“; *Terminal → Run Task…* → „snmpwrap: test (all)“, „… test (unit + MIB only)“ oder
„… demo (snmpd + mib_agent + mib_client)“. Die Tasks rufen `wsl -d Ubuntu -- bash scripts/….sh` auf
([.vscode/tasks.json](.vscode/tasks.json)).

**Weg 3 – von Hand im Ubuntu-Terminal** (Startmenü „Ubuntu“, `wsl` in PowerShell, oder in VS Code im Terminal-Menü
„Ubuntu (WSL)“ wählen):

```sh
cd /mnt/c/Users/gerri/Projects/SnmpWrapper
cmake --preset debug                  # konfigurieren
cmake --build --preset debug          # bauen (Library, Generator, Beispiele, Tests, generated/)
ctest --preset debug                  # alle Tests (inkl. Integration gegen einen eigenen snmpd, kein root nötig)
ctest --preset debug -R mib_model     # nur bestimmte Tests

# oder mit den Skripten:
scripts/build.sh [debug|release|shared]
scripts/test.sh  [preset] [ctest-Optionen]
scripts/demo.sh  [mib_agent|simple_agent|test_agent] [mib_client|simple_client|none]   # snmpd + Agent + Client

# Programme liegen danach in build/debug/, z. B.
build/debug/examples/mib_agent tcp:127.0.0.1:705
build/debug/snmpwrap-mibgen --module MEIN-MIB --out generated/mein_mib mibs/MEIN-MIB.txt
```

Ohne eigenes Net-SNMP in `~/netsnmp` genügt `sudo apt install build-essential cmake ninja-build libsnmp-dev snmpd snmp gdb`.

## In eigenen Projekten verwenden

```cmake
# a) als Unterverzeichnis / FetchContent
add_subdirectory(external/SnmpWrapper)
# b) oder installiert:  cmake --install build --prefix /opt/snmpwrap
#    find_package(snmpwrap REQUIRED)   (CMAKE_PREFIX_PATH=/opt/snmpwrap)
target_link_libraries(my_app PRIVATE snmpwrap::snmpwrap)
```
Die öffentlichen Header (`snmpwrap/*.hpp`) enthalten keine Net-SNMP-Header; Net-SNMP wird privat gelinkt.
Statische und Shared-Library-Builds (`-DBUILD_SHARED_LIBS=ON`) werden unterstützt.

## Von der MIB zum Code (empfohlen)

```cmake
add_executable(my_agent main.cpp)
snmpwrap_add_mib(my_agent MODULE SNMPWRAPPER-DEMO-MIB MIB mibs/SNMPWRAPPER-DEMO-MIB.txt)   # erzeugt snmpwrapper_demo_mib.hpp/.cpp
```

```cpp
#include "snmpwrapper_demo_mib.hpp"            // generiert, wird bei MIB-Änderung automatisch neu erzeugt
using namespace snmpwrapper_demo_mib;

class Device : public Instrumentation {        // eine Methode pro MIB-Objekt, C++-Typen aus der MIB
    std::int32_t alarmThreshold() override { return t_; }
    void setAlarmThreshold(std::int32_t v) override { t_ = v; }   // Bereich 0..100 bereits geprüft
    std::vector<SensorEntryIndex> sensorTableRows() override { ... }
    TruthValue sensorEnabled(const SensorEntryIndex& i) override { ... }
    ...
};

snmpwrap::Agent agent;
Device device;
registerMib(agent, device);                    // registriert alle Objekte der MIB
sendSensorAlarm(agent, "cpu", 85, SensorEntryIndex{1});        // typisierte Notification
```

* Ändert sich die MIB, meldet der **Compiler**, wo der Code nicht mehr passt (neue Spalte = neue Methode).
* Typ, Bereiche, SIZE, Enums, Index-Aufbau, RowStatus und DEFVALs kommen aus der MIB; die Prüfungen werden generiert.
* Zur Laufzeit wird die MIB-Datei **nicht** gebraucht; der generierte Code nutzt nur die snmpwrap-API.
* Client-Seite: generierter typisierter `Client` (`device.alarmThreshold()`, `device.sensorTable()` → `std::map`) und
  `MibModel` für Namen, lesbare Ausgabe und Eingabeprüfung (`client_cli -m <mib> … get sensorEnabled.1`).
* Laufzeit-Bindung per Name (`MibBinder`) gibt es zusätzlich für Simulatoren/Tests.

Der generierte Code für die beiden Beispiel-MIBs liegt lesbar in [generated/](generated) – der Build schreibt ihn dorthin
(eine Library pro MIB) und aktualisiert ihn bei jeder MIB-Änderung; nicht von Hand bearbeiten.

Details: [docs/GUIDE.md, Kapitel 5](docs/GUIDE.md#5-working-from-the-mib-file-recommended); Beispiele:
[examples/mib_agent.cpp](examples/mib_agent.cpp), [examples/mib_client.cpp](examples/mib_client.cpp).

## Agent (Kern-API, ohne MIB)

Voraussetzung: `snmpd` läuft mit `master agentx` (und z. B. `agentXSocket tcp:127.0.0.1:705`) in der `snmpd.conf`.

```cpp
#include <snmpwrap/agent.hpp>
using namespace snmpwrap;

Agent agent;                                        // AgentConfig: Name, AgentX-Socket, ...
Mib& mib = agent.addMib(Oid::parse("1.3.6.1.4.1.99999"));

int limit = 50;
mib.scalar({1, 4}, {Type::Integer,                  // -> 1.3.6.1.4.1.99999.1.4.0
    [&] { return Value::integer(limit); },          // get
    [&](const Value& v) { limit = v.asInt(); },     // set (optional, sonst read-only)
    [](const Value& v) { if (v.asInt() > 100) throw SetError(ErrorStatus::WrongValue); }});  // validate (optional)

TableDef t;                                         // -> 1.3.6.1.4.1.99999.2.1.<col>.<index>
t.indexes = {IndexSpec::integer()};                 // INDEX-Aufbau (optional, aktiviert strikte Index-Prüfung)
t.columns = {{2, Type::OctetString, Access::ReadWrite}, {3, Type::Gauge32}};
t.rows = [&] { return std::vector<Oid>{indexInt(1), indexInt(2)}; };
t.get  = [&](const Oid& index, SubId col) { /* Wert für Zeile/Spalte */ };
t.set  = [&](const Oid& index, SubId col, const Value& v) { /* schreiben */ };
mib.table(2, std::move(t));

agent.run();                                        // oder: while (agent.poll()) { ...eigene Arbeit... }
```

**Tabellen**
* Zeilenindex = `Oid`. Mit `TableDef::indexes` (`IndexSpec::integer/unsignedInt/string/impliedString/fixedString/ipAddress/objectId`)
  wird er strikt geprüft; `encodeIndex`/`decodeIndex` wandeln zwischen Indexwerten und OID (mehrspaltig, IMPLIED).
* Kleine Tabellen: `rows()` liefern. Große Tabellen: `nextRow(after)` + `hasRow(index)` – GETNEXT/GETBULK sind dann
  O(log n), und `rows()` wird nie aufgerufen (getestet mit 100 000 Zeilen).
* **RowStatus** (RFC 2579): `TableDef::rowStatus` mit `create/destroy/setState/state/complete` und `requiredColumns`.
  Die Zustandsmaschine (createAndGo, createAndWait → notReady/notInService, active, destroy, Fehlercodes) steckt im
  Wrapper; die Anwendung speichert nur Zeilen. Die RowStatus-Spalte nicht in `columns` aufführen.

**SET** läuft als Transaktion über alle Varbinds eines Requests (Net-SNMP-Phasen RESERVE1 → ACTION → COMMIT / UNDO):
Alles wird zuerst geprüft (`validate`, Typ, Zugriff, RowStatus), dann geschrieben. Scheitert ein Schreibvorgang
(z. B. `SetError(ErrorStatus::CommitFailed)`), werden alle bisherigen Änderungen und angelegten Zeilen zurückgerollt.
Löschungen passieren erst in COMMIT, weil sie nicht rückgängig zu machen sind. Fehler werden dem richtigen Varbind
zugeordnet (errindex).

**GET-Semantik:** `noSuchObject` (kein Objekt passt) vs. `noSuchInstance` (Objekt bekannt, Instanz/Zeile fehlt) nach
RFC 3416; für SNMPv1-Manager macht Net-SNMP daraus `noSuchName`.

* Für volle Kontrolle `Handler` selbst implementieren (`get`, `getNext`, `missing`, `prepare` → `SetTransaction`) und
  mit `agent.addHandler(root, handler)` registrieren.
* **Threading / Lebensdauer:** Net-SNMP hat globalen Zustand → ein `Agent` pro Prozess, alle Aufrufe im Loop-Thread;
  nur `stop()` ist thread-/signalsicher. Die Callbacks laufen im Loop-Thread; eigene Daten bei Zugriff aus anderen
  Threads per Mutex schützen. Agent und Client im selben Prozess: **den Agent zuerst erzeugen** (sonst wirft der
  Konstruktor – Net-SNMP könnte die AgentX-Verbindung nicht mehr aufbauen) und Clients nicht nach dem Agent weiterverwenden.

**SNMP-Versionen:** v1, v2c und v3 werden vom Master (`snmpd`) terminiert; der Subagent ist versionsneutral.
Communities und v3-Benutzer gehören in die `snmpd.conf` (`rocommunity`/`rwcommunity`, `createUser`, `rouser`/`rwuser`),
Trap-Ziele ebenso (`trapsink` = v1, `trap2sink` = v2c, `trapsess` = v3). Counter64-Werte sind für v1-Manager unsichtbar.

Vollständiges Beispiel: [examples/test_agent.cpp](examples/test_agent.cpp) zur Beispiel-MIB
[mibs/SNMPWRAPPER-TEST-MIB.txt](mibs/SNMPWRAPPER-TEST-MIB.txt): Skalare inkl. Counter64, schreibbare Tabelle,
RowStatus-Tabelle, Tabelle mit zusammengesetztem Index (IpAddress, Port, IMPLIED String), große Tabelle
(`--big-table N`), Notification. `enterprises.99999` ist ein Platzhalter – eigene Enterprise-Nummer eintragen.

Manuell ausprobieren:
```sh
snmpd -f -Le -C -c test-snmpd.conf     # master agentx, siehe test/run_integration.sh für eine fertige Konfiguration
build/debug/examples/test_agent --socket tcp:127.0.0.1:705
snmpwalk -v2c -c public -M +mibs -m +SNMPWRAPPER-TEST-MIB localhost SNMPWRAPPER-TEST-MIB::snmpWrapperTestMIB
```

## Client

```cpp
#include <snmpwrap/client.hpp>
snmpwrap::SessionConfig cfg;  cfg.peer = "127.0.0.1:161";  cfg.community = "public";   // v2c (Default)
// v3: cfg.version = SessionConfig::Version::V3; cfg.user = "authuser";
//     cfg.securityLevel = SessionConfig::SecurityLevel::AuthNoPriv; cfg.authPassphrase = "...";
snmpwrap::Client c(cfg);
auto v   = c.get(Oid::parse("1.3.6.1.2.1.1.1.0"));       // VarBind{oid, value}; v2c/v3 ggf. Type::NoSuchInstance
auto all = c.walk(Oid::parse("1.3.6.1.4.1.99999"));      // GETBULK (v2c/v3) bzw. GETNEXT (v1)
c.set(Oid::parse("1.3.6.1.4.1.99999.1.4.0"), Value::integer(75));
c.set({{oidA, Value::integer(1)}, {oidB, Value::string("x")}});   // mehrere Varbinds in EINEM atomaren SET
```
Fehler: `TransportError` (Timeout, Netz, v3-Authentifizierung), `ResponseError` (error-status des Agents,
`.status()`, `.index()`). Kommandozeilen-Beispiel mit allen Versionen: [examples/client_cli.cpp](examples/client_cli.cpp).

## Getestet / Grenzen
* Linux (WSL2 Ubuntu 22.04, GCC 11), unverändertes Net-SNMP 5.9.1, statisch und als Shared Library (`-Werror`):
  * Unit-Tests ohne snmpd (`test/unit_tests.cpp`): Index-Codec, GET/GETNEXT-Randfälle, noSuchObject/-Instance,
    RowStatus-Zustandsmaschine, Rollback, 100 000-Zeilen-Tabelle ohne `rows()`-Aufruf.
  * Lifecycle-Tests (`test/lifecycle_test.cpp`): Init-Reihenfolge Agent/Client, nur ein Agent pro Prozess.
  * MIB-Tests (`test/mib_model_tests.cpp`, `test/mib_binder_tests.cpp`): Einlesen per Net-SNMP-Parser, AUGMENTS,
    IMPLIED-/Fixed-Size-Indizes, Bereiche > 2^31, DEFVAL, Namen/Format/Parsing, kaputte MIBs, Bindungsfehler.
  * Integrationstest mit echtem `snmpd` (`test/run_integration.sh`): dieselben Kernfälle für **v1, v2c,
    v3 noAuthNoPriv und v3 authNoPriv (SHA)** – jeweils mit den Net-SNMP-Tools und mit `snmpwrap::Client` –, dazu
    RowStatus, Multi-Index, große Tabelle per GETBULK, Rollback mit echtem UNDO, v1- und v2c-Traps, v3-Fehlerfälle,
    `client_cli -m`. Die Suite läuft gegen **drei Implementierungen** derselben MIB – handgeschrieben, generiert
    (plus generierter Client) und Laufzeit-gebunden – und alle bestehen identisch.
  * **Verschachtelte Datenstruktur aus der MIB:** Der Generator erzeugt zusätzlich `struct Data` (Gruppen als verschachtelte
  Structs, Skalare als Felder, Tabellen als Zeilencontainer `data.gruppe.tabelle[index].spalte`) und `DataAgent`, der sie per
  SNMP bereitstellt – ohne handgeschriebene Methode pro Objekt. Änderungen von Managern melden `onSet` / `onGet`
  (siehe Guide, Kapitel 20). Lokale Zuweisungen werden nicht automatisch geprüft, dafür gibt es `data.validate()`.
* Generator: deterministische Ausgabe, Neugenerierung bei MIB-Änderung, `snmpwrap_add_mib` aus einem installierten
    Paket (`find_package`).
* **v3 authPriv ist in dieser Umgebung nicht verifiziert:** Das lokal gebaute Net-SNMP (`--with-openssl=internal`,
  ohne OpenSSL-Header) beantwortet authPriv/AES auch mit seinen eigenen Tools nicht. Der Test erkennt das und meldet
  SKIP. Mit einem Net-SNMP mit OpenSSL (z. B. Ubuntu-Paket `libsnmp-dev`) läuft dieser Modus automatisch mit.
* **SHA-2 und AES192/256 (v3):** Der Client kennt SHA-224/256/384/512 und AES-192/256, aber sie funktionieren nur, wenn das
  verwendete Net-SNMP sie kann (SHA-2: mit OpenSSL gebaut; AES192/256: `--enable-blumenthal-aes`). Sonst meldet der
  Client einen Fehler. Im lokal gebauten Net-SNMP 5.9.1 dieser Umgebung sind sie nicht verfügbar.
* Nur AgentX-Subagent (kein eigenständiger Master-Agent). Windows nativ wird nicht unterstützt/getestet.
* `Opaque` und `BITS` haben eigene `Value`-Typen (`Value::opaque`, `Value::bits`), behalten also ihren ASN.1-Typ;
  Getter müssen sie mit der passenden Factory liefern (nicht `Value::string`). Der Generator bildet beide als `std::string` ab; DEFVAL-Formen, die sich nicht in einen Wert
  übersetzen lassen (z. B. BITS als `{ bitA, bitB }` oder OIDs unbekannter Namen), werden ausgelassen (im generierten
  Code kommentiert); Zahlen, Enum-Labels, Strings, Hex-/Binär-Strings und IP-Adressen werden übernommen. Beim Cross-Kompilieren
  muss `snmpwrap-mibgen` für den Build-Rechner gebaut werden.
* Den AgentX-Reconnect nach einem Neustart des Masters übernimmt Net-SNMP (`pingIntervalSec`); er ist nicht
  automatisiert getestet.
