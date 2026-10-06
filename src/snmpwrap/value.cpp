#include "snmpwrap/value.hpp"

#include <cctype>
#include <cstdio>

#include "snmpwrap/error.hpp"

namespace snmpwrap {

const char* toString(Type t) noexcept {
    switch (t) {
        case Type::Null: return "Null";
        case Type::Integer: return "Integer";
        case Type::OctetString: return "OctetString";
        case Type::ObjectId: return "ObjectId";
        case Type::IpAddress: return "IpAddress";
        case Type::Counter32: return "Counter32";
        case Type::Gauge32: return "Gauge32";
        case Type::TimeTicks: return "TimeTicks";
        case Type::Counter64: return "Counter64";
        case Type::Opaque: return "Opaque";
        case Type::Bits: return "Bits";
        case Type::NoSuchObject: return "NoSuchObject";
        case Type::NoSuchInstance: return "NoSuchInstance";
        case Type::EndOfMibView: return "EndOfMibView";
    }
    return "?";
}

namespace {
[[noreturn]] void mismatch(Type have, const char* want) {
    throw Error(std::string("value is ") + toString(have) + ", not " + want);
}
}  // namespace

std::int32_t Value::asInt() const {
    if (type_ != Type::Integer) mismatch(type_, "Integer");
    return std::get<std::int32_t>(data_);
}

std::uint32_t Value::asUInt() const {
    switch (type_) {
        case Type::Gauge32:
        case Type::Counter32:
        case Type::TimeTicks:
            return std::get<std::uint32_t>(data_);
        case Type::Integer:
            if (std::get<std::int32_t>(data_) >= 0) return static_cast<std::uint32_t>(std::get<std::int32_t>(data_));
            throw Error("negative Integer cannot be read as unsigned");
        default:
            mismatch(type_, "an unsigned type");
    }
}

std::uint64_t Value::asUInt64() const {
    if (type_ == Type::Counter64) return std::get<std::uint64_t>(data_);
    return asUInt();
}

const std::string& Value::asString() const {
    if (type_ != Type::OctetString && type_ != Type::Opaque && type_ != Type::Bits)
        mismatch(type_, "OctetString, Opaque or Bits");
    return std::get<std::string>(data_);
}

const Oid& Value::asOid() const {
    if (type_ != Type::ObjectId) mismatch(type_, "ObjectId");
    return std::get<Oid>(data_);
}

std::array<std::uint8_t, 4> Value::asIp() const {
    if (type_ != Type::IpAddress) mismatch(type_, "IpAddress");
    return std::get<std::array<std::uint8_t, 4>>(data_);
}

std::string Value::str() const {
    switch (type_) {
        case Type::Integer: return "Integer: " + std::to_string(asInt());
        case Type::Gauge32:
        case Type::Counter32:
        case Type::TimeTicks: return std::string(toString(type_)) + ": " + std::to_string(asUInt());
        case Type::Counter64: return "Counter64: " + std::to_string(asUInt64());
        case Type::ObjectId: return "ObjectId: " + asOid().str();
        case Type::IpAddress: {
            auto ip = asIp();
            return "IpAddress: " + std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) +
                   "." + std::to_string(ip[3]);
        }
        case Type::OctetString:
        case Type::Opaque:
        case Type::Bits: {
            const std::string& s = asString();
            const std::string name = toString(type_);
            bool printable = type_ == Type::OctetString;
            for (unsigned char c : s) printable = printable && std::isprint(c);
            if (printable) return name + ": \"" + s + "\"";
            std::string out = name + " (hex):";
            char buf[4];
            for (unsigned char c : s) {
                std::snprintf(buf, sizeof buf, " %02X", c);
                out += buf;
            }
            return out;
        }
        default: return toString(type_);
    }
}

}  // namespace snmpwrap
