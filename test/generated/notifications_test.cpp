// Generated notification code (SNMPWRAPPER-TEST-MIB): send<Name>() through a mocked NotificationSender,
// the typed Notifications dispatcher fed by a mocked NotificationSource.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "snmpwrapper_test_mib.hpp"
#include "support/mocks.hpp"
#include "support/test_support.hpp"

namespace t = snmpwrapper_test_mib;
using namespace snmpwrap;
using namespace snmpwrap::test;
using ::testing::_;
using ::testing::SaveArg;

namespace {

TEST(Notifications, SendPutsTheObjectsIntoTheVarbinds) {
    MockNotificationSender sender;
    std::vector<VarBind> vars;
    EXPECT_CALL(sender, sendTrap(t::oids::swtAlarm, _)).WillOnce(SaveArg<1>(&vars));
    t::sendSwtAlarm(sender, "pump", 42);
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0].value, Value::string("pump"));
    EXPECT_EQ(vars[1].value, Value::integer(42));
}

TEST(Notifications, ReceivedNotificationReachesTheTypedHandler) {
    ::testing::NiceMock<MockNotificationSource> source;
    EXPECT_CALL(source, onNotification(_));  // the dispatcher registers itself once
    t::Notifications notifications(source);
    ASSERT_TRUE(source.handler);

    std::string name;
    std::int32_t limit = 0;
    notifications.onSwtAlarm([&](const t::SwtAlarm& a) {
        name = a.swtName;
        limit = a.swtLimit;
    });

    // what an agent sends: captured from send<Name>()
    MockNotificationSender sender;
    Notification n;
    n.trapOid = t::oids::swtAlarm;
    EXPECT_CALL(sender, sendTrap(_, _)).WillOnce(SaveArg<1>(&n.vars));
    t::sendSwtAlarm(sender, "pump", 42);

    source.handler(n);
    EXPECT_EQ(name, "pump");
    EXPECT_EQ(limit, 42);
}

TEST(Notifications, OtherTrapOidsAreIgnored) {
    ::testing::NiceMock<MockNotificationSource> source;
    t::Notifications notifications(source);
    bool called = false;
    notifications.onSwtAlarm([&](const t::SwtAlarm&) { called = true; });
    Notification n;
    n.trapOid = Oid({1, 3, 6, 1, 6, 3, 1, 1, 5, 1});  // coldStart
    source.handler(n);
    EXPECT_FALSE(called);
}

}  // namespace
