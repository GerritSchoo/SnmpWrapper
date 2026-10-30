// The generated typed client Remote (NESTED-TEST-MIB) against a mocked Session: OIDs, types and the MIB
// check come from the generated code; nothing reaches the network.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "nested_test_mib.hpp"
#include "support/mocks.hpp"
#include "support/test_support.hpp"

namespace m = nested_test_mib;
using namespace snmpwrap;
using namespace snmpwrap::test;
using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::Return;

namespace {

/// Remote on a MockSession that answers like the FakeSession `agent` unless a test sets an expectation.
class RemoteTest : public ::testing::Test {
protected:
    RemoteTest() {
        agent.values[scalar(m::oids::boatName)] = Value::string("mocked");
        session.delegateTo(agent);
    }

    /// One varbind with this OID and value.
    static auto VarBindIs(const Oid& oid, const Value& value) {
        return ::testing::AllOf(Field(&VarBind::oid, oid), Field(&VarBind::value, value));
    }

    FakeSession agent;
    ::testing::NiceMock<MockSession> session;
    m::Remote remote{session};
};

TEST_F(RemoteTest, GetReadsTheScalarOid) {
    EXPECT_CALL(session, get(ElementsAre(scalar(m::oids::boatName))));
    EXPECT_EQ(remote.boatName.get(), "mocked");
}

TEST_F(RemoteTest, SetSendsTheEnumAsInteger) {
    EXPECT_CALL(session, set(ElementsAre(VarBindIs(scalar(m::oids::navigationMode), Value::integer(2)))));
    remote.navigation.navigationMode.set(m::NavigationMode::automatic);
}

TEST_F(RemoteTest, InvalidValueNeverReachesTheSession) {  // SIZE (1..16)
    EXPECT_CALL(session, set(_)).Times(0);
    EXPECT_THROW(remote.boatName.set(std::string(40, 'x')), SetError);
}

TEST_F(RemoteTest, TableCellSetUsesTheRowIndex) {
    EXPECT_CALL(session, set(ElementsAre(VarBindIs(cell(m::oids::extendedRollValue, m::ExtendedRollEntryIndex{3}.toOid()),
                                                   Value::integer(-12)))));
    remote.attitude.extendedRollTable[3].extendedRollValue.set(-12);
}

TEST_F(RemoteTest, ReadWalksIntoData) {
    remote.attitude.extendedRollTable[3].extendedRollValue.set(-12);  // stored by the fake agent
    EXPECT_CALL(session, walk(m::oids::root, _));
    const m::Data snapshot = remote.read();
    EXPECT_EQ(snapshot.boatName, "mocked");
    EXPECT_EQ(snapshot.attitude.extendedRollTable.at(m::ExtendedRollEntryIndex{3}).extendedRollValue, -12);
}

TEST_F(RemoteTest, EmptyAnswerIsAnError) {  // not undefined behaviour
    EXPECT_CALL(session, get(_)).WillOnce(Return(std::vector<VarBind>{}));
    try {
        (void)remote.boatName.get();
        FAIL() << "no Error for an empty answer";
    } catch (const Error& e) {
        EXPECT_THAT(e.what(), ::testing::HasSubstr("no varbind"));
    }
}

TEST_F(RemoteTest, ErrorOfTheSessionIsPassedOn) {
    EXPECT_CALL(session, set(_)).WillOnce([](const std::vector<VarBind>&) {
        throw ResponseError(ErrorStatus::NotWritable, 1, "agent returned error");
    });
    EXPECT_THROW(remote.boatName.set("neptune"), ResponseError);
}

}  // namespace
