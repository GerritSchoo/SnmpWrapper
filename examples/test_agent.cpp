// Example AgentX subagent implementing SNMPWRAPPER-TEST-MIB (mibs/SNMPWRAPPER-TEST-MIB.txt)
// with the generic snmpwrap API. Needs a running snmpd with "master agentx".
//
//   test_agent [--socket tcp:127.0.0.1:705] [--trap-every <seconds>] [--big-table <rows>]

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>

#include "snmpwrap/agent.hpp"

using namespace snmpwrap;

namespace {

Agent* g_agent = nullptr;
void onSignal(int) {
    if (g_agent) g_agent->stop();
}

struct Row {
    std::string name;
    std::uint32_t value;
    std::int32_t status;  // 1 down, 2 up, 3 testing
};

struct RowTableEntry {
    std::string name;
    std::uint32_t value = 0;
    RowStatus status = RowStatus::NotReady;
};

// Application data. Handlers run in the agent thread; the mutex shows how to share with other threads.
struct State {
    std::mutex mutex;
    std::string name = "snmpwrap";
    std::uint32_t counter = 0;
    std::int32_t limit = 50;
    std::map<std::int32_t, Row> rows{{1, {"eth0", 100, 2}}, {2, {"eth1", 200, 1}}, {3, {"lo", 5, 3}}};
    std::map<std::uint32_t, RowTableEntry> rowTable;                // swtRowTable, rows created by managers
    std::map<Oid, std::int32_t> conns;                              // swtConnTable
    std::uint32_t bigRows = 0;                                      // swtBigTable
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    std::uint32_t seconds() const {
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count());
    }
};

}  // namespace

int main(int argc, char** argv) {
    AgentConfig config;
    config.name = "snmpwrap-test-agent";
    State st;
    int trapEvery = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--socket" && i + 1 < argc) config.agentxSocket = argv[++i];
        else if (a == "--trap-every" && i + 1 < argc) trapEvery = std::atoi(argv[++i]);
        else if (a == "--big-table" && i + 1 < argc) st.bigRows = static_cast<std::uint32_t>(std::atol(argv[++i]));
        else {
            std::cerr << "usage: " << argv[0] << " [--socket <agentx socket>] [--trap-every <seconds>] [--big-table <rows>]\n";
            return 2;
        }
    }

    const Oid root = Oid::parse("1.3.6.1.4.1.99999");  // snmpWrapperTestMIB

    try {
        Agent agent(config);
        g_agent = &agent;
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        Mib& mib = agent.addMib(root);

        // --- scalars (swtScalars = .1) ------------------------------------------------------
        mib.scalar({1, 1}, {Type::OctetString,  // swtName
                            [&] { std::lock_guard l(st.mutex); return Value::string(st.name); },
                            [&](const Value& v) { std::lock_guard l(st.mutex); st.name = v.asString(); },
                            [](const Value& v) {
                                if (v.asString().size() > 64) throw SetError(ErrorStatus::WrongLength, "max 64 chars");
                            }});
        mib.scalar({1, 2}, {Type::Counter32,  // swtCounter
                            [&] { std::lock_guard l(st.mutex); return Value::counter32(++st.counter); }});
        mib.scalar({1, 3}, {Type::Gauge32,  // swtGauge
                            [&] { return Value::gauge(st.seconds() % 100); }});
        mib.scalar({1, 4}, {Type::Integer,  // swtLimit
                            [&] { std::lock_guard l(st.mutex); return Value::integer(st.limit); },
                            [&](const Value& v) { std::lock_guard l(st.mutex); st.limit = v.asInt(); },
                            [](const Value& v) {
                                if (v.asInt() < 1 || v.asInt() > 100)
                                    throw SetError(ErrorStatus::WrongValue, "swtLimit must be 1..100");
                            }});
        mib.scalar({1, 5}, {Type::TimeTicks,  // swtUptime
                            [&] { return Value::timeTicks(st.seconds() * 100); }});
        mib.scalar({1, 6}, {Type::Counter64,  // swtBigCounter
                            [] { return Value::counter64(4294967297ULL); }});

        // --- table (swtTable = .2, entry .2.1, columns 2..4, index swtIndex) --------------------
        TableDef t;
        t.indexes = {IndexSpec::integer()};
        t.columns = {{2, Type::OctetString, Access::ReadWrite},  // swtEntryName
                     {3, Type::Gauge32, Access::ReadWrite},      // swtEntryValue
                     {4, Type::Integer, Access::ReadWrite}};     // swtEntryStatus
        t.rows = [&] {
            std::lock_guard l(st.mutex);
            std::vector<Oid> out;
            for (const auto& [idx, row] : st.rows) out.push_back(indexInt(static_cast<std::uint32_t>(idx)));
            return out;
        };
        t.get = [&](const Oid& index, SubId col) {
            std::lock_guard l(st.mutex);
            const Row& r = st.rows.at(static_cast<std::int32_t>(index[0]));
            switch (col) {
                case 2: return Value::string(r.name);
                case 3: return Value::gauge(r.value);
                default: return Value::integer(r.status);
            }
        };
        t.validate = [](const Oid&, SubId col, const Value& v) {
            if (col == 2 && v.asString().size() > 32) throw SetError(ErrorStatus::WrongLength, "max 32 chars");
            if (col == 4 && (v.asInt() < 1 || v.asInt() > 3)) throw SetError(ErrorStatus::WrongValue, "status 1..3");
        };
        t.set = [&](const Oid& index, SubId col, const Value& v) {
            std::lock_guard l(st.mutex);
            Row& r = st.rows.at(static_cast<std::int32_t>(index[0]));
            if (col == 2) {
                // fault injection for the rollback tests: fails in the commit-failed-capable apply phase
                if (v.asString() == "commitfail") throw SetError(ErrorStatus::CommitFailed, "injected failure");
                r.name = v.asString();
            } else if (col == 3) {
                r.value = v.asUInt();
            } else {
                r.status = v.asInt();
            }
        };
        mib.table(2, std::move(t));

        // --- swtRowTable (.4): rows created and destroyed through RowStatus ---------------------
        TableDef rt;
        rt.indexes = {IndexSpec::integer()};
        rt.columns = {{2, Type::OctetString, Access::ReadWrite},  // swtRowName
                      {3, Type::Gauge32, Access::ReadWrite}};     // swtRowValue
        rt.rows = [&] {
            std::lock_guard l(st.mutex);
            std::vector<Oid> out;
            for (const auto& [idx, row] : st.rowTable) out.push_back(Oid{idx});
            return out;
        };
        rt.get = [&](const Oid& index, SubId col) {
            std::lock_guard l(st.mutex);
            const auto& r = st.rowTable.at(index[0]);
            return col == 2 ? Value::string(r.name) : Value::gauge(r.value);
        };
        rt.validate = [](const Oid&, SubId col, const Value& v) {
            if (col == 2 && v.asString().size() > 32) throw SetError(ErrorStatus::WrongLength, "max 32 chars");
            if (col == 3 && v.asUInt() > 1000) throw SetError(ErrorStatus::WrongValue, "value 0..1000");
        };
        rt.set = [&](const Oid& index, SubId col, const Value& v) {
            std::lock_guard l(st.mutex);
            auto& r = st.rowTable.at(index[0]);
            if (col == 2) {
                if (v.asString() == "commitfail") throw SetError(ErrorStatus::CommitFailed, "injected failure");
                r.name = v.asString();
            } else {
                r.value = v.asUInt();
            }
        };
        RowStatusSpec rs;
        rs.column = 4;                 // swtRowStatus
        rs.requiredColumns = {2};      // swtRowName must be given to createAndGo
        rs.create = [&](const Oid& index, const std::map<SubId, Value>& cols) {
            std::lock_guard l(st.mutex);
            RowTableEntry e;
            if (auto n = cols.find(2); n != cols.end()) e.name = n->second.asString();
            if (auto v = cols.find(3); v != cols.end()) e.value = v->second.asUInt();
            st.rowTable[index[0]] = std::move(e);
        };
        rs.destroy = [&](const Oid& index) { std::lock_guard l(st.mutex); st.rowTable.erase(index[0]); };
        rs.setState = [&](const Oid& index, RowStatus s) { std::lock_guard l(st.mutex); st.rowTable.at(index[0]).status = s; };
        rs.state = [&](const Oid& index) { std::lock_guard l(st.mutex); return st.rowTable.at(index[0]).status; };
        rs.complete = [&](const Oid& index) { std::lock_guard l(st.mutex); return !st.rowTable.at(index[0]).name.empty(); };
        rt.rowStatus = rs;
        mib.table(4, std::move(rt));

        // --- swtConnTable (.5): composite index (IpAddress, port, IMPLIED tag) --------------------
        const std::vector<IndexSpec> connIndex{IndexSpec::ipAddress(), IndexSpec::integer(), IndexSpec::impliedString()};
        auto connKey = [&](std::uint8_t d, int port, const char* tag) {
            return encodeIndex(connIndex, {Value::ipAddress(10, 0, 0, d), Value::integer(port), Value::string(tag)});
        };
        st.conns = {{connKey(1, 80, "web"), 3}, {connKey(1, 443, "tls"), 2}, {connKey(2, 22, "ssh"), 3}};
        TableDef ct;
        ct.indexes = connIndex;
        ct.columns = {{4, Type::Integer, Access::ReadWrite}};  // swtConnState (address / port / tag live in the index)
        ct.rows = [&] {
            std::lock_guard l(st.mutex);
            std::vector<Oid> out;
            for (const auto& [k, v] : st.conns) out.push_back(k);
            return out;
        };
        ct.get = [&](const Oid& index, SubId) { std::lock_guard l(st.mutex); return Value::integer(st.conns.at(index)); };
        ct.validate = [](const Oid&, SubId, const Value& v) {
            if (v.asInt() < 1 || v.asInt() > 3) throw SetError(ErrorStatus::WrongValue, "state 1..3");
        };
        ct.set = [&](const Oid& index, SubId, const Value& v) { std::lock_guard l(st.mutex); st.conns.at(index) = v.asInt(); };
        mib.table(5, std::move(ct));

        // --- swtBigTable (.6): N rows with index 2*i, answered via nextRow/hasRow (no enumeration) ---
        if (st.bigRows > 0) {
            TableDef bt;
            bt.indexes = {IndexSpec::unsignedInt()};
            bt.columns = {{2, Type::Gauge32, Access::ReadOnly}, {3, Type::Gauge32, Access::ReadOnly}};
            const std::uint32_t n = st.bigRows;
            bt.hasRow = [n](const Oid& i) { return i.size() == 1 && i[0] >= 2 && i[0] % 2 == 0 && i[0] / 2 <= n; };
            bt.nextRow = [n](const Oid* after) -> std::optional<Oid> {
                std::uint64_t next = 2;  // first index
                if (after && !after->empty()) next = (static_cast<std::uint64_t>((*after)[0]) / 2 + 1) * 2;
                if (next / 2 > n) return std::nullopt;
                return Oid{static_cast<SubId>(next)};
            };
            bt.get = [](const Oid& i, SubId col) { return Value::gauge(col == 2 ? i[0] / 2 : i[0]); };
            mib.table(6, std::move(bt));
        }

        std::cerr << "test agent connecting to " << config.agentxSocket << " (Ctrl+C to quit)\n";
        agent.start();

        auto nextTrap = std::chrono::steady_clock::now() + std::chrono::seconds(trapEvery);
        while (agent.poll()) {
            if (trapEvery > 0 && std::chrono::steady_clock::now() >= nextTrap) {
                std::lock_guard l(st.mutex);
                agent.sendTrap(root + SubId{3} + SubId{0} + SubId{1},  // swtAlarm
                               {{root + SubId{1} + SubId{1} + SubId{0}, Value::string(st.name)},
                                {root + SubId{1} + SubId{4} + SubId{0}, Value::integer(st.limit)}});
                nextTrap += std::chrono::seconds(trapEvery);
            }
        }
        std::cerr << "test agent stopped\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
