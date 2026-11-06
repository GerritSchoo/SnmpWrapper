// Unit tests of ISession.hpp: the non-virtual helpers of the ISession interface - get(oid), set(oid, value),
// walk(root) - on top of a mocked ISession.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <vector>

#include "snmpwrap/Error.hpp"
#include "snmpwrap/ISession.hpp"
#include "snmpwrap/ISessionMock.hpp"
#include "TestSupport.hpp"

using namespace snmpwrap;
using namespace snmpwrap::test;
using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::Invoke;
using ::testing::Return;

namespace {

const Oid kA{1, 3, 6, 1, 2, 1, 1, 1, 0};
const Oid kB{1, 3, 6, 1, 2, 1, 1, 3, 0};

class SessionTest : public ::testing::Test {
protected:
    ::testing::StrictMock<MockSession> session;
};

TEST_F(SessionTest, GetOfOneOidAsksForExactlyThatOid) {
    EXPECT_CALL(session, get(ElementsAre(kA))).WillOnce(Return(std::vector<VarBind>{{kA, Value::string("x")}}));
    const VarBind vb = session.get(kA);
    EXPECT_EQ(vb.oid, kA);
    EXPECT_EQ(vb.value, Value::string("x"));
}

TEST_F(SessionTest, GetOfOneOidReturnsTheFirstVarbind) {
    EXPECT_CALL(session, get(_)).WillOnce(Return(std::vector<VarBind>{{kA, Value::integer(1)}, {kB, Value::integer(2)}}));
    EXPECT_EQ(session.get(kA).value, Value::integer(1));
}

TEST_F(SessionTest, GetWithoutAnswerIsAnError) {
    EXPECT_CALL(session, get(_)).WillOnce(Return(std::vector<VarBind>{}));
    EXPECT_THROW(session.get(kA), Error);
}

TEST_F(SessionTest, SetOfOneValueIsOneVarbind) {
    EXPECT_CALL(session, set(ElementsAre(::testing::AllOf(Field(&VarBind::oid, kA), Field(&VarBind::value, Value::integer(5))))));
    session.set(kA, Value::integer(5));
}

TEST_F(SessionTest, SetPassesErrorsOn) {
    EXPECT_CALL(session, set(_)).WillOnce([](const std::vector<VarBind>&) {
        throw ResponseError(ErrorStatus::NotWritable, 1, "refused");
    });
    EXPECT_THROW(session.set(kA, Value::integer(5)), ResponseError);
}

TEST_F(SessionTest, WalkCollectsEverythingInOrder) {
    EXPECT_CALL(session, walk(Oid({1, 3, 6}), _))
        .WillOnce(Invoke([](const Oid&, const std::function<bool(const VarBind&)>& cb) {
            cb({kA, Value::integer(1)});
            cb({kB, Value::integer(2)});
        }));
    const std::vector<VarBind> all = session.walk(Oid({1, 3, 6}));
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].oid, kA);
    EXPECT_EQ(all[1].oid, kB);
}

TEST_F(SessionTest, WalkCollectorNeverStopsEarly) {
    bool kept = false;
    EXPECT_CALL(session, walk(_, _)).WillOnce(Invoke([&](const Oid&, const std::function<bool(const VarBind&)>& cb) {
        kept = cb({kA, Value::integer(1)});
    }));
    session.walk(Oid({1}));
    EXPECT_TRUE(kept);
}

TEST_F(SessionTest, WalkOfAnEmptySubtreeIsEmpty) {
    EXPECT_CALL(session, walk(_, _));
    EXPECT_TRUE(session.walk(Oid({1})).empty());
}

// --- the FakeSession used by the tests of the generated code behaves like an agent ---------------

TEST(FakeSession, AnswersFromItsValuesAndStoresSets) {
    FakeSession fake;
    fake.values[kA] = Value::integer(1);
    EXPECT_EQ(fake.get(kA).value, Value::integer(1));
    EXPECT_EQ(fake.get(kB).value, Value::exception(Type::NoSuchInstance));
    fake.set(kB, Value::integer(2));
    EXPECT_EQ(fake.get(kB).value, Value::integer(2));
    EXPECT_EQ(fake.sets.size(), 1u);
}

}  // namespace
