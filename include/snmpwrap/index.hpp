/**
 * @file index.hpp
 * @brief Encoding and decoding of table row indexes (RFC 2578 section 7.7).
 *
 * The row index is the part of a table cell OID after the column number:
 * `<table>.1.<column>.<index...>`. For a table with `INDEX { ifIndex }` it is a single number,
 * for `INDEX { addr, port, IMPLIED tag }` it is 4 octets, a number and the tag bytes.
 *
 * @code
 * const std::vector<IndexSpec> spec{IndexSpec::ipAddress(), IndexSpec::integer(), IndexSpec::impliedString()};
 * Oid idx = encodeIndex(spec, {Value::ipAddress(10, 0, 0, 1), Value::integer(80), Value::string("web")});
 * // idx == 10.0.0.1.80.119.101.98
 * auto values = decodeIndex(spec, idx);   // std::optional<std::vector<Value>>, nullopt if malformed
 * @endcode
 */
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "snmpwrap/oid.hpp"
#include "snmpwrap/value.hpp"

namespace snmpwrap {

/**
 * @brief How one INDEX column is encoded into the row index.
 */
enum class IndexKind {
    Integer,        ///< INTEGER / Integer32: one sub-id; Value::integer.
    Unsigned,       ///< Unsigned32 / Gauge32: one sub-id; Value::gauge.
    String,         ///< OCTET STRING: length + one sub-id per byte; Value::string.
    ImpliedString,  ///< IMPLIED OCTET STRING (last column only): bytes without length prefix.
    FixedString,    ///< OCTET STRING (SIZE (n)): exactly n bytes, no length prefix; see IndexSpec::size.
    IpAddress,      ///< IpAddress: 4 sub-ids; Value::ipAddress.
    ObjectId,       ///< OBJECT IDENTIFIER: length + sub-ids; Value::oid.
    ImpliedObjectId,  ///< IMPLIED OBJECT IDENTIFIER (last column only): sub-ids without length prefix.
};

/**
 * @brief Description of one INDEX column; use the static factories.
 */
struct IndexSpec {
    IndexKind kind = IndexKind::Integer;  ///< Encoding of the column.
    std::size_t size = 0;                 ///< Number of octets, only for IndexKind::FixedString.

    /// @brief INTEGER / Integer32 column. @return The spec.
    static IndexSpec integer() { return {IndexKind::Integer, 0}; }
    /// @brief Unsigned32 / Gauge32 column. @return The spec.
    static IndexSpec unsignedInt() { return {IndexKind::Unsigned, 0}; }
    /// @brief Variable-length OCTET STRING column. @return The spec.
    static IndexSpec string() { return {IndexKind::String, 0}; }
    /// @brief IMPLIED OCTET STRING column (must be the last index column). @return The spec.
    static IndexSpec impliedString() { return {IndexKind::ImpliedString, 0}; }
    /**
     * @brief Fixed-size OCTET STRING column.
     * @param[in] n Number of octets.
     * @return The spec.
     */
    static IndexSpec fixedString(std::size_t n) { return {IndexKind::FixedString, n}; }
    /// @brief IpAddress column. @return The spec.
    static IndexSpec ipAddress() { return {IndexKind::IpAddress, 0}; }
    /// @brief OBJECT IDENTIFIER column. @return The spec.
    static IndexSpec objectId() { return {IndexKind::ObjectId, 0}; }
    /// @brief IMPLIED OBJECT IDENTIFIER column (must be the last index column). @return The spec.
    static IndexSpec impliedObjectId() { return {IndexKind::ImpliedObjectId, 0}; }
};

/**
 * @brief Builds the row index from index column values.
 * @param[in] specs  Layout of the index, one entry per INDEX column, in MIB order.
 * @param[in] values One value per spec, with the matching type (see IndexKind).
 * @return The row-index OID to append after `<table>.1.<column>`.
 * @throws Error on a count or type mismatch, a wrong FixedString length, or IMPLIED not being last.
 */
Oid encodeIndex(const std::vector<IndexSpec>& specs, const std::vector<Value>& values);

/**
 * @brief Splits a row index into its column values (strict inverse of encodeIndex).
 * @param[in] specs Layout of the index.
 * @param[in] index Row-index OID as found in a request.
 * @return The values, or std::nullopt unless @p index is a complete, well-formed encoding
 *         (correct length, no trailing sub-ids, string octets <= 255, IP octets <= 255).
 * @throws Error if @p specs itself is invalid (IMPLIED column not last).
 */
std::optional<std::vector<Value>> decodeIndex(const std::vector<IndexSpec>& specs, const Oid& index);

}  // namespace snmpwrap
