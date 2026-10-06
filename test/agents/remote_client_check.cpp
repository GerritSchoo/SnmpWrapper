// Checks the generated nested client view (snmpwrapper_test_mib::Remote) against a running agent.
// Called by run_integration.sh (EXTRA_CHECK) with the agent address; prints "  ok ..." / "  FAIL ..." lines.
//
//   remote_client_check <host:port> [notification address]

#include <chrono>
#include <iostream>
#include <memory>
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

        // notifications: listen first, the agent sends swtAlarm every few seconds (as v2c trap, v1 trap and inform)
        std::unique_ptr<snmpwrap::NotificationReceiver> receiver;
        std::unique_ptr<Notifications> notifications;
        int v2 = 0, v1 = 0, informs = 0;
        bool fieldsOk = true;
        if (argc > 2) {
            receiver = std::make_unique<snmpwrap::NotificationReceiver>(argv[2]);
            notifications = std::make_unique<Notifications>(*receiver);
            notifications->onSwtAlarm([&](const SwtAlarm& a) {
                fieldsOk = fieldsOk && !a.swtName.empty() && a.swtLimit >= 1 && a.swtLimit <= 100;
                if (a.raw.inform) ++informs;
                else if (a.raw.version == 1) ++v1;
                else ++v2;
            });
        }

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

        // several values in ONE request: all or nothing
        remote.change().set(remote.swtScalars.swtLimit, 66).set(remote.swtTable[2].swtEntryValue, 222u).send();
        check(remote.swtScalars.swtLimit.get() == 66 && remote.swtTable[2].swtEntryValue.get() == 222, "change(): both values written");
        bool refused = false;
        try {
            remote.change().set(remote.swtScalars.swtLimit, 77).set(remote.swtTable[1].swtEntryName, "commitfail").send();
        } catch (const snmpwrap::ResponseError& e) {
            refused = e.status() == snmpwrap::ErrorStatus::CommitFailed && e.index() == 2;
        }
        check(refused && remote.swtScalars.swtLimit.get() == 66 && remote.swtTable[1].swtEntryName.get() == "eth0",
              "change(): agent refuses the 2nd value -> nothing written, error names value 2");
        bool local = false;
        try {
            remote.change().set(remote.swtScalars.swtLimit, 500);
        } catch (const snmpwrap::SetError&) {
            local = true;
        }
        check(local, "change(): MIB check when a value is added");
        remote.change().set(remote.swtScalars.swtLimit, before).set(remote.swtTable[2].swtEntryValue, 200u).send();

        // a subtree as one message, a row as one message, the whole MIB as one message
        Data::SwtScalarsGroup msg;
        msg.swtName = "message";
        msg.swtLimit = 33;
        remote.swtScalars.send(msg);
        check(remote.swtScalars.swtName.get() == "message" && remote.swtScalars.swtLimit.get() == 33, "group send(): both values");
        SwtEntry row = remote.swtTable[3].read();
        row.swtEntryName = "loop";
        row.swtEntryValue = 6;
        remote.swtTable[3].send(row);
        check(remote.swtTable[3].read().swtEntryName == "loop" && remote.swtTable[3].swtEntryValue.get() == 6, "row send()");
        remote.swtTable[3].send(SwtEntry{"lo", 5, SwtEntryStatus::testing});
        Data all = remote.read();
        all.swtScalars.swtName = "snmpwrap";
        all.swtScalars.swtLimit = before;
        remote.send(all);
        check(remote.swtScalars.swtName.get() == "snmpwrap" && remote.swtScalars.swtLimit.get() == before, "whole-MIB send()");

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

        if (receiver) {
            for (int i = 0; i < 100 && (v2 == 0 || v1 == 0 || informs == 0); ++i) receiver->poll(std::chrono::milliseconds(100));
            check(v2 > 0, "notification: v2c trap swtAlarm received, typed");
            check(v1 > 0, "notification: v1 trap converted to swtAlarm");
            check(informs > 0, "notification: inform received (and acknowledged)");
            check(fieldsOk, "notification: values decoded (swtName, swtLimit)");
        }
    } catch (const std::exception& e) {
        check(false, std::string("unexpected exception: ") + e.what());
    }
    return fails ? 1 : 0;
}
