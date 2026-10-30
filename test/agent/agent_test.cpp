// Agent (agent.hpp): registering and removing subtrees, and a DataAgent that removes its own subtree.
// No snmpd: the Agent is never started; Net-SNMP still keeps the registry, so duplicates are refused.

#include <gtest/gtest.h>

#include <memory>

#include "snmpwrap/agent.hpp"
#include "snmpwrapper_test_mib.hpp"
#include "support/test_support.hpp"

using namespace snmpwrap;
namespace t = snmpwrapper_test_mib;

namespace {

const Oid kRoot = Oid::parse("1.3.6.1.4.1.99999.900");

/// One Agent per test (Net-SNMP allows one per process at a time); it never connects.
class AgentTest : public ::testing::Test {
protected:
    static AgentConfig quiet() {
        AgentConfig cfg;
        cfg.logToStderr = false;
        cfg.agentxSocket = "tcp:127.0.0.1:1";  // nothing listens; start() is never called
        return cfg;
    }
    static ScalarDef constant(int v) {
        return {Type::Integer, [v] { return Value::integer(v); }};
    }

    Agent agent{quiet()};
};

TEST_F(AgentTest, SameRootCannotBeRegisteredTwice) {
    agent.addMib(kRoot);
    EXPECT_THROW(agent.addMib(kRoot), Error);
}

TEST_F(AgentTest, RemovedRootCanBeRegisteredAgain) {
    Mib& first = agent.addMib(kRoot);
    first.scalar(1, constant(1));
    agent.removeMib(first);
    Mib& second = agent.addMib(kRoot);  // Net-SNMP was told: no duplicate any more
    second.scalar(1, constant(2));
    EXPECT_EQ(second.get(kRoot + Oid({1, 0})), Value::integer(2));
}

TEST_F(AgentTest, RemoveHandlerByRoot) {
    agent.addHandler(kRoot, std::make_shared<Mib>(kRoot));
    EXPECT_NO_THROW(agent.removeHandler(kRoot));
    EXPECT_NO_THROW(agent.addHandler(kRoot, std::make_shared<Mib>(kRoot)));
}

TEST_F(AgentTest, RemovingWhatIsNotRegisteredThrows) {
    EXPECT_THROW(agent.removeHandler(kRoot), Error);
    Mib foreign(kRoot);
    EXPECT_THROW(agent.removeMib(foreign), Error);  // not created by this Agent
}

TEST_F(AgentTest, RemovingTwiceThrows) {
    agent.addMib(kRoot);
    agent.removeHandler(kRoot);
    EXPECT_THROW(agent.removeHandler(kRoot), Error);
}

TEST_F(AgentTest, OtherSubtreesStayRegistered) {
    const Oid other = Oid::parse("1.3.6.1.4.1.99999.901");
    agent.addMib(kRoot);
    agent.addMib(other);
    agent.removeHandler(kRoot);
    EXPECT_THROW(agent.addMib(other), Error);  // still registered
    EXPECT_NO_THROW(agent.addMib(kRoot));
}

// --- DataAgent(Agent&, Data&) -------------------------------------------------------------------

TEST_F(AgentTest, DataAgentHoldsItsRootWhileAlive) {
    t::Data data;
    t::DataAgent adapter(agent, data);
    EXPECT_THROW(agent.addMib(t::oids::root), Error);
}

TEST_F(AgentTest, DestroyingTheDataAgentRemovesItsSubtree) {
    t::Data data;
    { t::DataAgent adapter(agent, data); }
    EXPECT_NO_THROW(agent.addMib(t::oids::root).scalar(1, constant(7)));  // the root is free again
}

TEST_F(AgentTest, ANewDataAgentCanFollowTheOldOne) {  // e.g. reconfiguration at run time
    t::Data data;
    auto adapter = std::make_unique<t::DataAgent>(agent, data);
    adapter.reset();
    EXPECT_NO_THROW(adapter = std::make_unique<t::DataAgent>(agent, data));
}

}  // namespace
