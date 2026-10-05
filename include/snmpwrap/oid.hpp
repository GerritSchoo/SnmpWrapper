/**
 * @file oid.hpp
 * @brief Object identifiers (OIDs) and helpers to build table row indexes.
 */
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace snmpwrap {

/**
 * @brief One sub-identifier of an OID.
 *
 * SNMP defines sub-identifiers as 32-bit unsigned numbers, independent of Net-SNMP's
 * platform-dependent `oid` typedef (which is 64 bit on most Linux systems).
 */
using SubId = std::uint32_t;

/**
 * @brief An SNMP object identifier such as 1.3.6.1.4.1.99999.1.1.0.
 *
 * Oids are cheap value types. Comparison operators implement the lexicographic order of
 * sub-identifiers, which is exactly the order SNMP GETNEXT / walks use
 * (e.g. 1.3.6.1.2 < 1.3.6.1.10, and 1.3.6 < 1.3.6.0).
 *
 * @code
 * Oid root = Oid::parse("1.3.6.1.4.1.99999");
 * Oid inst = root + Oid{1, 4, 0};          // 1.3.6.1.4.1.99999.1.4.0
 * bool inside = root.isPrefixOf(inst);     // true
 * Oid rel = root.suffixOf(inst);           // 1.4.0
 * @endcode
 */
class Oid {
public:
    /// @brief Creates an empty OID.
    Oid() = default;

    /**
     * @brief Creates an OID from a list of sub-identifiers.
     * @param[in] ids Sub-identifiers, e.g. `Oid{1, 3, 6, 1}`.
     */
    Oid(std::initializer_list<SubId> ids) : ids_(ids) {}

    /**
     * @brief Creates an OID from a vector of sub-identifiers.
     * @param[in] ids Sub-identifiers (moved into the OID).
     */
    explicit Oid(std::vector<SubId> ids) : ids_(std::move(ids)) {}

    /**
     * @brief Parses the dotted numeric notation.
     * @param[in] dotted Text like "1.3.6.1.4.1.99999"; a leading dot is allowed. Symbolic MIB names
     *                   are not supported (snmpwrap never reads MIB files).
     * @return The parsed OID.
     * @throws Error if the text is empty, malformed or a sub-identifier exceeds 2^32-1.
     */
    static Oid parse(std::string_view dotted);

    /**
     * @brief Dotted numeric representation.
     * @return Text like "1.3.6.1.4.1.99999" (without leading dot).
     */
    std::string str() const;

    /// @brief Access to the raw sub-identifiers. @return The sub-identifier vector.
    const std::vector<SubId>& ids() const noexcept { return ids_; }
    /// @brief Number of sub-identifiers. @return The length of the OID.
    std::size_t size() const noexcept { return ids_.size(); }
    /// @brief Checks for the empty OID. @return True if the OID has no sub-identifiers.
    bool empty() const noexcept { return ids_.empty(); }
    /**
     * @brief Sub-identifier at position @p i (no bounds check).
     * @param[in] i 0-based position.
     * @return The sub-identifier.
     */
    SubId operator[](std::size_t i) const { return ids_[i]; }

    /**
     * @brief Appends one sub-identifier in place.
     * @param[in] id Sub-identifier to append.
     * @return Reference to this OID.
     */
    Oid& append(SubId id) { ids_.push_back(id); return *this; }

    /**
     * @brief Appends all sub-identifiers of another OID in place.
     * @param[in] other OID to append.
     * @return Reference to this OID.
     */
    Oid& append(const Oid& other) { ids_.insert(ids_.end(), other.ids_.begin(), other.ids_.end()); return *this; }

    /**
     * @brief Returns a copy with one sub-identifier appended.
     * @param[in] id Sub-identifier to append.
     * @return The new OID.
     */
    Oid operator+(SubId id) const { Oid r = *this; r.append(id); return r; }

    /**
     * @brief Returns the concatenation of two OIDs.
     * @param[in] other OID to append.
     * @return The new OID.
     */
    Oid operator+(const Oid& other) const { Oid r = *this; r.append(other); return r; }

    /**
     * @brief Checks whether this OID is a prefix of another one.
     * @param[in] other OID to test.
     * @return True if `other` starts with all sub-identifiers of this OID (also true if both are equal).
     */
    bool isPrefixOf(const Oid& other) const noexcept;

    /**
     * @brief The part of @p other that follows this OID.
     * @param[in] other An OID that starts with this OID (precondition: isPrefixOf(other)).
     * @return The remaining sub-identifiers, e.g. root 1.3 and other 1.3.6.1 -> 6.1.
     */
    Oid suffixOf(const Oid& other) const;

    /// @name Comparison (lexicographic by sub-identifier = SNMP order)
    /// @{
    friend bool operator==(const Oid& a, const Oid& b) noexcept { return a.ids_ == b.ids_; }
    friend bool operator!=(const Oid& a, const Oid& b) noexcept { return a.ids_ != b.ids_; }
    friend bool operator<(const Oid& a, const Oid& b) noexcept { return a.ids_ < b.ids_; }
    friend bool operator>(const Oid& a, const Oid& b) noexcept { return b.ids_ < a.ids_; }
    friend bool operator<=(const Oid& a, const Oid& b) noexcept { return !(b < a); }
    friend bool operator>=(const Oid& a, const Oid& b) noexcept { return !(a < b); }
    /// @}

private:
    std::vector<SubId> ids_;
};

/// @name Row-index shortcuts (RFC 2578 section 7.7)
/// For multi-column or strictly validated indexes see index.hpp (encodeIndex / decodeIndex).
/// @{

/**
 * @brief Index part for an INTEGER / Unsigned32 index column.
 * @param[in] v Index value.
 * @return One sub-identifier, e.g. indexInt(5) -> 5.
 */
Oid indexInt(std::uint32_t v);

/**
 * @brief Index part for an OCTET STRING index column.
 * @param[in] s       The string; every byte becomes one sub-identifier.
 * @param[in] implied True for the last index column declared `IMPLIED` in the MIB (no length prefix).
 * @return E.g. indexString("ab") -> 2.97.98, indexString("ab", true) -> 97.98.
 */
Oid indexString(std::string_view s, bool implied = false);

/**
 * @brief Index part for an IpAddress index column.
 * @param[in] a First octet.
 * @param[in] b Second octet.
 * @param[in] c Third octet.
 * @param[in] d Fourth octet.
 * @return Four sub-identifiers, e.g. indexIp(10, 0, 0, 1) -> 10.0.0.1.
 */
Oid indexIp(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d);

/// @}

}  // namespace snmpwrap
