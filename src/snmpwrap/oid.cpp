#include "snmpwrap/oid.hpp"

#include <algorithm>
#include <limits>

#include "snmpwrap/error.hpp"

namespace snmpwrap {

Oid Oid::parse(std::string_view s) {
    if (!s.empty() && s.front() == '.') s.remove_prefix(1);
    if (s.empty()) throw Error("empty OID");

    std::vector<SubId> ids;
    std::uint64_t cur = 0;
    bool haveDigit = false;
    for (char ch : s) {
        if (ch >= '0' && ch <= '9') {
            cur = cur * 10 + static_cast<unsigned>(ch - '0');
            if (cur > std::numeric_limits<SubId>::max()) throw Error("OID sub-identifier too large: " + std::string(s));
            haveDigit = true;
        } else if (ch == '.') {
            if (!haveDigit) throw Error("malformed OID: " + std::string(s));
            ids.push_back(static_cast<SubId>(cur));
            cur = 0;
            haveDigit = false;
        } else {
            throw Error("malformed OID: " + std::string(s));
        }
    }
    if (!haveDigit) throw Error("malformed OID: " + std::string(s));
    ids.push_back(static_cast<SubId>(cur));
    return Oid(std::move(ids));
}

std::string Oid::str() const {
    std::string out;
    for (std::size_t i = 0; i < ids_.size(); ++i) {
        if (i) out += '.';
        out += std::to_string(ids_[i]);
    }
    return out;
}

bool Oid::isPrefixOf(const Oid& other) const noexcept {
    return ids_.size() <= other.ids_.size() && std::equal(ids_.begin(), ids_.end(), other.ids_.begin());
}

Oid Oid::suffixOf(const Oid& other) const {
    return Oid(std::vector<SubId>(other.ids_.begin() + static_cast<std::ptrdiff_t>(ids_.size()), other.ids_.end()));
}

Oid indexInt(std::uint32_t v) { return Oid{v}; }

Oid indexString(std::string_view s, bool implied) {
    Oid o;
    if (!implied) o.append(static_cast<SubId>(s.size()));
    for (unsigned char c : s) o.append(c);
    return o;
}

Oid indexIp(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) { return Oid{a, b, c, d}; }

}  // namespace snmpwrap
