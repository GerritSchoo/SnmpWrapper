// The generated typed client RemoteClient (NESTED-TEST-MIB) against a mocked ISession: OIDs, types and the MIB
// check come from the generated code; nothing reaches the network.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "nested_test_mib.hpp"
#include "snmpwrap/ISessionMock.hpp"
#include "snmpwrap/INotificationSenderMock.hpp"
#include "snmpwrap/INotificationSourceMock.hpp"
#include "TestSupport.hpp"

namespace m = nested_test_mib;
using namespace snmpwrap;
using namespace snmpwrap::test;
using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::Return;

namespace {

/// RemoteClient on a MockSession that answers like the FakeSession `agent` unless a test sets an expectation.
class RemoteTest : public ::testing::Test {
protected:
    RemoteTest() {
        agent.values[scalar(m::oids::boatName)] = Value::string("mocked");
        session->delegateTo(agent);
    }

    /// One varbind with this OID and value.
    static auto VarBindIs(const Oid& oid, const Value& value) {
        return ::testing::AllOf(Field(&VarBind::oid, oid), Field(&VarBind::value, value));
    }

    FakeSession agent;
    std::shared_ptr<::testing::NiceMock<MockSession>> session = std::make_shared<::testing::NiceMock<MockSession>>();
    m::RemoteClient remote{session};
};

TEST_F(RemoteTest, GetReadsTheScalarOid) {
    EXPECT_CALL(*session, get(ElementsAre(scalar(m::oids::boatName))));
    EXPECT_EQ(remote.boatName.get(), "mocked");
}

TEST_F(RemoteTest, SetSendsTheEnumAsInteger) {
    EXPECT_CALL(*session, set(ElementsAre(VarBindIs(scalar(m::oids::navigationMode), Value::integer(2)))));
    remote.navigation.navigationMode.set(m::NavigationMode::automatic);
}

TEST_F(RemoteTest, InvalidValueNeverReachesTheSession) {  // SIZE (1..16)
    EXPECT_CALL(*session, set(_)).Times(0);
    EXPECT_THROW(remote.boatName.set(std::string(40, 'x')), SetError);
}

TEST_F(RemoteTest, TableCellSetUsesTheRowIndex) {
    EXPECT_CALL(*session, set(ElementsAre(VarBindIs(cell(m::oids::extendedRollValue, m::ExtendedRollEntryIndex{3}.toOid()),
                                                   Value::integer(-12)))));
    remote.attitude.extendedRollTable[3].extendedRollValue.set(-12);
}

TEST_F(RemoteTest, ReadWalksIntoData) {
    remote.attitude.extendedRollTable[3].extendedRollValue.set(-12);  // stored by the fake agent
    EXPECT_CALL(*session, walk(m::oids::root, _));
    const m::Data snapshot = remote.read();
    EXPECT_EQ(snapshot.boatName, "mocked");
    EXPECT_EQ(snapshot.attitude.extendedRollTable.at(m::ExtendedRollEntryIndex{3}).extendedRollValue, -12);
}

TEST_F(RemoteTest, AGroupReadWalksOnlyItsSubtree) {
    remote.attitude.extendedRollTable[3].extendedRollValue.set(-12);
    const Oid attitude{1, 3, 6, 1, 4, 1, 99999, 300, 2};  // NESTED-TEST-MIB: attitude
    EXPECT_CALL(*session, walk(attitude, _));
    const m::Data::AttitudeGroup group = remote.attitude.read();
    EXPECT_EQ(group.extendedRollTable.at(m::ExtendedRollEntryIndex{3}).extendedRollValue, -12);
}

TEST_F(RemoteTest, AGroupReadFillsScalarsAndTablesOfThatGroupOnly) {
    remote.navigation.navigationMode.set(m::NavigationMode::automatic);
    agent.values[cell(m::oids::extendedCourseValue, m::ExtendedCourseEntryIndex{2}.toOid())] = Value::integer(900);
    const m::Data::NavigationGroup group = remote.navigation.read();
    EXPECT_EQ(group.navigationMode, m::NavigationMode::automatic);
    EXPECT_EQ(group.extendedCourseTable.at(m::ExtendedCourseEntryIndex{2}).extendedCourseValue, 900);
    EXPECT_EQ(group.extendedCourseTable.size(), 1u);
}

// --- values that do not fit the MIB (e.g. a wrong type in a simulator's .snmprec) ---------------

/// A RemoteClient on a FakeSession: the agent's values can be anything.
struct WrongAgent {
    std::shared_ptr<FakeSession> agent = std::make_shared<FakeSession>();
    m::RemoteClient remote{agent};
};

TEST(RemoteClientUnexpectedValue, AWrongTypeNamesObjectOidAgentAndTheMibDefinition) {
    WrongAgent w;
    const Oid oid = scalar(m::oids::boatName);
    w.agent->values[oid] = Value::integer(5);  // the MIB: DisplayString (SIZE (1..16))
    try {
        (void)w.remote.boatName.get();
        FAIL() << "no UnexpectedValueError";
    } catch (const UnexpectedValueError& e) {
        const std::string text = e.what();
        EXPECT_THAT(text, ::testing::HasSubstr("boatName.0 (" + oid.str() + ") from fake-agent:161"));
        EXPECT_THAT(text, ::testing::HasSubstr("the agent sent Integer: 5"));
        EXPECT_THAT(text, ::testing::HasSubstr("the MIB defines read-write, DisplayString SIZE(1..16)"));
        EXPECT_THAT(text, ::testing::HasSubstr(".snmprec"));
        EXPECT_EQ(e.oid(), oid);
        EXPECT_EQ(e.received(), Value::integer(5));
    }
}

TEST(RemoteClientUnexpectedValue, AMissingInstanceSaysSo) {
    WrongAgent w;  // the fake agent has nothing: noSuchInstance
    try {
        (void)w.remote.attitude.extendedRollTable[7].extendedRollValue.get();
        FAIL() << "no UnexpectedValueError";
    } catch (const UnexpectedValueError& e) {
        EXPECT_THAT(e.what(), ::testing::HasSubstr("extendedRollValue (" + cell(m::oids::extendedRollValue, m::ExtendedRollEntryIndex{7}.toOid()).str()));
        EXPECT_THAT(e.what(), ::testing::HasSubstr("no row with this index"));
    }
}

TEST(RemoteClientUnexpectedValue, ReadReportsEveryValueThatDoesNotFitInsteadOfLeavingItEmpty) {
    WrongAgent w;
    w.agent->values[scalar(m::oids::boatName)] = Value::integer(5);
    w.agent->values[cell(m::oids::extendedRollValue, m::ExtendedRollEntryIndex{1}.toOid())] = Value::string("12");
    w.agent->values[cell(m::oids::extendedRollSensor, m::ExtendedRollEntryIndex{1}.toOid())] = Value::string("gyro");  // fits
    try {
        (void)w.remote.read();
        FAIL() << "no UnexpectedValueError";
    } catch (const UnexpectedValueError& e) {
        const std::string text = e.what();
        EXPECT_THAT(text, ::testing::HasSubstr("2 values from fake-agent:161 did not fit the MIB"));
        EXPECT_THAT(text, ::testing::HasSubstr("boatName.0"));
        EXPECT_THAT(text, ::testing::HasSubstr("extendedRollValue"));
        EXPECT_THAT(text, ::testing::Not(::testing::HasSubstr("extendedRollSensor")));
    }
}

TEST(RemoteClientUnexpectedValue, GroupTableAndRowReadsReportIt) {
    WrongAgent w;
    const Oid roll = m::ExtendedRollEntryIndex{1}.toOid();
    w.agent->values[cell(m::oids::extendedRollValue, roll)] = Value::string("12");
    w.agent->values[cell(m::oids::extendedRollSensor, roll)] = Value::string("gyro");
    w.agent->values[cell(m::oids::extendedRollCounter, roll)] = Value::counter32(1);
    w.agent->values[cell(m::oids::extendedRollTimepoint, roll)] = Value::timeTicks(1);
    EXPECT_THROW((void)w.remote.attitude.read(), UnexpectedValueError);
    EXPECT_THROW((void)w.remote.attitude.extendedRollTable.read(), UnexpectedValueError);
    EXPECT_THROW((void)w.remote.attitude.extendedRollTable[1].read(), UnexpectedValueError);
}

TEST(RemoteClientUnexpectedValue, ManyValuesAreCutAfterTen) {
    WrongAgent w;
    for (SubId i = 1; i <= 15; ++i)
        w.agent->values[cell(m::oids::extendedRollValue, m::ExtendedRollEntryIndex{static_cast<std::int32_t>(i)}.toOid())] = Value::string("x");
    try {
        (void)w.remote.attitude.extendedRollTable.read();
        FAIL() << "no UnexpectedValueError";
    } catch (const UnexpectedValueError& e) {
        EXPECT_THAT(e.what(), ::testing::HasSubstr("15 values"));
        EXPECT_THAT(e.what(), ::testing::HasSubstr("and 5 more"));
    }
}

TEST_F(RemoteTest, EmptyAnswerIsAnError) {  // not undefined behaviour
    EXPECT_CALL(*session, get(_)).WillOnce(Return(std::vector<VarBind>{}));
    try {
        (void)remote.boatName.get();
        FAIL() << "no Error for an empty answer";
    } catch (const Error& e) {
        EXPECT_THAT(e.what(), ::testing::HasSubstr("no varbind"));
    }
}

TEST_F(RemoteTest, ErrorOfTheSessionIsPassedOn) {
    EXPECT_CALL(*session, set(_)).WillOnce([](const std::vector<VarBind>&) {
        throw ResponseError(ErrorStatus::NotWritable, 1, "agent returned error");
    });
    EXPECT_THROW(remote.boatName.set("neptune"), ResponseError);
}

TEST_F(RemoteTest, KeepsTheSessionAlive) {
    std::weak_ptr<MockSession> watch = session;
    session.reset();
    EXPECT_FALSE(watch.expired());
}

TEST(RemoteClient, NullSessionIsAnError) { EXPECT_THROW(m::RemoteClient(std::shared_ptr<snmpwrap::ISession>()), Error); }

}  // namespace
