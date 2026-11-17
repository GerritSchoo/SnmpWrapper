// Agent (agent->hpp): registering and removing subtrees, and a DataAgent that removes its own subtree.
// No snmpd: the Agent is never started; Net-SNMP still keeps the registry, so duplicates are refused.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "snmpwrap/Agent.hpp"
#include "snmpwrapper_test_mib.hpp"
#include "TestSupport.hpp"

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

    std::shared_ptr<Agent> agent = std::make_shared<Agent>(quiet());  // shared: the DataAgents keep it
};

TEST_F(AgentTest, SameRootCannotBeRegisteredTwice) {
    agent->addMib(kRoot);
    EXPECT_THROW(agent->addMib(kRoot), Error);
}

TEST_F(AgentTest, RemovedRootCanBeRegisteredAgain) {
    std::shared_ptr<Mib> first = agent->addMib(kRoot);
    first->scalar(1, constant(1));
    agent->removeMib(*first);
    std::shared_ptr<Mib> second = agent->addMib(kRoot);  // Net-SNMP was told: no duplicate any more
    second->scalar(1, constant(2));
    EXPECT_EQ(second->get(kRoot + Oid({1, 0})), Value::integer(2));
}

TEST_F(AgentTest, RemoveHandlerByRoot) {
    agent->addHandler(kRoot, std::make_shared<Mib>(kRoot));
    EXPECT_NO_THROW(agent->removeHandler(kRoot));
    EXPECT_NO_THROW(agent->addHandler(kRoot, std::make_shared<Mib>(kRoot)));
}

TEST_F(AgentTest, RemovingWhatIsNotRegisteredThrows) {
    EXPECT_THROW(agent->removeHandler(kRoot), Error);
    Mib foreign(kRoot);
    EXPECT_THROW(agent->removeMib(foreign), Error);  // not created by this Agent
}

TEST_F(AgentTest, RemovingTwiceThrows) {
    agent->addMib(kRoot);
    agent->removeHandler(kRoot);
    EXPECT_THROW(agent->removeHandler(kRoot), Error);
}

TEST_F(AgentTest, OtherSubtreesStayRegistered) {
    const Oid other = Oid::parse("1.3.6.1.4.1.99999.901");
    agent->addMib(kRoot);
    agent->addMib(other);
    agent->removeHandler(kRoot);
    EXPECT_THROW(agent->addMib(other), Error);  // still registered
    EXPECT_NO_THROW(agent->addMib(kRoot));
}

// --- DataAgent(agent, data) -------------------------------------------------------------------

TEST_F(AgentTest, DataAgentHoldsItsRootWhileAlive) {
    t::Data data;
    t::DataAgent adapter(agent, data);
    EXPECT_THROW(agent->addMib(t::oids::root), Error);
}

TEST_F(AgentTest, DestroyingTheDataAgentRemovesItsSubtree) {
    t::Data data;
    { t::DataAgent adapter(agent, data); }
    EXPECT_NO_THROW(agent->addMib(t::oids::root)->scalar(1, constant(7)));  // the root is free again
}

TEST_F(AgentTest, ANewDataAgentCanFollowTheOldOne) {  // e.g. reconfiguration at run time
    t::Data data;
    auto adapter = std::make_unique<t::DataAgent>(agent, data);
    adapter.reset();
    EXPECT_NO_THROW(adapter = std::make_unique<t::DataAgent>(agent, data));
}

// --- runInBackground() ------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;

TEST_F(AgentTest, RunInBackgroundReturnsAndStopEndsTheLoopAtOnce) {
    agent->runInBackground();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // the loop is waiting in select()
    const auto t0 = Clock::now();
    agent->stop();
    agent->wait();
    EXPECT_LT(Clock::now() - t0, std::chrono::milliseconds(500));  // woken up, not only at the 1 s heartbeat
}

TEST_F(AgentTest, RegistrationsFromAnotherThreadRunOnTheLoopAndPassErrorsOn) {
    agent->runInBackground();
    auto mib = std::make_shared<Mib>(kRoot);
    mib->scalar(1, constant(1));  // complete before it is registered
    agent->addHandler(kRoot, mib);
    EXPECT_THROW(agent->addMib(kRoot), Error);  // the exception of the loop thread reaches the caller
    agent->removeMib(*mib);
    EXPECT_THROW(agent->removeHandler(kRoot), Error);
    EXPECT_NO_THROW(agent->addMib(kRoot));  // removed on the loop: Net-SNMP was told
}

TEST_F(AgentTest, DataAgentsComeAndGoWhileTheLoopRuns) {
    agent->runInBackground();
    t::Data data;
    for (int i = 0; i < 3; ++i) {
        t::DataAgent adapter(agent, data);
        EXPECT_THROW(agent->addMib(t::oids::root), Error);
    }
    EXPECT_NO_THROW(agent->addMib(t::oids::root));  // the last one removed its subtree
}

TEST_F(AgentTest, NotificationsFromSeveralThreadsAreQueued) {
    agent->runInBackground();
    std::vector<std::thread> senders;
    for (int i = 0; i < 4; ++i)
        senders.emplace_back([this, i] {
            for (int n = 0; n < 25; ++n) agent->sendTrap(kRoot + Oid({3, 0, 1}), {{kRoot + Oid({1, 0}), Value::integer(i)}});
        });
    for (auto& s : senders) s.join();
    agent->stop();
    agent->wait();  // the queue is sent before the loop ends (dropped here: no master)
}

TEST_F(AgentTest, PollRunAndASecondBackgroundLoopAreRefused) {
    agent->runInBackground();
    EXPECT_THROW(agent->poll(false), Error);
    EXPECT_THROW(agent->run(), Error);
    EXPECT_THROW(agent->runInBackground(), Error);
}

TEST_F(AgentTest, AStoppedAgentDoesNotRunInBackground) {
    agent->stop();
    EXPECT_THROW(agent->runInBackground(), Error);
}

TEST_F(AgentTest, AfterTheLoopEndedTheCallerDoesTheWorkItself) {
    agent->runInBackground();
    agent->stop();
    agent->wait();
    EXPECT_NO_THROW(agent->addMib(kRoot));
    EXPECT_THROW(agent->addMib(kRoot), Error);
    EXPECT_NO_THROW(agent->sendTrap(kRoot + Oid({3, 0, 1})));
}

TEST_F(AgentTest, WaitWithoutABackgroundLoopReturnsAtOnce) { EXPECT_NO_THROW(agent->wait()); }

TEST_F(AgentTest, TheDestructorEndsTheBackgroundLoop) {
    agent->runInBackground();
    agent.reset();  // stop() + wait() + shutdown; must neither hang nor crash
    SUCCEED();
}

}  // namespace
