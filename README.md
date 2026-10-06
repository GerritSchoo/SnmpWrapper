# snmpwrap

C++17-Bibliothek auf Basis von Net-SNMP 5.9 (unverändert). **Die MIB-Datei ist die einzige Beschreibung deiner Daten:**
Der Build erzeugt daraus C++-Typen. Der **Agent** füllt eine normale, wie die MIB verschachtelte Struktur und stellt
sie per SNMP bereit; der **Client** liest und ändert dieselben Daten auf einem entfernten Gerät mit derselben
Verschachtelung. Keine OIDs, keine Net-SNMP-Strukturen, kein Callback pro Objekt.

```cpp
// Agent                                              // Client
my_app_mib::Data data;                                my_app_mib::Remote remote(session);
data.appName = "my-app";                              remote.appSensors.appLimit.set(35);
data.appSensors.appSensorTable[1].appSensorName = "cpu";   auto name = remote.appSensors.appSensorTable[1].appSensorName.get();
my_app_mib::DataAgent adapter(agent, data);
adapter.onSet([&](const std::string& object, const snmpwrap::Oid& index) { /* ein Manager hat etwas geändert */ });
while (agent.poll()) { ... }
```

**Anleitung (Englisch) mit allen Details: [docs/GUIDE.md](docs/GUIDE.md).**
Vollständiges, getestetes Beispiel: [examples/apps](examples/apps) (`agent_app` und `client_app`).

## So funktioniert es

```
MY-APP-MIB.txt ──(Build: snmpwrap_add_mib)──► my_app_mib.hpp/.cpp ──┬── Agent:  Data + DataAgent  (hinter snmpd, AgentX)
                                                                    └── Client: Remote             (direkt per SNMP)
```

* **Agent:** ein AgentX-Subagent. `snmpd` übernimmt Protokollversionen, Communities, SNMPv3-Benutzer und Zugriffsrechte;
  dein Programm liefert nur die Werte. SET-Anfragen werden vor dem Schreiben gegen die MIB geprüft (Typ, Bereich, Länge,
  Enum-Werte); `onSet` meldet Änderungen und darf sie ablehnen, `onGet` aktualisiert Werte vor dem Lesen.
* **Client:** `remote.gruppe.objekt.get()` / `.set(v)`, Tabellen per `[index]` oder `.read()`. Werte werden vor dem
  Senden gegen die MIB geprüft.
* **Notifications:** pro `NOTIFICATION-TYPE` eine typisierte Funktion (`sendAppLimitExceeded(agent, …)`).

## Bauen (Linux / WSL)

Voraussetzungen: Linux, GCC ≥ 11, CMake ≥ 3.16, Net-SNMP 5.9.x mit Entwicklungsdateien
(`sudo apt install build-essential cmake libsnmp-dev snmpd snmp` oder ein eigener Build in `~/netsnmp`).

```sh
scripts/build.sh            # = cmake --preset debug && cmake --build --preset debug
scripts/test.sh             # alle Tests, inkl. Integration gegen einen eigenen snmpd (kein root nötig)
scripts/demo.sh             # snmpd + agent_app + client_app zum Ausprobieren
```

Die Build-Ausgabe liegt in `build/<preset>/` im Projektordner (Presets `debug`, `release`, `shared` in
[CMakePresets.json](CMakePresets.json)). Liegt das Projekt unter Windows (`/mnt/c/...`), ist der Build aus WSL heraus
langsamer als im Linux-Dateisystem.

**VS Code:** Mit der Extension **WSL** den Ordner per „WSL: Reopen Folder in WSL“ öffnen, Preset „Debug (Linux / WSL)“
wählen, F7. Oder unter Windows bleiben und die Tasks nutzen (`Strg+Umschalt+B` → „snmpwrap: build“, *Run Task* →
„snmpwrap: test“, „snmpwrap: demo“), die per `wsl -- bash scripts/…` laufen.

## In eigenen Projekten verwenden

```sh
cmake --install build/debug --prefix $HOME/snmpwrap-install
```

```cmake
find_package(snmpwrap REQUIRED)            # -DCMAKE_PREFIX_PATH="$HOME/snmpwrap-install;<net-snmp-prefix>"
add_library(my_app_mib STATIC)
snmpwrap_add_mib(my_app_mib MODULE MY-APP-MIB MIB mibs/MY-APP-MIB.txt)   # erzeugt my_app_mib.hpp/.cpp beim Build
add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE my_app_mib)
```

Statt zu installieren geht auch `add_subdirectory(SnmpWrapper)`. Die Kopiervorlagen dafür sind
[examples/apps/agent_app](examples/apps/agent_app) und [examples/apps/client_app](examples/apps/client_app).

## Projektaufbau

| Ordner | Inhalt |
|---|---|
| `include/snmpwrap/`, `src/snmpwrap/` | die Bibliothek (Kern: `Agent`, `Client`, `Mib`, `MibModel`, …) |
| `tools/mibgen/` | der Generator `snmpwrap-mibgen` (MIB → `Data`, `DataAgent`, `Remote`, `send…`) |
| `cmake/` | `find_package`-Unterstützung, `snmpwrap_add_mib()` |
| `examples/` | `apps/agent_app`, `apps/client_app` mit `MY-APP-MIB`, dazu das Werkzeug `client_cli` |
| `test/` | Unit-Tests, Integrationstests gegen einen echten snmpd, Test-MIBs |
| `docs/GUIDE.md` | die Anleitung |

## Getestet / Grenzen

* Linux (WSL2 Ubuntu 22.04, GCC 11), Net-SNMP 5.9.1. Tests (`scripts/test.sh`):
  * Unit-Tests ohne snmpd: Index-Kodierung, GET/GETNEXT-Randfälle, RowStatus, Rollback, große Tabellen
    (`unit`), MIB-Einlesen (`mib_model`), die generierte `Data`-Struktur samt `DataAgent`, Hooks und Validierung (`data_model`),
    Init-Reihenfolge Agent/Client (`lifecycle_*`).
  * Integration mit echtem `snmpd` für v1, v2c und v3 (noAuthNoPriv, authNoPriv): mit den Net-SNMP-Tools, `snmpwrap::Client`
    und dem generierten `Remote`; RowStatus, Mehrspalten-Index, GETBULK, Rollback, Traps (`integration`, `integration_data`)
    sowie die Beispiel-Apps (`apps`).
* **v3 authPriv** ist in dieser Umgebung nicht verifiziert (das lokale Net-SNMP ist ohne OpenSSL gebaut; der Test meldet SKIP).
  **SHA-2 und AES192/256** brauchen ein Net-SNMP mit OpenSSL bzw. `--enable-blumenthal-aes`, sonst meldet der Client einen Fehler.
* SNMP kennt keine Gleitkommazahlen: `Integer32` mit vereinbarter Skalierung (z. B. Zehntelgrad) oder Text verwenden.
* Lokale Zuweisungen an `Data` werden nicht automatisch geprüft – dafür gibt es `data.validate()`; SNMP-SET wird immer geprüft.
* `Data` hält alle Tabellenzeilen im Speicher.
* Nur AgentX-Subagent, ein Agent pro Prozess, kein Windows. Den Reconnect nach einem snmpd-Neustart übernimmt Net-SNMP.
* Ohne MIB gibt es die Kern-API (`Mib`, `Handler`, `Client` mit OIDs) – kurz beschrieben im Anhang des Guides.

Änderungen: [CHANGELOG.md](CHANGELOG.md).
