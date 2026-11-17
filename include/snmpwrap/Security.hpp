/**
 * @file Security.hpp
 * @brief SNMPv3 (USM) security settings shared by Client (SessionConfig) and NotificationReceiver (NotificationUser).
 *
 * Which algorithms work depends on how Net-SNMP was built: with OpenSSL all of them; with --with-openssl=internal
 * MD5 / SHA1 / AES128 (no DES). SHA-2 needs Net-SNMP 5.8+ with OpenSSL; AES192/256 additionally
 * --enable-blumenthal-aes. An unavailable algorithm is reported as an Error where it is used.
 */
#pragma once

namespace snmpwrap {

/// @brief SNMPv3 security level (ordered: a higher level includes the lower ones).
enum class SecurityLevel { NoAuthNoPriv, AuthNoPriv, AuthPriv };

/// @brief SNMPv3 authentication protocol.
enum class AuthProtocol { MD5, SHA1, SHA224, SHA256, SHA384, SHA512 };

/// @brief SNMPv3 privacy (encryption) protocol.
enum class PrivProtocol { DES, AES128, AES192, AES256 };

}  // namespace snmpwrap
