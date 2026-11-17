#pragma once
// Internal: SNMPv3 (USM) protocol OIDs and names for Client (SessionSetup) and NotificationReceiver (users).

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

#include <cstddef>

#include "snmpwrap/Security.hpp"

namespace snmpwrap::detail {

// SHA-2 (RFC 7860) and AES192/256 (Blumenthal draft) protocol OIDs. Defined here because Net-SNMP only exports its
// own copies when built with OpenSSL / --enable-blumenthal-aes; if the library lacks the algorithm, key derivation or
// session opening fails and is reported as an Error.
inline const ::oid kUsmSHA224[] = {1, 3, 6, 1, 6, 3, 10, 1, 1, 4};
inline const ::oid kUsmSHA256[] = {1, 3, 6, 1, 6, 3, 10, 1, 1, 5};
inline const ::oid kUsmSHA384[] = {1, 3, 6, 1, 6, 3, 10, 1, 1, 6};
inline const ::oid kUsmSHA512[] = {1, 3, 6, 1, 6, 3, 10, 1, 1, 7};
inline const ::oid kUsmAES192[] = {1, 3, 6, 1, 4, 1, 14832, 1, 3};
inline const ::oid kUsmAES256[] = {1, 3, 6, 1, 4, 1, 14832, 1, 4};

/// An OID of Net-SNMP's protocol tables.
struct ProtocolOid {
    const ::oid* oid;
    std::size_t length;
};

/// @brief The OID of an authentication protocol. @param[in] p The protocol. @return Its OID.
inline ProtocolOid authProtocolOid(AuthProtocol p) {
    switch (p) {
        case AuthProtocol::MD5: return {usmHMACMD5AuthProtocol, OID_LENGTH(usmHMACMD5AuthProtocol)};
        case AuthProtocol::SHA1: return {usmHMACSHA1AuthProtocol, OID_LENGTH(usmHMACSHA1AuthProtocol)};
        case AuthProtocol::SHA224: return {kUsmSHA224, OID_LENGTH(kUsmSHA224)};
        case AuthProtocol::SHA256: return {kUsmSHA256, OID_LENGTH(kUsmSHA256)};
        case AuthProtocol::SHA384: return {kUsmSHA384, OID_LENGTH(kUsmSHA384)};
        case AuthProtocol::SHA512: return {kUsmSHA512, OID_LENGTH(kUsmSHA512)};
    }
    return {usmHMACSHA1AuthProtocol, OID_LENGTH(usmHMACSHA1AuthProtocol)};
}

/// @brief The OID of a privacy protocol. @param[in] p The protocol. @return Its OID.
inline ProtocolOid privProtocolOid(PrivProtocol p) {
    switch (p) {
        case PrivProtocol::DES: return {usmDESPrivProtocol, OID_LENGTH(usmDESPrivProtocol)};
        case PrivProtocol::AES128: return {usmAESPrivProtocol, OID_LENGTH(usmAESPrivProtocol)};
        case PrivProtocol::AES192: return {kUsmAES192, OID_LENGTH(kUsmAES192)};
        case PrivProtocol::AES256: return {kUsmAES256, OID_LENGTH(kUsmAES256)};
    }
    return {usmAESPrivProtocol, OID_LENGTH(usmAESPrivProtocol)};
}

/// @brief The name Net-SNMP's createUser takes for an authentication protocol. @param[in] p The protocol.
inline const char* authProtocolName(AuthProtocol p) {
    switch (p) {
        case AuthProtocol::MD5: return "MD5";
        case AuthProtocol::SHA1: return "SHA";
        case AuthProtocol::SHA224: return "SHA-224";
        case AuthProtocol::SHA256: return "SHA-256";
        case AuthProtocol::SHA384: return "SHA-384";
        case AuthProtocol::SHA512: return "SHA-512";
    }
    return "SHA";
}

/// @brief The name Net-SNMP's createUser takes for a privacy protocol. @param[in] p The protocol.
inline const char* privProtocolName(PrivProtocol p) {
    switch (p) {
        case PrivProtocol::DES: return "DES";
        case PrivProtocol::AES128: return "AES";
        case PrivProtocol::AES192: return "AES-192";
        case PrivProtocol::AES256: return "AES-256";
    }
    return "AES";
}

}  // namespace snmpwrap::detail
