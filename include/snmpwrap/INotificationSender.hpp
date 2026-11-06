/**
 * @file INotificationSender.hpp
 * @brief Interface INotificationSender (sends notifications) - implemented by Agent; mock it in tests.
 */
#pragma once

#include <vector>

#include "snmpwrap/Oid.hpp"
#include "snmpwrap/Value.hpp"

namespace snmpwrap {

/**
 * @brief Something that sends notifications - implemented by Agent; mock it in tests.
 * The generated `send<Name>()` functions take a INotificationSender&.
 */
class INotificationSender {
public:
    virtual ~INotificationSender() = default;
    /// @brief Sends a notification. @param[in] trapOid The NOTIFICATION-TYPE OID. @param[in] vars Its OBJECTS.
    virtual void sendTrap(const Oid& trapOid, const std::vector<VarBind>& vars) = 0;
};

}  // namespace snmpwrap
