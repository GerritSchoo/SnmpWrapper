// Checks the generated typed client (snmpwrapper_test_mib::Client) against a running agent.
// Called by run_integration.sh (EXTRA_CHECK) with the agent address; prints "  ok ..." / "  FAIL ..." lines.
//
//   generated_client_check <host:port>

#include <iostream>
#include <string>

#include "snmpwrapper_test_mib.hpp"

using namespace snmpwrapper_test_mib;

namespace {

int fails = 0;
void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << "generated client: " << what << "\n";
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
        Client mib(session);

        // scalars
        const std::int32_t before = mib.swtLimit();
        mib.setSwtLimit(42);
        check(mib.swtLimit() == 42, "setSwtLimit / swtLimit");
        check(mib.swtBigCounter() == 4294967297ULL, "Counter64 scalar");
        bool rejected = false;
        try {
            mib.setSwtLimit(500);  // MIB range 1..100, checked locally before sending
        } catch (const snmpwrap::SetError& e) {
            rejected = e.status() == snmpwrap::ErrorStatus::WrongValue;
        }
        check(rejected && mib.swtLimit() == 42, "out-of-range value rejected locally (MIB check)");
        mib.setSwtLimit(before);

        // plain table
        auto table = mib.swtTable();
        check(table.size() == 3, "swtTable() returns 3 rows");
        check(table.count({1}) && table.at({1}).swtEntryName == "eth0" && table.at({1}).swtEntryStatus == SwtEntryStatus::up,
              "row 1 = eth0 / up");
        check(mib.swtEntryValue({2}) == 200, "single cell swtEntryValue.2");

        // composite index with IMPLIED string
        auto conns = mib.swtConnTable();
        const SwtConnEntryIndex web{{10, 0, 0, 1}, 80, "web"};
        check(conns.size() == 3 && conns.count(web) && conns.at(web).swtConnState == SwtConnState::established,
              "swtConnTable() decodes (10.0.0.1, 80, 'web')");
        check(conns.begin()->first == web, "rows come back in OID order");

        // RowStatus table: create (DEFVAL for swtRowValue), read, destroy
        const SwtRowEntryIndex idx{60};
        SwtRowEntryValues values;
        values.swtRowName = "generated";
        mib.createSwtRowEntry(idx, values);
        check(mib.swtRowStatus(idx) == snmpwrap::RowStatus::Active, "createSwtRowEntry -> active");
        auto rows = mib.swtRowTable();
        check(rows.count(idx) && rows.at(idx).swtRowName == "generated" && rows.at(idx).swtRowValue == 0,
              "created row readable, DEFVAL 0 applied");
        mib.destroySwtRowEntry(idx);
        check(mib.swtRowTable().count(idx) == 0, "destroySwtRowEntry removes the row");

        bool missing = false;
        try {
            (void)mib.swtRowName(idx);
        } catch (const snmpwrap::Error& e) {
            missing = std::string(e.what()).find("NoSuchInstance") != std::string::npos;
        }
        check(missing, "getter on a missing row throws (NoSuchInstance)");
    } catch (const std::exception& e) {
        check(false, std::string("unexpected exception: ") + e.what());
    }
    return fails ? 1 : 0;
}
