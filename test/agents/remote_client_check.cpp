// Checks the generated nested client view (snmpwrapper_test_mib::Remote) against a running agent.
// Called by run_integration.sh (EXTRA_CHECK) with the agent address; prints "  ok ..." / "  FAIL ..." lines.
//
//   remote_client_check <host:port>

#include <iostream>
#include <string>

#include "snmpwrapper_test_mib.hpp"

using namespace snmpwrapper_test_mib;

namespace {

int fails = 0;
void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << "remote: " << what << "\n";
    if (!ok) ++fails;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    snmpwrap::SessionConfig cfg;
    cfg.peer = argv[1];
    cfg.community = "private";
    try {
        snmpwrap::Client session(cfg);
        Remote remote(session);

        // scalars in a group
        const std::int32_t before = remote.swtScalars.swtLimit.get();
        remote.swtScalars.swtLimit.set(42);
        check(remote.swtScalars.swtLimit.get() == 42, "swtScalars.swtLimit set / get");
        check(remote.swtScalars.swtBigCounter.get() == 4294967297ULL, "Counter64 scalar");
        bool rejected = false;
        try {
            remote.swtScalars.swtLimit.set(500);  // MIB range 1..100, checked before sending
        } catch (const snmpwrap::SetError& e) {
            rejected = e.status() == snmpwrap::ErrorStatus::WrongValue;
        }
        check(rejected && remote.swtScalars.swtLimit.get() == 42, "out-of-range value rejected locally");
        remote.swtScalars.swtLimit.set(before);

        // table with a plain index
        check(remote.swtTable[1].swtEntryName.get() == "eth0", "swtTable[1].swtEntryName");
        remote.swtTable[2].swtEntryStatus.set(SwtEntryStatus::testing);
        check(remote.swtTable[2].swtEntryStatus.get() == SwtEntryStatus::testing, "enum cell set / get");
        remote.swtTable[2].swtEntryStatus.set(SwtEntryStatus::down);
        const SwtEntry row1 = remote.swtTable[1].read();
        check(row1.swtEntryName == "eth0" && row1.swtEntryValue == 100, "row read()");
        check(remote.swtTable.read().size() == 3, "table read() returns 3 rows");

        // composite index
        check(remote.swtConnTable[{{10, 0, 0, 1}, 80, "web"}].swtConnState.get() == SwtConnState::established,
              "swtConnTable[(10.0.0.1, 80, 'web')]");

        // everything at once into a Data structure (one walk)
        const Data snapshot = remote.read();
        check(snapshot.swtScalars.swtName == remote.swtScalars.swtName.get(), "read(): scalar in a group");
        check(snapshot.swtTable.size() == 3 && snapshot.swtTable.at(SwtEntryIndex{2}).swtEntryValue == 200, "read(): table rows");
        check(snapshot.swtConnTable.at(SwtConnEntryIndex{{10, 0, 0, 2}, 22, "ssh"}).swtConnState == SwtConnState::established,
              "read(): composite index");
        check(snapshot.swtBigTable.size() > 1000, "read(): big table (" + std::to_string(snapshot.swtBigTable.size()) + " rows)");
        check(snapshot.swtScalars.swtBigCounter == 4294967297ULL, "read(): Counter64");

        // RowStatus table
        SwtRowEntryValues values;
        values.swtRowName = "remote";
        remote.swtRowTable.create({61}, values);
        check(remote.swtRowTable[61].swtRowStatus.get() == snmpwrap::RowStatus::Active, "create -> active");
        check(remote.swtRowTable[61].swtRowName.get() == "remote", "created row readable");
        remote.swtRowTable.destroy({61});
        check(remote.swtRowTable.read().count({61}) == 0, "destroy removes the row");
    } catch (const std::exception& e) {
        check(false, std::string("unexpected exception: ") + e.what());
    }
    return fails ? 1 : 0;
}
