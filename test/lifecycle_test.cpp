// Agent / Client construction order. Net-SNMP keeps process-wide state, so every scenario runs in a child
// process of its own (death test: EXPECT_EXIT) and starts with an uninitialized library.
//   ClientThenAgent : creating an Agent after a Client must fail loudly (the AgentX connection could not be
//                     set up any more), not silently produce a disconnected subagent.
//   AgentTwice      : a second Agent while the first exists is refused; after destruction a new one works.

#include <gtest/gtest.h>

#include <cstdlib>
#include <iostream>

#include "snmpwrap/agent.hpp"
#include "snmpwrap/client.hpp"

using namespace snmpwrap;

namespace {

AgentConfig quietConfig() {
    AgentConfig cfg;
    cfg.logToStderr = false;
    cfg.agentxSocket = "tcp:127.0.0.1:1";  // nothing listens; start() is never called
    return cfg;
}

void clientThenAgent() {
    SessionConfig sc;
    sc.peer = "127.0.0.1:1";
    Client client(sc);
    try {
        Agent agent(quietConfig());
    } catch (const Error& e) {
        std::cerr << "refused: " << e.what() << "\n";
        std::exit(0);
    }
    std::cerr << "FAIL: Agent constructed after a Client without error\n";
    std::exit(1);
}

void agentTwice() {
    {
        Agent first(quietConfig());
        try {
            Agent second(quietConfig());
            std::cerr << "FAIL: second Agent accepted\n";
            std::exit(1);
        } catch (const Error&) {
        }
    }
    Agent again(quietConfig());  // must work after the first one is gone
    std::cerr << "ok\n";
    std::exit(0);
}

}  // namespace

TEST(LifecycleDeathTest, ClientThenAgentIsRefused) {
    EXPECT_EXIT(clientThenAgent(), ::testing::ExitedWithCode(0), "refused: .*before the first snmpwrap::Client");
}

TEST(LifecycleDeathTest, SecondAgentIsRefusedUntilTheFirstIsGone) {
    EXPECT_EXIT(agentTwice(), ::testing::ExitedWithCode(0), "ok");
}
