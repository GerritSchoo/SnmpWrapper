// net-snmp keeps process-wide state; these checks need a fresh process each, selected by argv[1].
//   client-then-agent : creating an Agent after a Client must fail loudly (the AgentX connection
//                       could not be set up any more), not silently produce a disconnected subagent.
//   agent-twice       : a second Agent while the first exists is refused; after destruction a new one works.

#include <iostream>
#include <string>

#include "snmpwrap/agent.hpp"
#include "snmpwrap/client.hpp"

using namespace snmpwrap;

int main(int argc, char** argv) {
    const std::string scenario = argc > 1 ? argv[1] : "";
    AgentConfig cfg;
    cfg.logToStderr = false;
    cfg.agentxSocket = "tcp:127.0.0.1:1";  // nothing listens; start() is never called

    if (scenario == "client-then-agent") {
        SessionConfig sc;
        sc.peer = "127.0.0.1:1";
        Client client(sc);
        try {
            Agent agent(cfg);
        } catch (const Error& e) {
            std::cout << "ok: " << e.what() << "\n";
            return 0;
        }
        std::cerr << "FAIL: Agent constructed after a Client without error\n";
        return 1;
    }
    if (scenario == "agent-twice") {
        {
            Agent first(cfg);
            try {
                Agent second(cfg);
                std::cerr << "FAIL: second Agent accepted\n";
                return 1;
            } catch (const Error&) {
            }
        }
        Agent again(cfg);  // must work after the first one is gone
        std::cout << "ok\n";
        return 0;
    }
    std::cerr << "usage: lifecycle_test client-then-agent|agent-twice\n";
    return 2;
}
