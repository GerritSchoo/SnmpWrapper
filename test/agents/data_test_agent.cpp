// SNMPWRAPPER-TEST-MIB served from the generated nested Data structure (snmpwrap_add_mib + DataAgent).
// Same behaviour and command line as the other test agents, so the same integration suite runs against it:
//
//   data_test_agent [--socket <agentx>] [--trap-every <s>] [--big-table <rows>]
//
// The application only fills snmpwrapper_test_mib::Data; no OIDs, callbacks per object or index handling appear here.
// The two hooks cover what a plain structure cannot do by itself: values computed on read (counter, gauge, uptime)
// and the injected "commitfail" error.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#include "snmpwrapper_test_mib.hpp"

using namespace snmpwrapper_test_mib;

namespace {

std::atomic<bool> g_stop{false};  // set by the signal handler, checked by the main loop
void onSignal(int) { g_stop = true; }

}  // namespace

int main(int argc, char** argv) {
    snmpwrap::AgentConfig config;
    config.name = "snmpwrap-data-test-agent";
    int trapEvery = 0;
    std::uint32_t bigRows = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--socket" && i + 1 < argc) config.agentxSocket = argv[++i];
        else if (a == "--trap-every" && i + 1 < argc) trapEvery = std::atoi(argv[++i]);
        else if (a == "--big-table" && i + 1 < argc) bigRows = static_cast<std::uint32_t>(std::atol(argv[++i]));
        else {
            std::cerr << "usage: " << argv[0] << " [--socket s] [--trap-every s] [--big-table n]\n";
            return 2;
        }
    }

    try {
        snmpwrap::Agent agent(config);
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        // --- the application's data ---------------------------------------------------------------------
        Data data;
        data.swtScalars.swtName = "snmpwrap";
        data.swtScalars.swtLimit = 50;
        data.swtScalars.swtBigCounter = 4294967297ULL;
        data.swtTable[1] = {"eth0", 100, SwtEntryStatus::up};
        data.swtTable[2] = {"eth1", 200, SwtEntryStatus::down};
        data.swtTable[3] = {"lo", 5, SwtEntryStatus::testing};
        data.swtConnTable[SwtConnEntryIndex{{10, 0, 0, 1}, 80, "web"}].swtConnState = SwtConnState::established;
        data.swtConnTable[SwtConnEntryIndex{{10, 0, 0, 1}, 443, "tls"}].swtConnState = SwtConnState::listen;
        data.swtConnTable[SwtConnEntryIndex{{10, 0, 0, 2}, 22, "ssh"}].swtConnState = SwtConnState::established;
        for (std::uint32_t i = 1; i <= bigRows; ++i) data.swtBigTable[2 * i] = {i, 2 * i};

        const auto start = std::chrono::steady_clock::now();
        const auto seconds = [&] {
            return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count());
        };

        // --- serve it ------------------------------------------------------------------------------------
        DataAgent adapter(agent, data);

        adapter.onGet([&](Object object, const snmpwrap::Oid&) {  // values computed when they are read
            switch (object) {
                case Object::swtCounter: ++data.swtScalars.swtCounter; break;
                case Object::swtGauge: data.swtScalars.swtGauge = seconds() % 100; break;
                case Object::swtUptime: data.swtScalars.swtUptime = seconds() * 100; break;
                default: break;
            }
        });
        adapter.onSet([&](Object object, const snmpwrap::Oid& index) {  // a manager changed something
            std::string written;
            switch (object) {
                case Object::swtEntryName: written = data.swtTable.at(*SwtEntryIndex::fromOid(index)).swtEntryName; break;
                case Object::swtRowName: written = data.swtRowTable.at(*SwtRowEntryIndex::fromOid(index)).swtRowName; break;
                default: break;
            }
            if (written == "commitfail") throw snmpwrap::SetError(snmpwrap::ErrorStatus::CommitFailed, "injected failure");
        });

        std::cerr << "data test agent connecting to " << config.agentxSocket << "\n";
        agent.start();
        auto nextTrap = std::chrono::steady_clock::now() + std::chrono::seconds(trapEvery);
        while (!g_stop && agent.poll()) {
            if (trapEvery > 0 && std::chrono::steady_clock::now() >= nextTrap) {
                std::string name;
                std::int32_t limit = 0;
                {
                    auto guard = adapter.lock();
                    name = data.swtScalars.swtName;
                    limit = data.swtScalars.swtLimit;
                }
                sendSwtAlarm(agent, name, limit);  // generated, typed
                nextTrap += std::chrono::seconds(trapEvery);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
