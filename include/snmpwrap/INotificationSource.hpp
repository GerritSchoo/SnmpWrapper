/**
 * @file INotificationSource.hpp
 * @brief Interface INotificationSource (delivers received notifications) and the Notification it delivers -
 *        implemented by NotificationReceiver; mock it in tests.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "snmpwrap/Oid.hpp"
#include "snmpwrap/Value.hpp"

namespace snmpwrap {

/// @brief One received notification.
struct Notification {
    Oid trapOid;                ///< Which notification (snmpTrapOID.0; v1 traps are converted as in RFC 3584).
    std::uint32_t uptime = 0;   ///< sysUpTime.0 of the sender (hundredths of a second).
    std::vector<VarBind> vars;  ///< The varbinds (the OBJECTS of the notification), without sysUpTime.0 / snmpTrapOID.0.
    std::string source;         ///< Sender address, e.g. "UDP: [127.0.0.1]:48531->[127.0.0.1]:1162".
    std::string community;      ///< Community string of the message.
    int version = 2;            ///< 1 = SNMPv1 trap, 2 = SNMPv2c trap or inform.
    bool inform = false;        ///< True for an inform (acknowledged automatically).
};


/**
 * @brief Something that delivers received notifications - implemented by NotificationReceiver; mock it in tests. The
 *        generated `Notifications` class takes a INotificationSource&.
 */
class INotificationSource {
public:
    virtual ~INotificationSource() = default;
    /// @brief Adds a handler that gets every notification. @param[in] handler The handler.
    virtual void onNotification(std::function<void(const Notification&)> handler) = 0;
};

}  // namespace snmpwrap
