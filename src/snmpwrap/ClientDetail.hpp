#pragma once
// Internal: the parts of Client that need no network - checking and applying a SessionConfig to a Net-SNMP
// session, the source address, reading a response and the walk loop. Client itself only adds the socket.

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "snmpwrap/Client.hpp"

namespace snmpwrap::detail {

/// The settings of a Client, checked and applied to a Net-SNMP session.
class SessionSetup {
public:
    /// @param[in] config The settings (copied). @throws Error for settings that cannot work (localAddress with a TCP peer).
    explicit SessionSetup(const SessionConfig& config);

    /// @brief The settings. @return The copy taken by the constructor.
    const SessionConfig& config() const noexcept { return cfg_; }

    /**
     * Fills a session initialized with snmp_sess_init(): peer, timeout (microseconds), retries, version and
     * community, or the SNMPv3 user, context, security level, protocols and the keys derived from the passphrases.
     * The session points into the strings of this object: keep it alive until snmp_sess_open() copied them.
     * @throws Error if a key cannot be derived (passphrase shorter than 8 characters, algorithm missing in this build).
     */
    void configure(netsnmp_session& sess) const;

private:
    SessionConfig cfg_;
};

/// Net-SNMP reads the source address of a client from the process-wide setting "clientaddr" when it creates the
/// socket (in snmp_sess_open); netsnmp_session::localname is not used for that. Sets it for one snmp_sess_open
/// and restores what was there before (e.g. clientaddr from snmp.conf). Holds a lock: only one open at a time.
class ClientAddrScope {
public:
    /// @brief "192.168.1.5:5000" / "[fe80::1]:5000" name a port, "192.168.1.5" / "fe80::1" do not.
    static bool namesPort(const std::string& address);

    explicit ClientAddrScope(const std::string& address) : lock_(mutex()) {
        if (address.empty()) return;
        if (const char* old = netsnmp_ds_get_string(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_CLIENT_ADDR)) oldAddr_ = old;
        oldUsesPort_ = netsnmp_ds_get_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_CLIENT_ADDR_USES_PORT) != 0;
        netsnmp_ds_set_string(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_CLIENT_ADDR, address.c_str());
        netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_CLIENT_ADDR_USES_PORT, namesPort(address) ? 1 : 0);
        set_ = true;
    }
    ~ClientAddrScope() {
        if (!set_) return;
        netsnmp_ds_set_string(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_CLIENT_ADDR, oldAddr_ ? oldAddr_->c_str() : nullptr);
        netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_CLIENT_ADDR_USES_PORT, oldUsesPort_ ? 1 : 0);
    }
    ClientAddrScope(const ClientAddrScope&) = delete;
    ClientAddrScope& operator=(const ClientAddrScope&) = delete;

private:
    static std::mutex& mutex() {
        static std::mutex m;
        return m;
    }
    std::lock_guard<std::mutex> lock_;
    std::optional<std::string> oldAddr_;
    bool oldUsesPort_ = false;
    bool set_ = false;
};

/// Reads the response to one request: its error-status and its varbinds.
class ResponseReader {
public:
    /// @param[in] requestType SNMP_MSG_GET / GETNEXT / GETBULK / SET. @param[in] requested Varbinds sent.
    /// @param[in] peer Agent address, for messages.
    ResponseReader(int requestType, std::size_t requested, std::string peer);

    /**
     * @brief The varbinds of @p response.
     * @throws ResponseError for an error-status (index() as sent by the agent, 1-based);
     *         Error if a GET / GETNEXT / SET answer has another number of varbinds (RFC 3416 4.2), or for a value of an
     *         ASN.1 type snmpwrap cannot represent.
     */
    std::vector<VarBind> read(const netsnmp_pdu& response) const;

private:
    int type_;
    std::size_t requested_;
    std::string peer_;
};

/// The walk loop of Client::walk(), independent of how the instances are fetched.
class Walker {
public:
    /// Fetches the instances after an OID: one GETNEXT on v1, one GETBULK otherwise.
    using Fetch = std::function<std::vector<VarBind>(const Oid& after)>;

    /// @param[in] root Subtree to walk. @param[in] v1 SNMPv1 (end of MIB = ResponseError(NoSuchName)). @param[in] fetch The requests.
    Walker(Oid root, bool v1, Fetch fetch);

    /**
     * @brief Calls @p callback for every instance below the root, in OID order, until the end of the subtree,
     *        EndOfMibView, an empty answer or @p callback returns false.
     * @throws Error if the agent returns an OID that is not greater than the previous one (the walk would never end);
     *         whatever @p fetch throws (except the v1 end of MIB).
     */
    void walk(const std::function<bool(const VarBind&)>& callback) const;

private:
    Oid root_;
    bool v1_;
    Fetch fetch_;
};

}  // namespace snmpwrap::detail
