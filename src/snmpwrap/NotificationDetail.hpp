#pragma once
// Internal: what a received PDU means as a Notification - separate from NotificationReceiver (which only
// owns the socket, acknowledges informs and calls the handlers) so that it can be tested with hand-made PDUs.

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

#include <map>
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

    /// @brief Accept SNMPv3 notifications of @p user at @p minLevel or above (NotificationReceiver::addUser()).
    void acceptUser(std::string user, SecurityLevel minLevel) { users_[std::move(user)] = minLevel; }

    /// @brief Drop v1 / v2c notifications (NotificationReceiver::acceptOnlyV3()).
    void acceptOnlyV3() { onlyV3_ = true; }

    /**
     * @brief The notification in a received PDU.
     * @param[in] pdu    The received PDU.
     * @param[in] source Sender address as text (Notification::source).
     * @return The notification, or std::nullopt if @p pdu is no notification, or not accepted: v1 / v2c with another
     *         community (or after acceptOnlyV3()), v3 of a user not accepted or below the user's level.
     */
    std::optional<Notification> decode(const netsnmp_pdu& pdu, std::string source) const;

private:
    std::vector<std::string> accepted_;
    std::map<std::string, SecurityLevel> users_;
    bool onlyV3_ = false;
};

/**
 * @brief Makes Net-SNMP accept SNMPv3 messages of @p user (USM user table, as snmptrapd's createUser).
 * @param[in] user The user.
 * @throws Error if a field is invalid or Net-SNMP refuses the user (passphrase shorter than 8 characters, protocol
 *         not in this Net-SNMP build); the message names the reason.
 */
void createUsmUser(const NotificationUser& user);

}  // namespace snmpwrap::detail
