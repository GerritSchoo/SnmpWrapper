// gmock mock of INotificationSource (INotificationSource.hpp); keeps the handler registered last, so a test can feed notifications.
#pragma once

#include <gmock/gmock.h>

#include <functional>

#include "snmpwrap/INotificationSource.hpp"

namespace snmpwrap::test {

/// gmock; keeps the handler registered last, so a test can feed notifications through it.
class MockNotificationSource : public INotificationSource {
public:
    MOCK_METHOD(void, onNotification, (std::function<void(const Notification&)>), (override));

    std::function<void(const Notification&)> handler;

    MockNotificationSource() {
        ON_CALL(*this, onNotification(::testing::_)).WillByDefault([this](std::function<void(const Notification&)> h) {
            handler = std::move(h);
        });
    }
};

}  // namespace snmpwrap::test
