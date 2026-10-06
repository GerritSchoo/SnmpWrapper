#include "snmpwrap/index.hpp"

#include "snmpwrap/error.hpp"

namespace snmpwrap {

namespace {

void checkSpecs(const std::vector<IndexSpec>& specs) {
    for (std::size_t i = 0; i < specs.size(); ++i)
        if ((specs[i].kind == IndexKind::ImpliedString || specs[i].kind == IndexKind::ImpliedObjectId) &&
            i + 1 != specs.size())
            throw Error("IMPLIED index column must be the last one");
}

}  // namespace

Oid encodeIndex(const std::vector<IndexSpec>& specs, const std::vector<Value>& values) {
    checkSpecs(specs);
    if (specs.size() != values.size()) throw Error("index: expected " + std::to_string(specs.size()) + " values");
    Oid out;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const IndexSpec& s = specs[i];
        const Value& v = values[i];
        switch (s.kind) {
            case IndexKind::Integer:
                out.append(static_cast<SubId>(static_cast<std::uint32_t>(v.asInt())));
                break;
            case IndexKind::Unsigned:
                if (v.type() != Type::Gauge32 && v.type() != Type::Counter32 && v.type() != Type::TimeTicks)
                    throw Error("index: Unsigned expects a Gauge32, Counter32 or TimeTicks value");
                out.append(v.asUInt());
                break;
            case IndexKind::String:
                out.append(indexString(v.asString(), false));
                break;
            case IndexKind::ImpliedString:
                out.append(indexString(v.asString(), true));
                break;
            case IndexKind::FixedString:
                if (v.asString().size() != s.size) throw Error("index: fixed string must have " + std::to_string(s.size) + " octets");
                out.append(indexString(v.asString(), true));
                break;
            case IndexKind::IpAddress: {
                auto ip = v.asIp();
                out.append(indexIp(ip[0], ip[1], ip[2], ip[3]));
                break;
            }
            case IndexKind::ObjectId:
                out.append(static_cast<SubId>(v.asOid().size()));
                out.append(v.asOid());
                break;
            case IndexKind::ImpliedObjectId:
                out.append(v.asOid());
                break;
        }
    }
    return out;
}

std::optional<std::vector<Value>> decodeIndex(const std::vector<IndexSpec>& specs, const Oid& index) {
    checkSpecs(specs);
    std::vector<Value> out;
    std::size_t pos = 0;
    const auto& ids = index.ids();

    auto takeString = [&](std::size_t n) -> std::optional<std::string> {
        if (ids.size() - pos < n) return std::nullopt;
        std::string s;
        for (std::size_t k = 0; k < n; ++k) {
            if (ids[pos + k] > 255) return std::nullopt;
            s.push_back(static_cast<char>(ids[pos + k]));
        }
        pos += n;
        return s;
    };

    for (const IndexSpec& s : specs) {
        switch (s.kind) {
            case IndexKind::Integer:
                if (pos >= ids.size()) return std::nullopt;
                out.push_back(Value::integer(static_cast<std::int32_t>(ids[pos++])));
                break;
            case IndexKind::Unsigned:
                if (pos >= ids.size()) return std::nullopt;
                out.push_back(Value::gauge(ids[pos++]));
                break;
            case IndexKind::String: {
                if (pos >= ids.size()) return std::nullopt;
                const std::size_t len = ids[pos++];
                auto str = takeString(len);
                if (!str) return std::nullopt;
                out.push_back(Value::string(std::move(*str)));
                break;
            }
            case IndexKind::ImpliedString: {
                auto str = takeString(ids.size() - pos);
                if (!str) return std::nullopt;
                out.push_back(Value::string(std::move(*str)));
                break;
            }
            case IndexKind::FixedString: {
                auto str = takeString(s.size);
                if (!str) return std::nullopt;
                out.push_back(Value::string(std::move(*str)));
                break;
            }
            case IndexKind::IpAddress: {
                if (ids.size() - pos < 4) return std::nullopt;
                for (int k = 0; k < 4; ++k)
                    if (ids[pos + k] > 255) return std::nullopt;
                out.push_back(Value::ipAddress(static_cast<std::uint8_t>(ids[pos]), static_cast<std::uint8_t>(ids[pos + 1]),
                                               static_cast<std::uint8_t>(ids[pos + 2]), static_cast<std::uint8_t>(ids[pos + 3])));
                pos += 4;
                break;
            }
            case IndexKind::ObjectId: {
                if (pos >= ids.size()) return std::nullopt;
                const std::size_t len = ids[pos++];
                if (ids.size() - pos < len) return std::nullopt;
                out.push_back(Value::oid(Oid(std::vector<SubId>(ids.begin() + static_cast<std::ptrdiff_t>(pos),
                                                                ids.begin() + static_cast<std::ptrdiff_t>(pos + len)))));
                pos += len;
                break;
            }
            case IndexKind::ImpliedObjectId:
                out.push_back(Value::oid(Oid(std::vector<SubId>(ids.begin() + static_cast<std::ptrdiff_t>(pos), ids.end()))));
                pos = ids.size();
                break;
        }
    }
    if (pos != ids.size()) return std::nullopt;  // trailing sub-ids
    return out;
}

}  // namespace snmpwrap
