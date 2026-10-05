/**
 * @file value.hpp
 * @brief Typed SNMP values (Integer32, OCTET STRING, Counter32, ...) and varbinds.
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <variant>

#include "snmpwrap/oid.hpp"

namespace snmpwrap {

/**
 * @brief SNMP value types (SMIv2 base and application types).
 *
 * The type declared for an object (ScalarDef::type, Column::type) must match the type of the
 * Value your getter returns, and SET requests with a different type are rejected with WrongType.
 */
enum class Type {
    Null,           ///< NULL (no value).
    Integer,        ///< Integer32 / INTEGER / enumerations / RowStatus / TruthValue.
    OctetString,    ///< OCTET STRING / DisplayString / binary data.
    ObjectId,       ///< OBJECT IDENTIFIER.
    IpAddress,      ///< IpAddress (4 octets).
    Counter32,      ///< Counter32 (wraps around, read-only by convention).
    Gauge32,        ///< Gauge32 / Unsigned32.
    TimeTicks,      ///< TimeTicks (hundredths of a second).
    Counter64,      ///< Counter64 (not visible to SNMPv1 managers).
    Opaque,         ///< Opaque (arbitrary bytes wrapped in the Opaque ASN.1 type).
    Bits,           ///< BITS (bit string, MSB of the first octet = bit 0).
    NoSuchObject,   ///< Exception marker (Client only): no such object.
    NoSuchInstance, ///< Exception marker (Client only): object exists, instance does not.
    EndOfMibView,   ///< Exception marker (Client only): nothing follows (GETNEXT / GETBULK).
};

/**
 * @brief Name of a type.
 * @param[in] t The type.
 * @return A static string such as "Gauge32".
 */
const char* toString(Type t) noexcept;

/**
 * @brief A typed SNMP value.
 *
 * Create values with the static factory functions and read them with the matching accessor.
 * Accessors throw Error if the stored type does not fit, so mistakes surface early.
 *
 * @code
 * Value a = Value::integer(42);
 * Value b = Value::string("eth0");
 * Value c = Value::gauge(1000);
 * int n = a.asInt();                 // 42
 * std::string s = b.asString();      // "eth0"
 * std::cout << c.str();              // "Gauge32: 1000"
 * @endcode
 */
class Value {
public:
    /// @brief Creates a Null value.
    Value() = default;

    /// @name Factories
    /// @{

    /// @brief NULL value. @return A value of Type::Null.
    static Value null() { return Value(); }
    /**
     * @brief Integer32 / INTEGER (also enumerations and RowStatus).
     * @param[in] v The number.
     * @return A value of Type::Integer.
     */
    static Value integer(std::int32_t v) { return Value(Type::Integer, v); }
    /**
     * @brief Gauge32 / Unsigned32.
     * @param[in] v The number.
     * @return A value of Type::Gauge32.
     */
    static Value gauge(std::uint32_t v) { return Value(Type::Gauge32, v); }
    /**
     * @brief Counter32.
     * @param[in] v The counter value.
     * @return A value of Type::Counter32.
     */
    static Value counter32(std::uint32_t v) { return Value(Type::Counter32, v); }
    /**
     * @brief TimeTicks.
     * @param[in] v Hundredths of a second.
     * @return A value of Type::TimeTicks.
     */
    static Value timeTicks(std::uint32_t v) { return Value(Type::TimeTicks, v); }
    /**
     * @brief Counter64.
     * @param[in] v The counter value.
     * @return A value of Type::Counter64.
     */
    static Value counter64(std::uint64_t v) { return Value(Type::Counter64, v); }
    /**
     * @brief OCTET STRING (text or binary; any byte values allowed).
     * @param[in] v The bytes.
     * @return A value of Type::OctetString.
     */
    static Value string(std::string v) { return Value(Type::OctetString, std::move(v)); }
    /**
     * @brief Opaque (binary data keeping the Opaque ASN.1 type).
     * @param[in] v The bytes.
     * @return A value of Type::Opaque.
     */
    static Value opaque(std::string v) { return Value(Type::Opaque, std::move(v)); }
    /**
     * @brief BITS (bit string; bit 0 is the most significant bit of the first octet).
     * @param[in] v The octets.
     * @return A value of Type::Bits.
     */
    static Value bits(std::string v) { return Value(Type::Bits, std::move(v)); }
    /**
     * @brief OBJECT IDENTIFIER.
     * @param[in] v The OID.
     * @return A value of Type::ObjectId.
     */
    static Value oid(Oid v) { return Value(Type::ObjectId, std::move(v)); }
    /**
     * @brief IpAddress a.b.c.d.
     * @param[in] a First octet.
     * @param[in] b Second octet.
     * @param[in] c Third octet.
     * @param[in] d Fourth octet.
     * @return A value of Type::IpAddress.
     */
    static Value ipAddress(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
        return Value(Type::IpAddress, std::array<std::uint8_t, 4>{a, b, c, d});
    }
    /**
     * @brief Exception marker (noSuchObject, noSuchInstance, endOfMibView); normally only produced by Client.
     * @param[in] t One of Type::NoSuchObject, Type::NoSuchInstance, Type::EndOfMibView.
     * @return A data-less value of that type.
     */
    static Value exception(Type t) { Value v; v.type_ = t; return v; }
    /// @}

    /// @brief The stored type. @return The type of this value.
    Type type() const noexcept { return type_; }

    /**
     * @brief Checks for an exception marker.
     * @return True for NoSuchObject, NoSuchInstance and EndOfMibView.
     */
    bool isException() const noexcept {
        return type_ == Type::NoSuchObject || type_ == Type::NoSuchInstance || type_ == Type::EndOfMibView;
    }

    /// @name Accessors (throw Error on a type mismatch)
    /// @{

    /**
     * @brief Reads an Integer.
     * @return The number.
     * @throws Error if the type is not Type::Integer.
     */
    std::int32_t asInt() const;
    /**
     * @brief Reads Gauge32, Counter32, TimeTicks or a non-negative Integer.
     * @return The number.
     * @throws Error for other types or a negative Integer.
     */
    std::uint32_t asUInt() const;
    /**
     * @brief Reads Counter64 or anything asUInt() accepts.
     * @return The number.
     * @throws Error for other types.
     */
    std::uint64_t asUInt64() const;
    /**
     * @brief Reads an OCTET STRING, Opaque or BITS value.
     * @return Reference to the stored bytes.
     * @throws Error if the type is not Type::OctetString, Type::Opaque or Type::Bits.
     */
    const std::string& asString() const;
    /**
     * @brief Reads an OBJECT IDENTIFIER.
     * @return Reference to the stored OID.
     * @throws Error if the type is not Type::ObjectId.
     */
    const Oid& asOid() const;
    /**
     * @brief Reads an IpAddress.
     * @return The four octets.
     * @throws Error if the type is not Type::IpAddress.
     */
    std::array<std::uint8_t, 4> asIp() const;
    /// @}

    /**
     * @brief Human readable representation for logs and debugging.
     * @return Text like "Gauge32: 42" or "OctetString: \"abc\"" (hex for binary strings).
     */
    std::string str() const;

    /// @brief Equal if type and data are equal (Integer 1 != Gauge32 1).
    friend bool operator==(const Value& a, const Value& b) { return a.type_ == b.type_ && a.data_ == b.data_; }
    /// @brief Negation of operator==.
    friend bool operator!=(const Value& a, const Value& b) { return !(a == b); }

private:
    using Data = std::variant<std::monostate, std::int32_t, std::uint32_t, std::uint64_t, std::string, Oid,
                              std::array<std::uint8_t, 4>>;
    template <class T>
    Value(Type t, T v) : type_(t), data_(std::move(v)) {}

    Type type_ = Type::Null;
    Data data_;
};

/**
 * @brief A variable binding: an instance OID together with its value.
 */
struct VarBind {
    Oid oid;      ///< Instance OID, e.g. 1.3.6.1.4.1.99999.1.4.0.
    Value value;  ///< The value (or an exception marker in Client results).
};

}  // namespace snmpwrap
