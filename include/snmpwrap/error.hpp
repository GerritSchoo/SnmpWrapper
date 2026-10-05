/**
 * @file error.hpp
 * @brief Exception types and SNMP error-status codes used throughout snmpwrap.
 *
 * snmpwrap reports problems with exceptions:
 * - snmpwrap::Error          – base class: invalid arguments, setup problems, programming errors.
 * - snmpwrap::SetError       – thrown by *your* callbacks to reject an SNMP SET with a specific status.
 * - snmpwrap::ResponseError  – thrown by snmpwrap::Client when the remote agent answers with an error-status.
 * - snmpwrap::TransportError – thrown by snmpwrap::Client on timeouts, network or SNMPv3 security failures.
 */
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace snmpwrap {

/**
 * @brief Base class of all exceptions thrown by snmpwrap.
 *
 * Catch this type to handle every snmpwrap error in one place.
 */
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief SNMP error-status codes as defined by RFC 3416 (section 3).
 *
 * The numeric values are identical to the values on the wire, so they can be compared with
 * the numbers printed by Net-SNMP tools. SNMPv1 only knows the codes 0..5; Net-SNMP maps the
 * newer codes to them automatically when talking to v1 managers (e.g. WrongValue -> BadValue).
 */
enum class ErrorStatus : int {
    NoError = 0,              ///< Success.
    TooBig = 1,               ///< Response would not fit into one message.
    NoSuchName = 2,           ///< SNMPv1 only: OID unknown / not writable.
    BadValue = 3,             ///< SNMPv1 only: wrong type, length or value.
    ReadOnly = 4,             ///< SNMPv1 only (rarely used).
    GenErr = 5,               ///< Any other failure.
    NoAccess = 6,             ///< Object exists but is not accessible.
    WrongType = 7,            ///< Value has the wrong ASN.1 type.
    WrongLength = 8,          ///< Value has the wrong length (e.g. string too long).
    WrongEncoding = 9,        ///< Value is wrongly encoded.
    WrongValue = 10,          ///< Value is out of range / not allowed.
    NoCreation = 11,          ///< Instance does not exist and cannot be created.
    InconsistentValue = 12,   ///< Value is valid in general but not in the current state.
    ResourceUnavailable = 13, ///< Not enough resources to perform the SET.
    CommitFailed = 14,        ///< Writing failed; all changes of the request were rolled back.
    UndoFailed = 15,          ///< Writing failed and the rollback failed as well.
    AuthorizationError = 16,  ///< Access denied by the access control of the master agent.
    NotWritable = 17,         ///< Object exists but is read-only.
    InconsistentName = 18,    ///< Instance cannot be created with this name (e.g. row does not exist).
};

/**
 * @brief Rejects an SNMP SET request with a specific error-status.
 *
 * Throw it from a `validate` or `set` callback (see ScalarDef, TableDef). The agent answers the
 * request with @ref status() and, if known, marks the varbind at @ref index() as the culprit.
 * When thrown from a callback, the index is filled in automatically by snmpwrap::Mib.
 *
 * @code
 * [](const Value& v) {
 *     if (v.asInt() < 1 || v.asInt() > 100)
 *         throw SetError(ErrorStatus::WrongValue, "must be 1..100");
 * }
 * @endcode
 */
class SetError : public Error {
public:
    /// Value of index() when the offending varbind is not known.
    static constexpr std::size_t kUnknownIndex = static_cast<std::size_t>(-1);

    /**
     * @brief Creates the exception.
     * @param[in] status SNMP error-status returned to the manager.
     * @param[in] what   Human readable reason (logged / visible in what(), not sent over SNMP).
     * @param[in] index  0-based position of the offending varbind within the request handed to the
     *                   handler, or kUnknownIndex.
     */
    explicit SetError(ErrorStatus status, const std::string& what = "SNMP SET rejected",
                      std::size_t index = kUnknownIndex)
        : Error(what), status_(status), index_(index) {}

    /**
     * @brief Error-status that will be returned to the manager.
     * @return The status passed to the constructor.
     */
    ErrorStatus status() const noexcept { return status_; }

    /**
     * @brief Position of the offending varbind.
     * @return 0-based index, or kUnknownIndex.
     */
    std::size_t index() const noexcept { return index_; }

private:
    ErrorStatus status_;
    std::size_t index_;
};

/**
 * @brief The remote agent answered a Client request with an error-status other than noError.
 *
 * Example: a SET with an out-of-range value yields status() == ErrorStatus::WrongValue (v2c/v3)
 * or ErrorStatus::BadValue (v1).
 */
class ResponseError : public Error {
public:
    /**
     * @brief Creates the exception.
     * @param[in] status Error-status from the response PDU.
     * @param[in] index  Error-index from the response PDU (1-based, 0 = unknown).
     * @param[in] what   Human readable description.
     */
    ResponseError(ErrorStatus status, int index, const std::string& what)
        : Error(what), status_(status), index_(index) {}

    /**
     * @brief Error-status reported by the agent.
     * @return The error-status of the response.
     */
    ErrorStatus status() const noexcept { return status_; }

    /**
     * @brief Which varbind caused the error.
     * @return 1-based index of the offending varbind in the request, 0 if unknown.
     */
    int index() const noexcept { return index_; }

private:
    ErrorStatus status_;
    int index_;
};

/**
 * @brief A Client request could not be completed at all.
 *
 * Typical causes: timeout (no agent, wrong port, wrong community – agents silently drop those),
 * network errors, SNMPv3 authentication / privacy failures, unknown SNMPv3 user.
 */
class TransportError : public Error {
public:
    using Error::Error;
};

}  // namespace snmpwrap
