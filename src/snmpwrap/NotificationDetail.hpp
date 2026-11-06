#pragma once
// Internal: what a received PDU means as a Notification - separate from NotificationReceiver (which only
// owns the socket, acknowledges informs and calls the handlers) so that it can be tested with hand-made PDUs.

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

#include <optional>
#include <string>
#include <vector>

#include "snmpwrap/NotificationReceiver.hpp"

namespace snmpwrap::detail {

/**
 * Turns received PDUs into Notifications.
 * - SNMPv2c trap / inform: trapOid and uptime from the snmpTrapOID.0 / sysUpTime.0 varbinds, the rest in vars.
 * - SNMPv1 trap: trapOid as in RFC 3584 3.1 (generic traps: snmpTraps.(generic + 1), enterprise-specific:
 *   enterprise.0.specific), uptime from the PDU.
 * Varbinds of a type snmpwrap cannot represent are skipped.
 */
class NotificationDecoder {
public:
    /// @brief Accepts every community.
    NotificationDecoder() = default;

    /// @brief Accept only these communities (NotificationReceiver::acceptCommunities()). @param[in] communities Empty = all.
    void acceptCommunities(std::vector<std::string> communities) { accepted_ = std::move(communities); }

    /**
     * @brief The notification in a received PDU.
     * @param[in] pdu    The received PDU.
     * @param[in] source Sender address as text (Notification::source).
     * @return The notification, or std::nullopt if @p pdu is no notification or its community is not accepted.
     */
    std::optional<Notification> decode(const netsnmp_pdu& pdu, std::string source) const;

private:
    std::vector<std::string> accepted_;
};

}  // namespace snmpwrap::detail
