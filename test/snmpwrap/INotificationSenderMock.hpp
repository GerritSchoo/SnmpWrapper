// gmock mock of INotificationSender (INotificationSender.hpp).
#pragma once

#include <gmock/gmock.h>

#include <vector>

#include "snmpwrap/INotificationSender.hpp"

namespace snmpwrap::test {

class MockNotificationSender : public INotificationSender {
public:
    MOCK_METHOD(void, sendTrap, (const Oid&, const std::vector<VarBind>&), (override));
};

}  // namespace snmpwrap::test
