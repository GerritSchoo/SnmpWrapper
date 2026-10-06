#include "snmpwrap/mib_model.hpp"

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/library/parse.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>

#include "snmpwrap/error.hpp"

namespace snmpwrap {

struct MibModel::Data {
    std::vector<std::string> modules;
    std::vector<MibNode> nodes;                                // sorted by OID
    std::map<std::string, std::size_t> byQualified;            // "MODULE::name"
    std::map<std::string, std::vector<std::size_t>> byName;    // "name"
    std::map<Oid, std::size_t> byOid;
};

const char* toString(MibAccess a) noexcept {
    switch (a) {
        case MibAccess::NotAccessible: return "not-accessible";
        case MibAccess::AccessibleForNotify: return "accessible-for-notify";
        case MibAccess::ReadOnly: return "read-only";
        case MibAccess::ReadWrite: return "read-write";
        case MibAccess::ReadCreate: return "read-create";
    }
    return "?";
}

const char* toString(MibNodeKind k) noexcept {
    switch (k) {
        case MibNodeKind::Other: return "Other";
        case MibNodeKind::Scalar: return "Scalar";
        case MibNodeKind::Table: return "Table";
        case MibNodeKind::Entry: return "Entry";
        case MibNodeKind::Column: return "Column";
        case MibNodeKind::Notification: return "Notification";
    }
    return "?";
}

namespace {

std::mutex g_loadMutex;  // Net-SNMP's MIB tree is process-wide

// --- collecting Net-SNMP's diagnostics while parsing --------------------------------------------

struct Capture {
    std::vector<std::string> errors;
};

int captureLog(int, int, void* serverarg, void* clientarg) {
    const auto* msg = static_cast<const snmp_log_message*>(serverarg);
    auto* cap = static_cast<Capture*>(clientarg);
    if (msg && msg->msg && msg->priority <= LOG_ERR) {
        std::string text(msg->msg);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        if (!text.empty()) cap->errors.push_back(std::move(text));
    }
    return 0;
}

/// Routes Net-SNMP log output into a Capture for its lifetime (RAII).
class LogCapture {
public:
    explicit LogCapture(Capture& cap) : cap_(cap) {
        oldMibErrors_ = netsnmp_ds_get_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_MIB_ERRORS);
        netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_MIB_ERRORS, 1);
        handler_ = netsnmp_register_loghandler(NETSNMP_LOGHANDLER_CALLBACK, LOG_WARNING);
        snmp_register_callback(SNMP_CALLBACK_LIBRARY, SNMP_CALLBACK_LOGGING, captureLog, &cap_);
    }
    ~LogCapture() {
        snmp_unregister_callback(SNMP_CALLBACK_LIBRARY, SNMP_CALLBACK_LOGGING, captureLog, &cap_, 1);
        if (handler_) netsnmp_remove_loghandler(handler_);
        netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_MIB_ERRORS, oldMibErrors_);
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

private:
    Capture& cap_;
    netsnmp_log_handler* handler_ = nullptr;
    int oldMibErrors_ = 0;
};

/// First identifier of a MIB file = module name (what Net-SNMP's read_mib() does as well).
std::string scanModuleName(const std::string& file) {
    std::ifstream in(file);
    if (!in) throw Error("cannot open MIB file " + file);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t i = 0;
    while (i < content.size()) {
        if (std::isspace(static_cast<unsigned char>(content[i]))) {
            ++i;
        } else if (content.compare(i, 2, "--") == 0) {  // comment up to end of line or next "--"
            i += 2;
            while (i < content.size() && content[i] != '\n' && content.compare(i, 2, "--") != 0) ++i;
            if (i < content.size() && content[i] != '\n') i += 2;
        } else {
            break;
        }
    }
    std::size_t start = i;
    while (i < content.size() && (std::isalnum(static_cast<unsigned char>(content[i])) || content[i] == '-' || content[i] == '_'))
        ++i;
    if (i == start || !std::isalpha(static_cast<unsigned char>(content[start])))
        throw Error(file + " does not look like a MIB module (no module name found)");
    return content.substr(start, i - start);
}

// --- tree -> C++ ------------------------------------------------------------------------------

bool isEntry(const struct tree* tp) { return tp && (tp->indexes || tp->augments); }

bool isSimpleType(int t) { return t >= TYPE_OBJID && t <= TYPE_SIMPLE_LAST && t != TYPE_NULL; }

Type mapType(int t) {
    switch (t) {
        case TYPE_OBJID: return Type::ObjectId;
        case TYPE_OCTETSTR:
        case TYPE_NSAPADDRESS: return Type::OctetString;
        case TYPE_OPAQUE: return Type::Opaque;
        case TYPE_BITSTRING: return Type::Bits;
        case TYPE_INTEGER:
        case TYPE_INTEGER32: return Type::Integer;
        case TYPE_NETADDR:
        case TYPE_IPADDR: return Type::IpAddress;
        case TYPE_COUNTER: return Type::Counter32;
        case TYPE_GAUGE:
        case TYPE_UNSIGNED32:
        case TYPE_UINTEGER: return Type::Gauge32;
        case TYPE_TIMETICKS: return Type::TimeTicks;
        case TYPE_COUNTER64: return Type::Counter64;
        default: return Type::Null;
    }
}

MibAccess mapAccess(int a) {
    switch (a) {
        case MIB_ACCESS_READONLY: return MibAccess::ReadOnly;
        case MIB_ACCESS_READWRITE: return MibAccess::ReadWrite;
        case MIB_ACCESS_CREATE: return MibAccess::ReadCreate;
        case MIB_ACCESS_NOTIFY: return MibAccess::AccessibleForNotify;
        default: return MibAccess::NotAccessible;
    }
}

bool isUnsigned(Type t) {
    return t == Type::Gauge32 || t == Type::Counter32 || t == Type::TimeTicks || t == Type::Counter64;
}

std::string str(const char* s) { return s ? std::string(s) : std::string(); }

MibNode convert(const struct tree* tp, const struct tree* parent, const Oid& oid) {
    MibNode n;
    n.name = str(tp->label);
    char buf[SPRINT_MAX_LEN];
    buf[0] = '\0';
    module_name(tp->modid, buf);
    n.module = buf;
    n.oid = oid;

    bool hasEntryChild = false;
    for (const struct tree* c = tp->child_list; c; c = c->next_peer) hasEntryChild = hasEntryChild || isEntry(c);

    if (tp->type == TYPE_NOTIFTYPE || tp->type == TYPE_TRAPTYPE)
        n.kind = MibNodeKind::Notification;
    else if (isEntry(tp))
        n.kind = MibNodeKind::Entry;
    else if (hasEntryChild)
        n.kind = MibNodeKind::Table;
    else if (isEntry(parent) && isSimpleType(tp->type))
        n.kind = MibNodeKind::Column;
    else if (isSimpleType(tp->type) && tp->access != 0)
        n.kind = MibNodeKind::Scalar;
    else
        n.kind = MibNodeKind::Other;

    if (n.kind == MibNodeKind::Scalar || n.kind == MibNodeKind::Column) n.type = mapType(tp->type);
    n.access = mapAccess(tp->access);
    if (tp->tc_index >= 0) n.textualConvention = str(get_tc_descriptor(tp->tc_index));

    const bool uns = isUnsigned(n.type);
    for (const struct range_list* r = tp->ranges; r; r = r->next) {
        MibRange mr;
        mr.low = uns ? static_cast<std::int64_t>(static_cast<std::uint32_t>(r->low)) : r->low;
        mr.high = uns ? static_cast<std::int64_t>(static_cast<std::uint32_t>(r->high)) : r->high;
        n.ranges.push_back(mr);
    }
    for (const struct enum_list* e = tp->enums; e; e = e->next) n.enums.push_back({e->value, str(e->label)});
    std::sort(n.enums.begin(), n.enums.end(), [](const MibEnum& a, const MibEnum& b) { return a.value < b.value; });
    for (const struct index_list* i = tp->indexes; i; i = i->next) n.index.push_back({str(i->ilabel), i->isimplied != 0});
    n.augments = str(tp->augments);
    for (const struct varbind_list* v = tp->varbinds; v; v = v->next) n.objects.push_back(str(v->vblabel));
    n.displayHint = str(tp->hint);
    n.units = str(tp->units);
    n.description = str(tp->description);
    n.defaultValue = str(tp->defaultValue);
    return n;
}


std::string joinErrors(const std::vector<std::string>& errors) {
    std::string out;
    for (const auto& e : errors) {
        if (!out.empty()) out += "\n";
        out += "  " + e;
    }
    return out;
}

}  // namespace

std::shared_ptr<MibModel::Data> MibModel::collect(std::vector<std::string> modules) {
    auto d = std::make_shared<MibModel::Data>();
    d->modules = std::move(modules);

    struct Item {
        const struct tree* tp;
        const struct tree* parent;
        Oid oid;
    };
    std::vector<Item> stack;
    for (const struct tree* tp = get_tree_head(); tp; tp = tp->next_peer)
        stack.push_back({tp, nullptr, Oid{static_cast<SubId>(tp->subid)}});
    while (!stack.empty()) {
        Item it = std::move(stack.back());
        stack.pop_back();
        if (it.tp->label) d->nodes.push_back(convert(it.tp, it.parent, it.oid));
        for (const struct tree* c = it.tp->child_list; c; c = c->next_peer)
            stack.push_back({c, it.tp, it.oid + static_cast<SubId>(c->subid)});
    }
    std::sort(d->nodes.begin(), d->nodes.end(), [](const MibNode& a, const MibNode& b) { return a.oid < b.oid; });
    d->nodes.erase(std::unique(d->nodes.begin(), d->nodes.end(), [](const MibNode& a, const MibNode& b) { return a.oid == b.oid; }),
                   d->nodes.end());

    for (std::size_t i = 0; i < d->nodes.size(); ++i) {
        const MibNode& n = d->nodes[i];
        d->byOid.emplace(n.oid, i);
        d->byQualified.emplace(n.module + "::" + n.name, i);
        d->byName[n.name].push_back(i);
    }

    // AUGMENTS: the augmenting entry uses the INDEX of the augmented one
    for (MibNode& n : d->nodes) {
        if (n.kind != MibNodeKind::Entry || n.augments.empty() || !n.index.empty()) continue;
        std::string target = n.augments;
        for (int hop = 0; hop < 8 && n.index.empty(); ++hop) {
            auto q = d->byQualified.find(n.module + "::" + target);
            const MibNode* t = nullptr;
            if (q != d->byQualified.end()) t = &d->nodes[q->second];
            else if (auto b = d->byName.find(target); b != d->byName.end() && b->second.size() == 1) t = &d->nodes[b->second.front()];
            if (!t) break;
            if (!t->index.empty()) n.index = t->index;
            else target = t->augments;
        }
    }
    return d;
}

// ---------------------------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------------------------

MibModel MibModel::load(const std::vector<std::string>& files, const std::vector<std::string>& mibDirs) {
    std::lock_guard<std::mutex> lock(g_loadMutex);
    netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_SAVE_MIB_DESCRS, 1);
    netsnmp_init_mib();  // idempotent (returns at once if the MIB tree exists); loads Net-SNMP's default MIBs

    std::vector<std::string> modules;
    Capture cap;
    {
        LogCapture capture(cap);
        for (const auto& dir : mibDirs)
            if (add_mibdir(dir.c_str()) < 0) cap.errors.push_back("cannot read MIB directory " + dir);
        for (const auto& file : files) {
            const std::string module = scanModuleName(file);  // throws for missing / non-MIB files
            const auto dir = std::filesystem::path(file).parent_path();
            add_mibdir(dir.empty() ? "." : dir.string().c_str());
            read_mib(file.c_str());
            modules.push_back(module);
        }
    }
    if (!cap.errors.empty()) throw Error("loading MIB failed:\n" + joinErrors(cap.errors));

    MibModel model(collect(modules));
    for (const auto& m : modules)
        if (model.objects(m).empty()) throw Error("MIB module " + m + " was not loaded (no objects found)");
    return model;
}

// ---------------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------------


const MibNode* MibModel::find(std::string_view name) const {
    const std::string key(name);
    if (key.find("::") != std::string::npos) {
        auto it = d_->byQualified.find(key);
        return it == d_->byQualified.end() ? nullptr : &d_->nodes[it->second];
    }
    auto it = d_->byName.find(key);
    if (it == d_->byName.end()) return nullptr;
    if (it->second.size() > 1) {
        // prefer a definition from an explicitly loaded module
        const MibNode* hit = nullptr;
        for (std::size_t i : it->second)
            if (std::find(d_->modules.begin(), d_->modules.end(), d_->nodes[i].module) != d_->modules.end()) {
                if (hit) throw Error("MIB name '" + key + "' is ambiguous; use MODULE::" + key);
                hit = &d_->nodes[i];
            }
        if (!hit) throw Error("MIB name '" + key + "' is ambiguous; use MODULE::" + key);
        return hit;
    }
    return &d_->nodes[it->second.front()];
}

const MibNode& MibModel::node(std::string_view name) const {
    const MibNode* n = find(name);
    if (!n) throw Error("unknown MIB object '" + std::string(name) + "'");
    return *n;
}

const MibNode* MibModel::findByOid(const Oid& oid) const {
    auto it = d_->byOid.find(oid);
    return it == d_->byOid.end() ? nullptr : &d_->nodes[it->second];
}

const MibNode* MibModel::nodeFor(const Oid& oid) const {
    std::vector<SubId> ids = oid.ids();
    while (!ids.empty()) {
        if (const MibNode* n = findByOid(Oid(ids))) return n;
        ids.pop_back();
    }
    return nullptr;
}

Oid MibModel::resolve(std::string_view text) const {
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    text.remove_prefix(i);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    if (text.empty()) throw Error("empty OID / name");
    if (std::isdigit(static_cast<unsigned char>(text.front())) || text.front() == '.') return Oid::parse(text);

    // name part: up to the first '.' (module names and descriptors contain no dots)
    std::size_t dot = text.find('.');
    Oid out = node(text.substr(0, dot)).oid;
    if (dot == std::string_view::npos) return out;

    std::size_t pos = dot;
    while (pos < text.size()) {
        if (text[pos] != '.') throw Error("malformed name '" + std::string(text) + "'");
        ++pos;
        if (pos >= text.size()) throw Error("malformed name '" + std::string(text) + "' (trailing dot)");
        const char c = text[pos];
        if (c == '"' || c == '\'') {
            const std::size_t end = text.find(c, pos + 1);
            if (end == std::string_view::npos) throw Error("unterminated string in '" + std::string(text) + "'");
            const std::string_view s = text.substr(pos + 1, end - pos - 1);
            out.append(indexString(s, c == '\''));
            pos = end + 1;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t end = pos;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
            out.append(Oid::parse(text.substr(pos, end - pos)));
            pos = end;
        } else {
            throw Error("malformed index in '" + std::string(text) + "'");
        }
    }
    return out;
}

std::vector<const MibNode*> MibModel::objects(std::string_view module) const {
    std::vector<const MibNode*> out;
    for (const MibNode& n : d_->nodes)
        if (n.module == module) out.push_back(&n);
    return out;
}

const MibNode& MibModel::entryOf(const MibNode& table) const {
    if (table.kind == MibNodeKind::Entry) return table;
    if (table.kind == MibNodeKind::Table)
        if (const MibNode* e = findByOid(table.oid + SubId{1}); e && e->kind == MibNodeKind::Entry) return *e;
    throw Error("'" + table.name + "' is not a table");
}

std::vector<const MibNode*> MibModel::columns(const MibNode& table) const {
    const MibNode& entry = entryOf(table);
    std::vector<const MibNode*> out;
    auto it = d_->byOid.upper_bound(entry.oid);
    for (; it != d_->byOid.end() && entry.oid.isPrefixOf(it->first); ++it) {
        const MibNode& n = d_->nodes[it->second];
        if (n.oid.size() == entry.oid.size() + 1 && n.kind == MibNodeKind::Column) out.push_back(&n);
    }
    return out;
}

std::vector<IndexSpec> MibModel::indexSpecs(const MibNode& table) const {
    const MibNode& entry = entryOf(table);
    if (entry.index.empty()) throw Error("table '" + table.name + "' has no INDEX");
    std::vector<IndexSpec> out;
    for (const MibIndexPart& part : entry.index) {
        const MibNode* n = find(entry.module + "::" + part.name);
        if (!n) n = find(part.name);
        if (!n) throw Error("index object '" + part.name + "' of '" + table.name + "' is unknown");
        switch (n->type) {
            case Type::Integer: out.push_back(IndexSpec::integer()); break;
            case Type::Gauge32:
            case Type::Counter32:
            case Type::TimeTicks: out.push_back(IndexSpec::unsignedInt()); break;
            case Type::IpAddress: out.push_back(IndexSpec::ipAddress()); break;
            case Type::ObjectId: out.push_back(part.implied ? IndexSpec::impliedObjectId() : IndexSpec::objectId()); break;
            case Type::OctetString:
            case Type::Opaque:
            case Type::Bits:
                if (part.implied)
                    out.push_back(IndexSpec::impliedString());
                else if (n->ranges.size() == 1 && n->ranges[0].low == n->ranges[0].high)
                    out.push_back(IndexSpec::fixedString(static_cast<std::size_t>(n->ranges[0].low)));
                else
                    out.push_back(IndexSpec::string());
                break;
            default:
                throw Error("index object '" + part.name + "' has a type that cannot be used as index");
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Validation, formatting, parsing
// ---------------------------------------------------------------------------------------------

void MibModel::validate(const MibNode& node, const Value& value) const {
    if (value.type() != node.type)
        throw SetError(ErrorStatus::WrongType, node.name + ": expected " + toString(node.type) + ", got " + toString(value.type()));

    auto inRanges = [&](std::int64_t v) {
        if (node.ranges.empty()) return true;
        for (const MibRange& r : node.ranges)
            if (v >= r.low && v <= r.high) return true;
        return false;
    };
    auto rangeText = [&] {
        std::string s;
        for (const MibRange& r : node.ranges) {
            if (!s.empty()) s += " | ";
            s += r.low == r.high ? std::to_string(r.low) : std::to_string(r.low) + ".." + std::to_string(r.high);
        }
        return s;
    };

    switch (value.type()) {
        case Type::Integer: {
            const std::int32_t v = value.asInt();
            if (!node.enums.empty()) {
                bool known = false;
                for (const MibEnum& e : node.enums) known = known || e.value == v;
                if (!known) throw SetError(ErrorStatus::WrongValue, node.name + ": " + std::to_string(v) + " is not a defined value");
            }
            if (!inRanges(v)) throw SetError(ErrorStatus::WrongValue, node.name + ": value must be in " + rangeText());
            break;
        }
        case Type::Gauge32:
        case Type::Counter32:
        case Type::TimeTicks:
            if (!inRanges(value.asUInt())) throw SetError(ErrorStatus::WrongValue, node.name + ": value must be in " + rangeText());
            break;
        case Type::OctetString:
        case Type::Opaque:
        case Type::Bits:
            if (!inRanges(static_cast<std::int64_t>(value.asString().size())))
                throw SetError(ErrorStatus::WrongLength, node.name + ": length must be in " + rangeText());
            break;
        default:
            break;
    }
}

std::string MibModel::formatValue(const MibNode* node, const Value& value) const {
    if (value.isException()) return toString(value.type());
    std::string out;
    switch (value.type()) {
        case Type::Integer: {
            const std::int32_t v = value.asInt();
            out = std::to_string(v);
            if (node)
                for (const MibEnum& e : node->enums)
                    if (e.value == v) out = e.label + "(" + std::to_string(v) + ")";
            break;
        }
        case Type::Gauge32:
        case Type::Counter32:
        case Type::Counter64: out = std::to_string(value.asUInt64()); break;
        case Type::TimeTicks: {
            // like Net-SNMP: "(153) 0:00:01.53", with "N days, " in front when needed
            const std::uint32_t t = value.asUInt();
            const std::uint32_t days = t / 8640000u;
            char buf[96];
            std::snprintf(buf, sizeof buf, "(%u) %s%u:%02u:%02u.%02u", t,
                          days ? (std::to_string(days) + (days == 1 ? " day, " : " days, ")).c_str() : "",
                          t / 360000u % 24u, t / 6000u % 60u, t / 100u % 60u, t % 100u);
            out = buf;
            break;
        }
        case Type::Opaque:
        case Type::Bits: {
            // always hex, like Net-SNMP: "0A 1B"
            char buf[4];
            for (unsigned char c : value.asString()) {
                std::snprintf(buf, sizeof buf, "%02X", c);
                out += (out.empty() ? "" : " ") + std::string(buf);
            }
            break;
        }
        case Type::OctetString: {
            const std::string& s = value.asString();
            bool printable = true;
            for (unsigned char c : s) printable = printable && (std::isprint(c) || c == '\n' || c == '\r' || c == '\t');
            if (printable) {
                out = "\"" + s + "\"";
            } else {
                out = value.str().substr(std::string("OctetString (hex): ").size());
            }
            break;
        }
        case Type::ObjectId: {
            const Oid& o = value.asOid();
            const MibNode* n = findByOid(o);
            out = n ? n->name : o.str();
            break;
        }
        case Type::IpAddress: {
            auto ip = value.asIp();
            out = std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." + std::to_string(ip[3]);
            break;
        }
        default: out = value.str(); break;
    }
    if (node && !node->units.empty() && value.type() != Type::OctetString && value.type() != Type::Opaque && value.type() != Type::Bits)
        out += " " + node->units;
    return out;
}

std::string MibModel::format(const VarBind& vb) const {
    const MibNode* n = nodeFor(vb.oid);
    std::string name;
    if (n) {
        name = n->name;
        if (vb.oid.size() > n->oid.size()) name += "." + n->oid.suffixOf(vb.oid).str();
    } else {
        name = vb.oid.str();
    }
    return name + " = " + formatValue(n, vb.value);
}

namespace {

std::uint64_t parseUnsigned(std::string_view text, std::uint64_t max, const std::string& what) {
    if (text.empty()) throw Error(what + ": empty number");
    std::uint64_t v = 0;
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) throw Error(what + ": '" + std::string(text) + "' is not a number");
        const std::uint64_t d = static_cast<std::uint64_t>(c - '0');
        if (v > (max - d) / 10) throw Error(what + ": '" + std::string(text) + "' is too large");
        v = v * 10 + d;
    }
    return v;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

}  // namespace

Value MibModel::parseValue(std::string_view name, std::string_view text) const {
    const MibNode* n = nodeFor(resolve(name));
    if (!n || (n->kind != MibNodeKind::Scalar && n->kind != MibNodeKind::Column))
        throw Error("'" + std::string(name) + "' is not a scalar or column");
    return parseValue(*n, text);
}

Value MibModel::parseValue(const MibNode& node, std::string_view textIn) const {
    std::string_view text = trim(textIn);
    Value v;
    auto makeString = [&](std::string bytes) {
        return node.type == Type::Opaque ? Value::opaque(std::move(bytes))
               : node.type == Type::Bits ? Value::bits(std::move(bytes))
                                         : Value::string(std::move(bytes));
    };
    switch (node.type) {
        case Type::Integer: {
            // enumeration label: "active" or "active(1)"
            std::string_view label = text.substr(0, text.find('('));
            for (const MibEnum& e : node.enums)
                if (label == e.label) {
                    v = Value::integer(e.value);
                    break;
                }
            if (v.type() == Type::Null) {
                const bool neg = !text.empty() && text.front() == '-';
                const std::uint64_t mag = parseUnsigned(neg ? text.substr(1) : text, 2147483648ULL, node.name);
                if (!neg && mag > 2147483647ULL) throw Error(node.name + ": '" + std::string(text) + "' is too large");
                v = Value::integer(neg ? static_cast<std::int32_t>(-static_cast<std::int64_t>(mag)) : static_cast<std::int32_t>(mag));
            }
            break;
        }
        case Type::Gauge32: v = Value::gauge(static_cast<std::uint32_t>(parseUnsigned(text, 0xFFFFFFFFULL, node.name))); break;
        case Type::Counter32: v = Value::counter32(static_cast<std::uint32_t>(parseUnsigned(text, 0xFFFFFFFFULL, node.name))); break;
        case Type::TimeTicks: v = Value::timeTicks(static_cast<std::uint32_t>(parseUnsigned(text, 0xFFFFFFFFULL, node.name))); break;
        case Type::Counter64: v = Value::counter64(parseUnsigned(text, std::numeric_limits<std::uint64_t>::max(), node.name)); break;
        case Type::OctetString:
        case Type::Opaque:
        case Type::Bits: {
            // SMI notations (also used in DEFVAL): '0A1B'H (hex) and '0101'B (binary); "text" or plain text
            const bool smi = text.size() >= 3 && text.front() == '\'' && text[text.size() - 2] == '\'';
            const char radix = smi ? static_cast<char>(std::toupper(static_cast<unsigned char>(text.back()))) : '\0';
            if (smi && (radix == 'H' || radix == 'B')) {
                const std::string_view digits = text.substr(1, text.size() - 3);
                const std::size_t per = radix == 'H' ? 2 : 8;
                if (digits.size() % per != 0)
                    throw Error(node.name + ": '" + std::string(text) + "' must have whole octets");
                std::string bytes;
                for (std::size_t i = 0; i < digits.size(); i += per) {
                    unsigned v8 = 0;
                    for (std::size_t k = 0; k < per; ++k) {
                        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(digits[i + k])));
                        int d;
                        if (radix == 'B' && (c == '0' || c == '1')) d = c - '0';
                        else if (radix == 'H' && std::isdigit(static_cast<unsigned char>(c))) d = c - '0';
                        else if (radix == 'H' && c >= 'a' && c <= 'f') d = c - 'a' + 10;
                        else throw Error(node.name + ": invalid digit in '" + std::string(text) + "'");
                        v8 = v8 * (radix == 'H' ? 16u : 2u) + static_cast<unsigned>(d);
                    }
                    bytes.push_back(static_cast<char>(v8));
                }
                v = makeString(std::move(bytes));
                break;
            }
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
            v = makeString(std::string(text));
            break;
        }
        case Type::IpAddress: {
            const Oid o = Oid::parse(text);
            if (o.size() != 4 || o[0] > 255 || o[1] > 255 || o[2] > 255 || o[3] > 255)
                throw Error(node.name + ": '" + std::string(text) + "' is not an IPv4 address");
            v = Value::ipAddress(static_cast<std::uint8_t>(o[0]), static_cast<std::uint8_t>(o[1]), static_cast<std::uint8_t>(o[2]),
                                 static_cast<std::uint8_t>(o[3]));
            break;
        }
        case Type::ObjectId: v = Value::oid(resolve(text)); break;
        default: throw Error(node.name + ": values of this object cannot be parsed");
    }
    validate(node, v);
    return v;
}

}  // namespace snmpwrap
