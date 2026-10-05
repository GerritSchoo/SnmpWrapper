// SNMPWRAPPER-TEST-MIB implemented with run-time MIB binding (MibModel + MibBinder).
// Same behaviour and command line as examples/test_agent.cpp (hand-written OIDs), so the same
// integration test suite runs against both:
//
//   runtime_test_agent [--socket <agentx>] [--trap-every <s>] [--big-table <rows>] [--mib-dir <dir>]
//
// Everything the MIB defines - OIDs, types, access, ranges, SIZEs, enumerations, index layouts,
// the RowStatus column, DEFVALs, notification objects - is taken from the MIB file; this file only
// contains the data.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

#include "snmpwrap/mib_binder.hpp"

using namespace snmpwrap;

namespace {

std::atomic<bool> g_stop{false};  // set by the signal handler, checked by the main loop
void onSignal(int) { g_stop = true; }

struct Row {
    std::string name;
    std::uint32_t value;
    std::int32_t status;
};
struct RowTableEntry {
    std::string name;
    std::uint32_t value = 0;
    RowStatus status = RowStatus::NotReady;
};

}  // namespace

int main(int argc, char** argv) {
    AgentConfig config;
    config.name = "snmpwrap-runtime-test-agent";
    int trapEvery = 0;
    std::uint32_t bigRows = 0;
    std::string mibDir = SNMPWRAP_SOURCE_DIR "/mibs";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--socket" && i + 1 < argc) config.agentxSocket = argv[++i];
        else if (a == "--trap-every" && i + 1 < argc) trapEvery = std::atoi(argv[++i]);
        else if (a == "--big-table" && i + 1 < argc) bigRows = static_cast<std::uint32_t>(std::atol(argv[++i]));
        else if (a == "--mib-dir" && i + 1 < argc) mibDir = argv[++i];
        else {
            std::cerr << "usage: " << argv[0] << " [--socket s] [--trap-every s] [--big-table n] [--mib-dir d]\n";
            return 2;
        }
    }

    // application data
    std::string name = "snmpwrap";
    std::uint32_t counter = 0;
    std::int32_t limit = 50;
    std::map<std::int32_t, Row> rows{{1, {"eth0", 100, 2}}, {2, {"eth1", 200, 1}}, {3, {"lo", 5, 3}}};
    std::map<std::uint32_t, RowTableEntry> rowTable;
    std::map<Oid, std::int32_t> conns;
    const auto start = std::chrono::steady_clock::now();
    auto seconds = [&] {
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count());
    };

    try {
        // 1) The MIB is the interface description: load it first (before the Agent touches Net-SNMP's config)
        MibModel model = MibModel::load({mibDir + "/SNMPWRAPPER-TEST-MIB.txt"});

        Agent agent(config);
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        MibBinder bind(agent.addMib(model.oid("snmpWrapperTestMIB")), model);

        // 2) scalars: only data, no OIDs / types / ranges
        bind.scalar("swtName", [&] { return Value::string(name); }, [&](const Value& v) { name = v.asString(); });
        bind.scalar("swtCounter", [&] { return Value::counter32(++counter); });
        bind.scalar("swtGauge", [&] { return Value::gauge(seconds() % 100); });
        bind.scalar("swtLimit", [&] { return Value::integer(limit); }, [&](const Value& v) { limit = v.asInt(); });
        bind.scalar("swtUptime", [&] { return Value::timeTicks(seconds() * 100); });
        bind.scalar("swtBigCounter", [] { return Value::counter64(4294967297ULL); });

        // 3) swtTable
        const SubId entryName = bind.column("swtEntryName"), entryValue = bind.column("swtEntryValue");
        TableBinding t;
        t.rows = [&] {
            std::vector<Oid> out;
            for (const auto& [idx, row] : rows) out.push_back(indexInt(static_cast<std::uint32_t>(idx)));
            return out;
        };
        t.get = [&, entryName, entryValue](const Oid& index, SubId col) {
            const Row& r = rows.at(static_cast<std::int32_t>(index[0]));
            if (col == entryName) return Value::string(r.name);
            if (col == entryValue) return Value::gauge(r.value);
            return Value::integer(r.status);
        };
        t.set = [&, entryName, entryValue](const Oid& index, SubId col, const Value& v) {
            Row& r = rows.at(static_cast<std::int32_t>(index[0]));
            if (col == entryName) {
                if (v.asString() == "commitfail") throw SetError(ErrorStatus::CommitFailed, "injected failure");
                r.name = v.asString();
            } else if (col == entryValue) {
                r.value = v.asUInt();
            } else {
                r.status = v.asInt();
            }
        };
        bind.table("swtTable", t);

        // 4) swtRowTable: RowStatus column, required columns and DEFVALs come from the MIB
        const SubId rowName = bind.column("swtRowName");
        TableBinding rt;
        rt.rows = [&] {
            std::vector<Oid> out;
            for (const auto& [idx, row] : rowTable) out.push_back(Oid{idx});
            return out;
        };
        rt.get = [&, rowName](const Oid& index, SubId col) {
            const auto& r = rowTable.at(index[0]);
            return col == rowName ? Value::string(r.name) : Value::gauge(r.value);
        };
        rt.set = [&, rowName](const Oid& index, SubId col, const Value& v) {
            auto& r = rowTable.at(index[0]);
            if (col == rowName) {
                if (v.asString() == "commitfail") throw SetError(ErrorStatus::CommitFailed, "injected failure");
                r.name = v.asString();
            } else {
                r.value = v.asUInt();
            }
        };
        rt.create = [&, rowName](const Oid& index, const std::map<SubId, Value>& cols) {
            RowTableEntry e;
            for (const auto& [col, v] : cols) {
                if (col == rowName) e.name = v.asString();
                else e.value = v.asUInt();  // swtRowValue, DEFVAL 0 if not supplied
            }
            rowTable[index[0]] = std::move(e);
        };
        rt.destroy = [&](const Oid& index) { rowTable.erase(index[0]); };
        rt.setState = [&](const Oid& index, RowStatus s) { rowTable.at(index[0]).status = s; };
        rt.state = [&](const Oid& index) { return rowTable.at(index[0]).status; };
        rt.complete = [&](const Oid& index) { return !rowTable.at(index[0]).name.empty(); };
        bind.table("swtRowTable", rt);

        // 5) swtConnTable: the composite index (IpAddress, Integer32, IMPLIED DisplayString) comes from the MIB
        const auto connIndex = model.indexSpecs(model.node("swtConnTable"));
        auto connKey = [&](std::uint8_t d, int port, const char* tag) {
            return encodeIndex(connIndex, {Value::ipAddress(10, 0, 0, d), Value::integer(port), Value::string(tag)});
        };
        conns = {{connKey(1, 80, "web"), 3}, {connKey(1, 443, "tls"), 2}, {connKey(2, 22, "ssh"), 3}};
        TableBinding ct;
        ct.rows = [&] {
            std::vector<Oid> out;
            for (const auto& [k, v] : conns) out.push_back(k);
            return out;
        };
        ct.get = [&](const Oid& index, SubId) { return Value::integer(conns.at(index)); };
        ct.set = [&](const Oid& index, SubId, const Value& v) { conns.at(index) = v.asInt(); };
        bind.table("swtConnTable", ct);

        // 6) swtBigTable: rows with index 2*i, served via nextRow / hasRow
        const SubId bigValue = bind.column("swtBigValue");
        TableBinding bt;
        bt.hasRow = [bigRows](const Oid& i) { return i.size() == 1 && i[0] >= 2 && i[0] % 2 == 0 && i[0] / 2 <= bigRows; };
        bt.nextRow = [bigRows](const Oid* after) -> std::optional<Oid> {
            std::uint64_t next = 2;
            if (after && !after->empty()) next = (static_cast<std::uint64_t>((*after)[0]) / 2 + 1) * 2;
            if (next / 2 > bigRows) return std::nullopt;
            return Oid{static_cast<SubId>(next)};
        };
        bt.get = [bigValue](const Oid& i, SubId col) { return Value::gauge(col == bigValue ? i[0] / 2 : i[0]); };
        bind.table("swtBigTable", bt);

        // 7) every object of the MIB must have data
        bind.finish();

        std::cerr << "runtime test agent connecting to " << config.agentxSocket << "\n";
        agent.start();
        auto nextTrap = std::chrono::steady_clock::now() + std::chrono::seconds(trapEvery);
        while (!g_stop && agent.poll()) {
            if (trapEvery > 0 && std::chrono::steady_clock::now() >= nextTrap) {
                bind.sendNotification(agent, "swtAlarm", {Value::string(name), Value::integer(limit)});
                nextTrap += std::chrono::seconds(trapEvery);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
