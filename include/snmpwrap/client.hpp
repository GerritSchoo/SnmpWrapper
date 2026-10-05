/**
 * @file client.hpp
 * @brief Synchronous SNMP client (manager side) for SNMPv1, v2c and v3.
 */
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "snmpwrap/error.hpp"
#include "snmpwrap/oid.hpp"
#include "snmpwrap/value.hpp"

namespace snmpwrap {

/**
 * @brief Connection and security settings of a Client.
 *
 * @code
 * SessionConfig v2;                       // SNMPv2c, community "public"
 * v2.peer = "192.168.1.10:161";
 *
 * SessionConfig v3;
 * v3.peer = "192.168.1.10";
 * v3.version = SessionConfig::Version::V3;
 * v3.user = "admin";
 * v3.securityLevel = SessionConfig::SecurityLevel::AuthPriv;
 * v3.authProtocol = SessionConfig::AuthProtocol::SHA1;   v3.authPassphrase = "authpass123";
 * v3.privProtocol = SessionConfig::PrivProtocol::AES128; v3.privPassphrase = "privpass123";
 * @endcode
 */
struct SessionConfig {
    /// @brief SNMP protocol version.
    enum class Version { V1, V2c, V3 };
    /// @brief SNMPv3 security level.
    enum class SecurityLevel { NoAuthNoPriv, AuthNoPriv, AuthPriv };
    /// @brief SNMPv3 authentication protocol.
    enum class AuthProtocol { MD5, SHA1, SHA224, SHA256, SHA384, SHA512 };
    /// @brief SNMPv3 privacy (encryption) protocol.
    enum class PrivProtocol { DES, AES128, AES192, AES256 };

    /// Agent address "[udp:|tcp:]host[:port]", e.g. "127.0.0.1:161" (port defaults to 161).
    std::string peer = "127.0.0.1:161";
    /// Protocol version.
    Version version = Version::V2c;
    /// Community string (v1 / v2c only).
    std::string community = "public";

    /// @name SNMPv3 (USM)
    /// Which algorithms work depends on how Net-SNMP was built (with OpenSSL: all;
    /// --with-openssl=internal: MD5/SHA1/AES128, no DES). SHA-2 needs Net-SNMP 5.8+ with OpenSSL;
    /// AES192/256 additionally need --enable-blumenthal-aes. The constructor / the first request
    /// fails with Error / TransportError if the chosen protocol is unavailable. Keys are derived from the passphrases
    /// (min. 8 characters) and localized by Net-SNMP during engine discovery.
    /// @{
    std::string user;                                         ///< USM user name.
    SecurityLevel securityLevel = SecurityLevel::NoAuthNoPriv; ///< Security level.
    AuthProtocol authProtocol = AuthProtocol::SHA1;           ///< Authentication protocol.
    std::string authPassphrase;                               ///< Authentication passphrase.
    PrivProtocol privProtocol = PrivProtocol::AES128;         ///< Privacy protocol.
    std::string privPassphrase;                               ///< Privacy passphrase.
    std::string contextName;                                  ///< SNMPv3 context; empty = default context.
    /// @}

    /// Timeout per try.
    std::chrono::milliseconds timeout{3000};
    /// Number of retries after a timeout (total tries = retries + 1).
    int retries = 1;
};

/**
 * @brief Synchronous SNMP client session (RAII: opened in the constructor, closed in the destructor).
 *
 * @code
 * Client c(cfg);
 * VarBind v = c.get(Oid::parse("1.3.6.1.2.1.1.1.0"));
 * std::cout << v.value.str() << "\n";
 * for (const VarBind& vb : c.walk(Oid::parse("1.3.6.1.2.1.2.2")))   // ifTable
 *     std::cout << vb.oid.str() << " = " << vb.value.str() << "\n";
 * c.set(Oid::parse("1.3.6.1.2.1.1.6.0"), Value::string("server room"));
 * @endcode
 *
 * Error handling:
 * - TransportError – timeout, network problem, SNMPv3 authentication / privacy failure;
 * - ResponseError  – the agent answered with an error-status (e.g. WrongValue, NotWritable);
 * - for v2c / v3, the per-varbind exceptions noSuchObject / noSuchInstance / endOfMibView are NOT
 *   thrown: they come back as a Value of the matching Type (see Value::isException()).
 *   SNMPv1 has no such markers; the agent then answers with ResponseError(NoSuchName).
 *
 * @note Not thread-safe: use one Client per thread. When an Agent lives in the same process, create
 *       the Agent first (see Agent).
 */
class Client {
public:
    /**
     * @brief Opens the session (for SNMPv3 including engine discovery).
     * @param[in] config Address, version and security settings.
     * @throws TransportError if the session cannot be opened (e.g. unknown host, v3 discovery failed).
     * @throws Error if the SNMPv3 keys cannot be derived.
     */
    explicit Client(const SessionConfig& config);

    /// @brief Closes the session.
    ~Client();

    /// @brief Moves the session; the moved-from object must not be used afterwards.
    Client(Client&&) noexcept;
    /// @brief Moves the session; the moved-from object must not be used afterwards.
    Client& operator=(Client&&) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /**
     * @brief GET of one instance.
     * @param[in] oid Instance OID, e.g. 1.3.6.1.2.1.1.1.0 (sysDescr.0).
     * @return The varbind; for v2c/v3 the value may be an exception marker (NoSuchObject / NoSuchInstance).
     * @throws TransportError, ResponseError
     */
    VarBind get(const Oid& oid);

    /**
     * @brief GET of several instances in one request.
     * @param[in] oids Instance OIDs.
     * @return One varbind per OID, in the same order.
     * @throws TransportError, ResponseError
     */
    std::vector<VarBind> get(const std::vector<Oid>& oids);

    /**
     * @brief GETNEXT: the first instance after @p oid.
     * @param[in] oid Any OID.
     * @return The next varbind; for v2c/v3 Type::EndOfMibView if nothing follows.
     * @throws TransportError, ResponseError (v1: NoSuchName at the end of the MIB)
     */
    VarBind getNext(const Oid& oid);

    /**
     * @brief GETBULK (SNMPv2c / v3 only).
     * @param[in] oids           Start OIDs.
     * @param[in] nonRepeaters   Number of leading OIDs that get only one GETNEXT.
     * @param[in] maxRepetitions Number of GETNEXT steps for each remaining OID.
     * @return All returned varbinds (may end with EndOfMibView markers).
     * @throws Error on SNMPv1; TransportError, ResponseError.
     */
    std::vector<VarBind> getBulk(const std::vector<Oid>& oids, int nonRepeaters = 0, int maxRepetitions = 10);

    /**
     * @brief SET of one instance.
     * @param[in] oid   Instance OID.
     * @param[in] value New value; its Type must match the object's type in the agent.
     * @throws TransportError, ResponseError (e.g. NotWritable, WrongType, WrongValue).
     */
    void set(const Oid& oid, const Value& value);

    /**
     * @brief SET of several instances in ONE atomic request: either all are applied or none.
     * @param[in] varbinds Instances and new values, e.g. a table row together with its RowStatus.
     * @throws TransportError, ResponseError (ResponseError::index() names the failing varbind, 1-based).
     *
     * @code
     * // create row 7 of a table with RowStatus (column 4) in one request
     * c.set({{tbl + Oid{1, 2, 7}, Value::string("eth7")},
     *        {tbl + Oid{1, 4, 7}, Value::integer(4)}});   // createAndGo
     * @endcode
     */
    void set(const std::vector<VarBind>& varbinds);

    /**
     * @brief Walks the subtree below @p root and calls @p callback for every instance.
     * @param[in] root     Subtree to walk.
     * @param[in] callback Called in OID order; return false to stop early.
     * @throws TransportError, ResponseError; Error if the agent returns non-increasing OIDs.
     * @note Uses GETBULK on v2c / v3 and GETNEXT on v1.
     */
    void walk(const Oid& root, const std::function<bool(const VarBind&)>& callback);

    /**
     * @brief Walks the subtree below @p root and collects all instances.
     * @param[in] root Subtree to walk.
     * @return All varbinds in OID order.
     * @throws as walk(const Oid&, const std::function<bool(const VarBind&)>&).
     */
    std::vector<VarBind> walk(const Oid& root);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snmpwrap
