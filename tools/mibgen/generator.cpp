#include "generator.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include "snmpwrap/error.hpp"

namespace snmpwrap::mibgen {

namespace {

// ---------------------------------------------------------------------------------------------
// naming / text helpers
// ---------------------------------------------------------------------------------------------

const std::set<std::string>& keywords() {
    static const std::set<std::string> k{
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
        "char", "char16_t", "char32_t", "class", "compl", "const", "constexpr", "const_cast", "continue", "decltype",
        "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false",
        "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
        "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register",
        "reinterpret_cast", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union",
        "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq", "oids", "impl", "mib",
        "agent", "index", "value", "values", "status", "client"};
    return k;
}

/// MIB descriptor -> C++ identifier (hyphens are legal in SMI enum labels; keywords get a trailing '_').
std::string ident(const std::string& s) {
    std::string out;
    for (char c : s) out.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_');
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
    if (keywords().count(out)) out += "_";
    return out;
}

std::string upperFirst(const std::string& s) {
    std::string out = ident(s);
    if (!out.empty() && out.back() == '_' && keywords().count(out.substr(0, out.size() - 1))) out.pop_back();
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

/// Text for a Doxygen comment: whitespace collapsed, comment terminators defused.
std::string doc(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = !out.empty();
            continue;
        }
        if (space) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    std::string::size_type p;
    while ((p = out.find("*/")) != std::string::npos) out.replace(p, 2, "* /");
    return out;
}

std::string oidInit(const Oid& o) {
    std::string out = "{";
    for (std::size_t i = 0; i < o.size(); ++i) out += (i ? ", " : "") + std::to_string(o[i]);
    return out + "}";
}

std::string cppString(const std::string& s) {
    std::string out = "std::string(\"";
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (std::isprint(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\%03o", c);
            out += buf;
        }
    }
    return out + "\", " + std::to_string(s.size()) + ")";
}

std::string typeEnum(Type t) { return std::string("snmpwrap::Type::") + toString(t); }

bool isStringLike(Type t) { return t == Type::OctetString || t == Type::Opaque || t == Type::Bits; }

std::string accessText(const MibNode& n) {
    std::string s = toString(n.access);
    s += ", ";
    s += n.textualConvention.empty() ? toString(n.type) : n.textualConvention;
    if (!n.ranges.empty()) {
        s += isStringLike(n.type) ? " SIZE(" : " (";
        for (std::size_t i = 0; i < n.ranges.size(); ++i) {
            if (i) s += " | ";
            s += std::to_string(n.ranges[i].low);
            if (n.ranges[i].high != n.ranges[i].low) s += ".." + std::to_string(n.ranges[i].high);
        }
        s += ")";
    }
    if (!n.units.empty()) s += ", UNITS \"" + n.units + "\"";
    return s;
}

// ---------------------------------------------------------------------------------------------
// type mapping
// ---------------------------------------------------------------------------------------------

bool isRowStatus(const MibNode& n) { return n.textualConvention == "RowStatus"; }

std::string enumName(const MibNode& n) {
    if (isRowStatus(n)) return "snmpwrap::RowStatus";
    if (!n.textualConvention.empty()) return upperFirst(n.textualConvention);
    return upperFirst(n.name);
}

bool hasEnum(const MibNode& n) { return n.type == Type::Integer && !n.enums.empty(); }

std::string cppType(const MibNode& n) {
    switch (n.type) {
        case Type::Integer: return hasEnum(n) ? enumName(n) : "std::int32_t";
        case Type::Gauge32:
        case Type::Counter32:
        case Type::TimeTicks: return "std::uint32_t";
        case Type::Counter64: return "std::uint64_t";
        case Type::OctetString:
        case Type::Opaque:
        case Type::Bits: return "std::string";
        case Type::ObjectId: return "snmpwrap::Oid";
        case Type::IpAddress: return "std::array<std::uint8_t, 4>";
        default: throw Error("object '" + n.name + "' has a type the generator does not support");
    }
}

std::string paramType(const MibNode& n) {
    const std::string t = cppType(n);
    return (isStringLike(n.type) || n.type == Type::ObjectId || n.type == Type::IpAddress) ? "const " + t + "&" : t;
}

std::string toValue(const MibNode& n, const std::string& e) {
    switch (n.type) {
        case Type::Integer: return hasEnum(n) ? "snmpwrap::Value::integer(static_cast<std::int32_t>(" + e + "))" : "snmpwrap::Value::integer(" + e + ")";
        case Type::Gauge32: return "snmpwrap::Value::gauge(" + e + ")";
        case Type::Counter32: return "snmpwrap::Value::counter32(" + e + ")";
        case Type::TimeTicks: return "snmpwrap::Value::timeTicks(" + e + ")";
        case Type::Counter64: return "snmpwrap::Value::counter64(" + e + ")";
        case Type::OctetString: return "snmpwrap::Value::string(" + e + ")";
        case Type::Opaque: return "snmpwrap::Value::opaque(" + e + ")";
        case Type::Bits: return "snmpwrap::Value::bits(" + e + ")";
        case Type::ObjectId: return "snmpwrap::Value::oid(" + e + ")";
        case Type::IpAddress: return "ipValue(" + e + ")";
        default: throw Error("unsupported type");
    }
}

std::string fromValue(const MibNode& n, const std::string& v) {
    switch (n.type) {
        case Type::Integer: return hasEnum(n) ? "static_cast<" + enumName(n) + ">(" + v + ".asInt())" : v + ".asInt()";
        case Type::Gauge32:
        case Type::Counter32:
        case Type::TimeTicks: return v + ".asUInt()";
        case Type::Counter64: return v + ".asUInt64()";
        case Type::OctetString:
        case Type::Opaque:
        case Type::Bits: return v + ".asString()";
        case Type::ObjectId: return v + ".asOid()";
        case Type::IpAddress: return v + ".asIp()";
        default: throw Error("unsupported type");
    }
}

/// C++ literal of a (DEFVAL) value for the node's C++ type.
std::string literal(const MibNode& n, const Value& v) {
    switch (n.type) {
        case Type::Integer:
            return hasEnum(n) ? "static_cast<" + enumName(n) + ">(" + std::to_string(v.asInt()) + ")" : std::to_string(v.asInt());
        case Type::Gauge32:
        case Type::Counter32:
        case Type::TimeTicks: return std::to_string(v.asUInt()) + "u";
        case Type::Counter64: return std::to_string(v.asUInt64()) + "ULL";
        case Type::OctetString:
        case Type::Opaque:
        case Type::Bits: return cppString(v.asString());
        case Type::ObjectId: return "snmpwrap::Oid" + oidInit(v.asOid());
        case Type::IpAddress: {
            auto ip = v.asIp();
            return "std::array<std::uint8_t, 4>{" + std::to_string(ip[0]) + ", " + std::to_string(ip[1]) + ", " +
                   std::to_string(ip[2]) + ", " + std::to_string(ip[3]) + "}";
        }
        default: throw Error("unsupported type");
    }
}

std::string indexSpecExpr(const IndexSpec& s) {
    switch (s.kind) {
        case IndexKind::Integer: return "snmpwrap::IndexSpec::integer()";
        case IndexKind::Unsigned: return "snmpwrap::IndexSpec::unsignedInt()";
        case IndexKind::String: return "snmpwrap::IndexSpec::string()";
        case IndexKind::ImpliedString: return "snmpwrap::IndexSpec::impliedString()";
        case IndexKind::FixedString: return "snmpwrap::IndexSpec::fixedString(" + std::to_string(s.size) + ")";
        case IndexKind::IpAddress: return "snmpwrap::IndexSpec::ipAddress()";
        case IndexKind::ObjectId: return "snmpwrap::IndexSpec::objectId()";
        case IndexKind::ImpliedObjectId: return "snmpwrap::IndexSpec::impliedObjectId()";
    }
    return "";
}

/// Value type an index column has in encodeIndex()/decodeIndex() (Unsigned index columns use Gauge32).
std::string indexToValue(const MibNode& n, const std::string& e) {
    if (n.type == Type::Counter32 || n.type == Type::TimeTicks) return "snmpwrap::Value::gauge(" + e + ")";
    return toValue(n, e);
}

// ---------------------------------------------------------------------------------------------
// collected module information
// ---------------------------------------------------------------------------------------------

struct Table {
    const MibNode* table = nullptr;
    const MibNode* entry = nullptr;
    std::vector<const MibNode*> index;      // index objects
    std::vector<IndexSpec> specs;
    std::vector<const MibNode*> columns;    // readable, without RowStatus
    const MibNode* rowStatus = nullptr;
    std::string indexType;                  // SwtEntryIndex
    std::string entryType;                  // SwtEntry
    std::string valuesType;                 // SwtEntryValues (RowStatus tables)
    std::string rowsFn, nextFn, hasFn;      // swtTableRows / swtTableNext / swtTableHas
    std::string createFn, destroyFn, completeFn;

    bool anyWritable() const {
        return std::any_of(columns.begin(), columns.end(), [](const MibNode* c) { return c->writable(); });
    }
    std::vector<const MibNode*> writable() const {
        std::vector<const MibNode*> w;
        for (const MibNode* c : columns)
            if (c->writable()) w.push_back(c);
        return w;
    }
};

/// One group node of the MIB tree (an OBJECT IDENTIFIER between the registration root and objects with values).
/// Groups have no values of their own; they only structure the generated Data type.
struct Group {
    SubId subid = 0;                      // last sub-identifier below the parent
    std::string mibName, member, type;    // MIB descriptor, member name in the parent, nested struct type
    std::string description;
    std::vector<const MibNode*> scalars;  // readable scalars directly below this group
    std::vector<std::size_t> tables;      // indexes into Module::tables
    std::vector<Group> children;
};

struct Module {
    std::string name, ns;
    Oid root;
    bool hasRoot = false;
    Oid dataRoot;                         // where Data / Remote start: the module node if it is above every object, else root
    Group data;                           // root of the group tree (below dataRoot)
    bool hasData = false;                 // Data / DataAgent are generated
    std::vector<const MibNode*> all, scalars, notifications;
    std::vector<Table> tables;
    std::map<std::string, const MibNode*> enums;  // enum type name -> first node using it
    std::vector<const MibNode*> checked;          // objects that get a generated check function (all readable scalars / columns)
};

const MibNode& resolveName(const MibModel& m, const std::string& module, const std::string& name) {
    if (const MibNode* n = m.find(module + "::" + name)) return *n;
    return m.node(name);
}

/// C++ member name of a MIB object / group. "validate" is taken by Data::validate().
std::string memberName(const std::string& mibName) {
    const std::string s = ident(mibName);
    return s == "validate" ? s + "_" : s;
}

/// Puts a scalar or table into the group chain given by the OIDs between the registration root and the object.
void placeInGroup(const MibModel& m, Module& mod, const MibNode& n, bool isTable, std::size_t tableIndex) {
    if (!mod.dataRoot.isPrefixOf(n.oid) || n.oid.size() <= mod.dataRoot.size())
        throw Error("object '" + n.name + "' is not below " + mod.dataRoot.str());
    Group* g = &mod.data;
    for (std::size_t k = mod.dataRoot.size(); k + 1 < n.oid.size(); ++k) {
        const SubId sid = n.oid[k];
        auto it = std::find_if(g->children.begin(), g->children.end(), [sid](const Group& c) { return c.subid == sid; });
        if (it == g->children.end()) {
            Group c;
            c.subid = sid;
            const MibNode* node = m.findByOid(Oid(std::vector<SubId>(n.oid.ids().begin(), n.oid.ids().begin() + static_cast<std::ptrdiff_t>(k) + 1)));
            const bool named = node && node->name.rfind("anonymous#", 0) != 0;
            c.mibName = named ? node->name : "node" + std::to_string(sid);
            c.description = named ? node->description : "";
            c.member = memberName(c.mibName);
            c.type = upperFirst(c.mibName) + "Group";
            g->children.push_back(std::move(c));
            it = g->children.end() - 1;
        }
        g = &*it;
    }
    if (isTable) g->tables.push_back(tableIndex);
    else g->scalars.push_back(&n);
}

void sortGroups(Group& g) {
    std::sort(g.children.begin(), g.children.end(), [](const Group& a, const Group& b) { return a.subid < b.subid; });
    for (Group& c : g.children) sortGroups(c);
}

/// Two members of one group must not share a C++ name.
void checkMembers(const Module& mod, const Group& g, const std::string& path) {
    std::map<std::string, std::string> seen;  // member -> MIB name
    auto add = [&](const std::string& member, const std::string& mibName) {
        auto [it, fresh] = seen.emplace(member, mibName);
        if (!fresh)
            throw Error("'" + it->second + "' and '" + mibName + "' both map to the C++ member '" + member + "'" +
                        (path.empty() ? "" : " in group " + path));
    };
    for (const MibNode* n : g.scalars) add(memberName(n->name), n->name);
    for (std::size_t i : g.tables) add(memberName(mod.tables[i].table->name), mod.tables[i].table->name);
    for (const Group& c : g.children) {
        add(c.member, c.mibName);
        checkMembers(mod, c, path.empty() ? c.mibName : path + "." + c.mibName);
    }
}

Module collect(const MibModel& m, const Options& o) {
    Module mod;
    mod.name = o.module;
    mod.ns = o.baseName.empty() ? snakeCase(o.module) : o.baseName;
    for (const MibNode* n : m.objects(o.module))
        if (n->name.rfind("anonymous#", 0) != 0) mod.all.push_back(n);  // Net-SNMP's nodes for "{ a b c }" paths
    if (mod.all.empty()) throw Error("module " + o.module + " is not loaded or has no objects");

    for (const MibNode* n : mod.all) {
        if (n->kind == MibNodeKind::Scalar && n->readable()) mod.scalars.push_back(n);
        if (n->kind == MibNodeKind::Notification) mod.notifications.push_back(n);
        if (n->kind != MibNodeKind::Table) continue;
        Table t;
        t.table = n;
        t.entry = &m.entryOf(*n);
        t.specs = m.indexSpecs(*n);
        for (const MibIndexPart& p : t.entry->index) t.index.push_back(&resolveName(m, t.entry->module, p.name));
        for (const MibNode* c : m.columns(*n)) {
            if (!c->readable()) continue;
            if (isRowStatus(*c)) t.rowStatus = c;
            else t.columns.push_back(c);
        }
        const std::string e = upperFirst(t.entry->name);
        t.indexType = e + "Index";
        t.entryType = e;
        t.valuesType = e + "Values";
        t.rowsFn = ident(n->name) + "Rows";
        t.nextFn = ident(n->name) + "Next";
        t.hasFn = ident(n->name) + "Has";
        t.createFn = "create" + e;
        t.destroyFn = "destroy" + e;
        t.completeFn = ident(t.entry->name) + "Complete";
        mod.tables.push_back(std::move(t));
    }

    // enum types: every Integer with named numbers (except RowStatus, which is snmpwrap::RowStatus)
    auto noteEnum = [&](const MibNode* n) {
        if (hasEnum(*n) && !isRowStatus(*n)) mod.enums.emplace(enumName(*n), n);
    };
    for (const MibNode* n : mod.scalars) noteEnum(n);
    for (const Table& t : mod.tables) {
        for (const MibNode* n : t.index) noteEnum(n);
        for (const MibNode* n : t.columns) noteEnum(n);
    }
    for (const MibNode* n : mod.notifications)
        for (const std::string& obj : n->objects) noteEnum(&resolveName(m, n->module, obj));

    for (const MibNode* n : mod.scalars) mod.checked.push_back(n);
    for (const Table& t : mod.tables)
        for (const MibNode* c : t.columns) mod.checked.push_back(c);

    // registration root
    if (!o.rootName.empty()) {
        mod.root = m.oid(o.rootName);
        mod.hasRoot = true;
    } else {
        std::vector<Oid> objs;
        for (const MibNode* n : mod.scalars) objs.push_back(n->oid);
        for (const Table& t : mod.tables) objs.push_back(t.table->oid);
        if (!objs.empty()) {
            std::vector<SubId> prefix = objs.front().ids();
            for (const Oid& x : objs) {
                std::size_t k = 0;
                while (k < prefix.size() && k < x.size() && prefix[k] == x[k]) ++k;
                prefix.resize(k);
            }
            for (const Oid& x : objs)
                if (Oid(prefix) == x && !prefix.empty()) prefix.pop_back();  // root must be above every object
            if (prefix.empty()) throw Error("objects of " + o.module + " have no common OID prefix; use --root");
            mod.root = Oid(prefix);
            mod.hasRoot = true;
        }
    }

    // group tree for the Data type
    if (mod.hasRoot && (!mod.scalars.empty() || !mod.tables.empty())) {
        // The structure starts at the module's top node (usually its MODULE-IDENTITY), so that a group stays a group
        // even when every object of the module lives inside it. The registration root is the deepest common prefix.
        mod.dataRoot = mod.root;
        const Oid& top = mod.all.front()->oid;  // nodes are sorted by OID
        bool above = top.size() < mod.root.size();
        for (const MibNode* n : mod.scalars) above = above && top.isPrefixOf(n->oid);
        for (const Table& t : mod.tables) above = above && top.isPrefixOf(t.table->oid);
        if (above) mod.dataRoot = top;
        for (const MibNode* n : mod.scalars) placeInGroup(m, mod, *n, false, 0);
        for (std::size_t i = 0; i < mod.tables.size(); ++i) placeInGroup(m, mod, *mod.tables[i].table, true, i);
        sortGroups(mod.data);
        checkMembers(mod, mod.data, "");
        mod.hasData = true;
    }
    return mod;
}

// ---------------------------------------------------------------------------------------------
// writer
// ---------------------------------------------------------------------------------------------

class W {
public:
    W& operator()(const std::string& line = "") {
        out_ << line << "\n";
        return *this;
    }
    std::string str() const { return out_.str(); }

private:
    std::ostringstream out_;
};

std::string checkFn(const MibNode& n) { return "check_" + ident(n.name); }

/// Parameters and varbinds of a send<Notification>() function. Columns of a table generated in this
/// module share one typed index parameter per table; columns of foreign tables get a raw Oid index.
struct NotificationSig {
    std::string params;                    // "snmpwrap::Agent& agent, ..."
    std::vector<std::string> paramDocs;    // "@param[in] x ..." lines
    std::vector<std::string> varbinds;     // "{oid + suffix, value}" expressions
};

NotificationSig notificationSig(const MibModel& m, const Module& mod, const MibNode& n) {
    NotificationSig sig;
    sig.params = "snmpwrap::Agent& agent";
    sig.paramDocs.push_back("agent The agent.");
    std::vector<std::pair<std::string, std::string>> indexParams;  // (declaration, doc) appended after the values
    std::set<std::string> seenIndex;
    for (const std::string& objName : n.objects) {
        const MibNode& ob = resolveName(m, n.module, objName);
        const std::string p = ident(ob.name);
        sig.params += ", " + paramType(ob) + " " + p;
        sig.paramDocs.push_back(p + " Value of " + ob.name + ".");
        std::string suffix = "snmpwrap::SubId{0}";
        if (ob.kind == MibNodeKind::Column) {
            const Oid entryOid(std::vector<SubId>(ob.oid.ids().begin(), ob.oid.ids().end() - 1));
            const Table* t = nullptr;
            for (const Table& x : mod.tables)
                if (x.entry->oid == entryOid) t = &x;
            if (t) {
                const std::string ip = ident(t->entry->name) + "Index";
                if (seenIndex.insert(ip).second)
                    indexParams.push_back({"const " + t->indexType + "& " + ip, ip + " Row of " + t->table->name + " the column values belong to."});
                suffix = ip + ".toOid()";
            } else {
                const std::string ip = p + "Index";
                indexParams.push_back({"const snmpwrap::Oid& " + ip, ip + " Row index of " + ob.name + "."});
                suffix = ip;
            }
        }
        sig.varbinds.push_back("{snmpwrap::Oid" + oidInit(ob.oid) + " + " + suffix + ", " + toValue(ob, p) + "}");
    }
    for (const auto& [decl, d] : indexParams) {
        sig.params += ", " + decl;
        sig.paramDocs.push_back(d);
    }
    return sig;
}

void genCheck(W& w, const MibNode& n) {
    w("/// MIB check of " + n.name + ": " + accessText(n));
    w("[[maybe_unused]] void " + checkFn(n) + "(const snmpwrap::Value& v) {");
    w("    if (v.type() != " + typeEnum(n.type) + ")");
    w("        throw snmpwrap::SetError(snmpwrap::ErrorStatus::WrongType, \"" + n.name + ": expected " + toString(n.type) + "\");");
    if (hasEnum(n)) {
        w("    switch (v.asInt()) {");
        std::string cases = "        ";
        for (const MibEnum& e : n.enums) cases += "case " + std::to_string(e.value) + ": ";
        w(cases + "break;");
        w("        default: throw snmpwrap::SetError(snmpwrap::ErrorStatus::WrongValue, \"" + n.name + ": not a defined value\");");
        w("    }");
    }
    if (!n.ranges.empty()) {
        std::string var, status, what;
        switch (n.type) {
            case Type::Integer: var = "static_cast<std::int64_t>(v.asInt())"; status = "WrongValue"; what = "value"; break;
            case Type::Gauge32:
            case Type::Counter32:
            case Type::TimeTicks: var = "static_cast<std::int64_t>(v.asUInt())"; status = "WrongValue"; what = "value"; break;
            case Type::OctetString:
            case Type::Opaque:
            case Type::Bits: var = "static_cast<std::int64_t>(v.asString().size())"; status = "WrongLength"; what = "length"; break;
            default: break;
        }
        if (!var.empty()) {
            std::string cond, text;
            for (std::size_t i = 0; i < n.ranges.size(); ++i) {
                const MibRange& r = n.ranges[i];
                cond += (i ? " || " : "") + std::string("(x >= ") + std::to_string(r.low) + "LL && x <= " + std::to_string(r.high) + "LL)";
                text += (i ? " | " : "") + std::to_string(r.low) + (r.high != r.low ? ".." + std::to_string(r.high) : "");
            }
            w("    const std::int64_t x = " + var + ";");
            w("    if (!(" + cond + "))");
            w("        throw snmpwrap::SetError(snmpwrap::ErrorStatus::" + status + ", \"" + n.name + ": " + what + " must be in " + text + "\");");
        }
    }
    w("}");
    w();
}


// ---------------------------------------------------------------------------------------------
// Data model (nested structs, table containers, DataAgent)
// ---------------------------------------------------------------------------------------------

/// Single-column integer index: the table container accepts a plain number, table[3].
bool plainIntegerIndex(const Table& t) {
    return t.index.size() == 1 && !hasEnum(*t.index[0]) &&
           (t.index[0]->type == Type::Integer || t.index[0]->type == Type::Gauge32 || t.index[0]->type == Type::Counter32 ||
            t.index[0]->type == Type::TimeTicks);
}

std::string tableType(const Table& t) { return t.entryType + "Table"; }

/// Every object that has a value: readable scalars, columns and RowStatus columns (in OID order per kind).
std::vector<const MibNode*> valueObjects(const Module& mod) {
    std::vector<const MibNode*> out(mod.scalars.begin(), mod.scalars.end());
    for (const Table& t : mod.tables) {
        out.insert(out.end(), t.columns.begin(), t.columns.end());
        if (t.rowStatus) out.push_back(t.rowStatus);
    }
    std::sort(out.begin(), out.end(), [](const MibNode* a, const MibNode* b) { return a->oid < b->oid; });
    return out;
}

void emitTableContainer(W& w, const Table& t) {
    const std::string base = "std::map<" + t.indexType + ", " + t.entryType + ">";
    w("/// @brief Rows of " + t.table->name + " by index, in SNMP (OID) order. Used in Data; add a row by accessing it.");
    w("class " + tableType(t) + " : public " + base + " {");
    w("public:");
    w("    using " + base + "::map;");
    w("    using " + base + "::operator[];");
    if (plainIntegerIndex(t)) {
        const std::string ct = cppType(*t.index[0]);
        w("    /// @brief Row for a plain index number (creates the row if it does not exist yet).");
        w("    " + t.entryType + "& operator[](" + ct + " " + ident(t.index[0]->name) + ") { return (*this)[" + t.indexType + "{" + ident(t.index[0]->name) + "}]; }");
    }
    w("};");
    w();
}

void emitGroupBody(W& w, const Module& mod, const Group& g, const std::string& pad) {
    for (const Group& c : g.children) {
        w(pad + "/// @brief Group " + c.mibName + (c.description.empty() ? "" : ": " + doc(c.description)));
        w(pad + "struct " + c.type + " {");
        emitGroupBody(w, mod, c, pad + "    ");
        w(pad + "} " + c.member + ";");
    }
    for (const MibNode* n : g.scalars)
        w(pad + cppType(*n) + " " + memberName(n->name) + "{};  ///< " + n->name + " (" + accessText(*n) + "). " + doc(n->description));
    for (std::size_t i : g.tables) {
        const Table& t = mod.tables[i];
        w(pad + tableType(t) + " " + memberName(t.table->name) + ";  ///< " + t.table->name + ": " + doc(t.table->description));
    }
}

/// `validate()` body part for one group: every value against its MIB definition.
void emitValidate(W& w, const Module& mod, const Group& g, const std::string& path, const std::string& label) {
    for (const MibNode* n : g.scalars) {
        const std::string f = memberName(n->name);
        w("    check(\"" + label + n->name + "\", \"" + n->name + "\", [&] { " + checkFn(*n) + "(" + toValue(*n, path + f) + "); });");
    }
    for (std::size_t i : g.tables) {
        const Table& t = mod.tables[i];
        const std::string tm = path + memberName(t.table->name);
        w("    for (const auto& kv : " + tm + ") {");
        w("        const std::string at = \"" + label + t.table->name + "[\" + kv.first.toOid().str() + \"].\";");
        for (const MibNode* c : t.columns)
            w("        check(at + \"" + c->name + "\", \"" + c->name + "\", [&] { " + checkFn(*c) + "(" + toValue(*c, "kv.second." + ident(c->name)) + "); });");
        w("    }");
    }
    for (const Group& c : g.children) emitValidate(w, mod, c, path + c.member + ".", label + c.mibName + ".");
}

/// Instrumentation overrides for one group: read / write the Data members under the adapter's mutex.
void emitImpl(W& w, const Module& mod, const Group& g, const std::string& path) {
    for (const MibNode* n : g.scalars) {
        const std::string f = ident(n->name), F = upperFirst(n->name), m = path + memberName(n->name);
        w("    " + cppType(*n) + " " + f + "() override {");
        w("        std::lock_guard<std::mutex> l(mu);");
        w("        reading(Object::" + ident(n->name) + ");");
        w("        return d." + m + ";");
        w("    }");
        if (n->writable()) {
            w("    void set" + F + "(" + paramType(*n) + " value) override {");
            w("        std::lock_guard<std::mutex> l(mu);");
            w("        auto& slot = d." + m + ";");
            w("        const auto old = slot;");
            w("        slot = value;");
            w("        try {");
            w("            changed(Object::" + ident(n->name) + ");");
            w("        } catch (...) {");
            w("            slot = old;  // the hook refused the change");
            w("            throw;");
            w("        }");
            w("    }");
        }
    }
    for (std::size_t i : g.tables) {
        const Table& t = mod.tables[i];
        const std::string tbl = "d." + path + memberName(t.table->name);
        const std::string ip = "const " + t.indexType + "& index";
        std::set<std::string> indexNames;
        for (const MibNode* x : t.index) indexNames.insert(x->name);
        w("    std::vector<" + t.indexType + "> " + t.rowsFn + "() override {");
        w("        std::lock_guard<std::mutex> l(mu);");
        w("        std::vector<" + t.indexType + "> r;");
        w("        for (const auto& kv : " + tbl + ") r.push_back(kv.first);");
        w("        return r;");
        w("    }");
        w("    std::optional<" + t.indexType + "> " + t.nextFn + "(const snmpwrap::Oid* after) override {");
        w("        std::lock_guard<std::mutex> l(mu);");
        w("        auto& rows = " + tbl + ";  // ordered like the OIDs of the indexes");
        w("        if (!after) return rows.empty() ? std::nullopt : std::optional<" + t.indexType + ">(rows.begin()->first);");
        w("        if (const auto idx = " + t.indexType + "::fromOid(*after)) {");
        w("            const auto it = rows.upper_bound(*idx);");
        w("            return it == rows.end() ? std::nullopt : std::optional<" + t.indexType + ">(it->first);");
        w("        }");
        w("        for (const auto& kv : rows)  // not a complete index: scan");
        w("            if (kv.first.toOid() > *after) return kv.first;");
        w("        return std::nullopt;");
        w("    }");
        w("    bool " + t.hasFn + "(" + ip + ") override { std::lock_guard<std::mutex> l(mu); return " + tbl + ".count(index) != 0; }");
        const std::string filled = "filled_" + memberName(t.table->name);
        if (t.rowStatus) {
            w("    // columns supplied for rows created by a manager (rows added by the application count as complete)");
            w("    std::map<" + t.indexType + ", std::set<std::string>> " + filled + ";");
        }
        for (const MibNode* c : t.columns) {
            const std::string f = ident(c->name), F = upperFirst(c->name);
            const std::string src = indexNames.count(c->name) ? "index." + ident(c->name) : tbl + ".at(index)." + ident(c->name);
            w("    " + cppType(*c) + " " + f + "(" + ip + ") override {");
            w("        std::lock_guard<std::mutex> l(mu);");
            w("        reading(Object::" + ident(c->name) + ", index.toOid());");
            w("        return " + src + ";");
            w("    }");
            if (c->writable()) {
                w("    void set" + F + "(" + ip + ", " + paramType(*c) + " value) override {");
                w("        std::lock_guard<std::mutex> l(mu);");
                w("        auto& slot = " + tbl + ".at(index)." + f + ";");
                w("        const auto old = slot;");
                w("        slot = value;");
                w("        try {");
                w("            changed(Object::" + ident(c->name) + ", index.toOid());");
                w("        } catch (...) {");
                w("            slot = old;  // the hook refused the change");
                w("            throw;");
                w("        }");
                if (t.rowStatus) {
                    w("        if (const auto it = " + filled + ".find(index); it != " + filled + ".end()) it->second.insert(\"" + c->name + "\");");
                }
                w("    }");
            }
        }
        if (t.rowStatus) {
            const std::string rs = ident(t.rowStatus->name);
            w("    void " + t.createFn + "(" + ip + ", const " + t.valuesType + "& values) override {");
            w("        std::lock_guard<std::mutex> l(mu);");
            w("        " + t.entryType + "& row = " + tbl + "[index];");
            w("        row = " + t.entryType + "{};");
            w("        std::set<std::string>& supplied = " + filled + "[index];");
            w("        supplied.clear();");
            for (const MibNode* c : t.writable())
                w("        if (values." + ident(c->name) + ") {\n            row." + ident(c->name) + " = *values." + ident(c->name) + ";\n            supplied.insert(\"" + c->name + "\");\n        }");
            w("        row." + rs + " = snmpwrap::RowStatus::NotReady;");
            w("        try {");
            w("            changed(Object::" + ident(t.rowStatus->name) + ", index.toOid());");
            w("        } catch (...) {");
            w("            " + tbl + ".erase(index);  // the hook refused the new row");
            w("            " + filled + ".erase(index);");
            w("            throw;");
            w("        }");
            w("    }");
            w("    void " + t.destroyFn + "(" + ip + ") override {");
            w("        std::lock_guard<std::mutex> l(mu);");
            w("        " + tbl + ".erase(index);");
            w("        " + filled + ".erase(index);");
            w("        try {");
            w("            changed(Object::" + ident(t.rowStatus->name) + ", index.toOid());");
            w("        } catch (...) {");
            w("            // a row that is being destroyed cannot be refused any more");
            w("        }");
            w("    }");
            w("    snmpwrap::RowStatus " + rs + "(" + ip + ") override {");
            w("        std::lock_guard<std::mutex> l(mu);");
            w("        reading(Object::" + ident(t.rowStatus->name) + ", index.toOid());");
            w("        return " + tbl + ".at(index)." + rs + ";");
            w("    }");
            w("    void set" + upperFirst(t.rowStatus->name) + "(" + ip + ", snmpwrap::RowStatus status) override {");
            w("        std::lock_guard<std::mutex> l(mu);");
            w("        " + tbl + ".at(index)." + rs + " = status;");
            w("    }");
            // complete: every writable column without DEFVAL was supplied (the rule RowStatusSpec::requiredColumns uses)
            std::string needs;
            for (const MibNode* c : t.writable())
                if (c->defaultValue.empty()) needs += (needs.empty() ? "" : " && ") + std::string("it->second.count(\"") + c->name + "\")";
            w("    bool " + t.completeFn + "(" + ip + ") override {");
            w("        std::lock_guard<std::mutex> l(mu);");
            w("        const auto it = " + filled + ".find(index);");
            w("        if (it == " + filled + ".end()) return true;");
            w("        return " + (needs.empty() ? std::string("true") : needs) + ";");
            w("    }");
        }
    }
    for (const Group& c : g.children) emitImpl(w, mod, c, path + c.member + ".");
}

// ---------------------------------------------------------------------------------------------
// Remote (nested client view on top of the flat generated Client)
// ---------------------------------------------------------------------------------------------

void emitRemoteHelpers(W& w) {
    w("/// @brief Building blocks of Remote: accessors bound to one object (and row) of the remote agent.");
    w("namespace remote_detail {");
    w();
    w("/// @brief Read-only scalar.");
    w("template <auto Get>");
    w("class RoScalar {");
    w("public:");
    w("    explicit RoScalar(Client& c) : c_(&c) {}");
    w("    /// @brief GET. @return The value. @throws snmpwrap::Error, snmpwrap::TransportError, snmpwrap::ResponseError");
    w("    auto get() const { return (c_->*Get)(); }");
    w();
    w("protected:");
    w("    Client* c_;");
    w("};");
    w();
    w("/// @brief Writable scalar.");
    w("template <auto Get, auto Set>");
    w("class RwScalar : public RoScalar<Get> {");
    w("public:");
    w("    using RoScalar<Get>::RoScalar;");
    w("    /// @brief SET; the value is checked against the MIB before it is sent. @param[in] value New value.");
    w("    /// @throws snmpwrap::SetError (MIB check), snmpwrap::TransportError, snmpwrap::ResponseError");
    w("    template <class V>");
    w("    void set(V&& value) const { (this->c_->*Set)(std::forward<V>(value)); }");
    w("};");
    w();
    w("/// @brief Read-only table cell.");
    w("template <class Index, auto Get>");
    w("class RoCell {");
    w("public:");
    w("    RoCell(Client& c, const Index& index) : c_(&c), index_(index) {}");
    w("    /// @brief GET. @return The value. @throws snmpwrap::Error, snmpwrap::TransportError, snmpwrap::ResponseError");
    w("    auto get() const { return (c_->*Get)(index_); }");
    w();
    w("protected:");
    w("    Client* c_;");
    w("    Index index_;");
    w("};");
    w();
    w("/// @brief Writable table cell.");
    w("template <class Index, auto Get, auto Set>");
    w("class RwCell : public RoCell<Index, Get> {");
    w("public:");
    w("    using RoCell<Index, Get>::RoCell;");
    w("    /// @brief SET; the value is checked against the MIB before it is sent. @param[in] value New value.");
    w("    /// @throws snmpwrap::SetError (MIB check), snmpwrap::TransportError, snmpwrap::ResponseError");
    w("    template <class V>");
    w("    void set(V&& value) const { (this->c_->*Set)(this->index_, std::forward<V>(value)); }");
    w("};");
    w();
    w("}  // namespace remote_detail");
    w();
}

std::string remoteScalarType(const MibNode& n) {
    const std::string get = "&Client::" + ident(n.name);
    return n.writable() ? "remote_detail::RwScalar<" + get + ", &Client::set" + upperFirst(n.name) + ">"
                        : "remote_detail::RoScalar<" + get + ">";
}

std::string remoteCellType(const Table& t, const MibNode& c) {
    const std::string get = "&Client::" + ident(c.name);
    return c.writable() ? "remote_detail::RwCell<" + t.indexType + ", " + get + ", &Client::set" + upperFirst(c.name) + ">"
                        : "remote_detail::RoCell<" + t.indexType + ", " + get + ">";
}

std::string remoteRowType(const Table& t) { return t.entryType + "Row"; }
std::string remoteTableType(const Table& t) { return t.entryType + "RemoteTable"; }

void emitRemoteTable(W& w, const Table& t) {
    const std::string row = remoteRowType(t), ip = "const " + t.indexType + "& index";
    // row proxy
    std::string init;
    for (const MibNode* c : t.columns) init += ident(c->name) + "(c, index), ";
    if (t.rowStatus) init += ident(t.rowStatus->name) + "(c, index), ";
    init += "index_(index), c_(&c)";
    w("/// @brief One row of " + t.table->name + " on the remote agent: every cell has get() and, if writable, set().");
    w("class " + row + " {");
    w("public:");
    w("    /// @brief Binds to a row. @param[in] c The typed client. @param[in] index Row index.");
    w("    " + row + "(Client& c, " + ip + ") : " + init + " {}");
    w();
    for (const MibNode* c : t.columns) w("    " + remoteCellType(t, *c) + " " + ident(c->name) + ";  ///< " + c->name + " (" + accessText(*c) + ")");
    if (t.rowStatus)
        w("    remote_detail::RwCell<" + t.indexType + ", &Client::" + ident(t.rowStatus->name) + ", &Client::set" + upperFirst(t.rowStatus->name) +
          "> " + ident(t.rowStatus->name) + ";  ///< " + t.rowStatus->name + " (RowStatus)");
    w();
    w("    /// @brief The row index. @return The index this row is bound to.");
    w("    const " + t.indexType + "& index() const { return index_; }");
    w("    /// @brief Reads every column of the row in ONE request. @return The row.");
    w("    /// @throws snmpwrap::Error if the row does not exist, snmpwrap::TransportError, snmpwrap::ResponseError");
    w("    " + t.entryType + " read() const;");
    w();
    w("private:");
    w("    " + t.indexType + " index_;");
    w("    Client* c_;");
    w("};");
    w();
    // table proxy
    const std::string tt = remoteTableType(t);
    w("/// @brief " + t.table->name + " on the remote agent: table[index] gives a row, read() fetches the whole table.");
    w("class " + tt + " {");
    w("public:");
    w("    explicit " + tt + "(Client& c) : c_(&c) {}");
    w("    /// @brief Row access (nothing is sent until a cell is read or written). @param[in] index Row index. @return The row.");
    w("    " + row + " operator[](" + ip + ") const { return " + row + "(*c_, index); }");
    if (plainIntegerIndex(t)) {
        const std::string ct = cppType(*t.index[0]), p = ident(t.index[0]->name);
        w("    /// @brief Row access with a plain index number. @param[in] " + p + " Row index. @return The row.");
        w("    " + row + " operator[](" + ct + " " + p + ") const { return (*this)[" + t.indexType + "{" + p + "}]; }");
    }
    w("    /// @brief Reads the whole table (walk). @return All rows by index.");
    w("    std::map<" + t.indexType + ", " + t.entryType + "> read() const { return c_->" + ident(t.table->name) + "(); }");
    if (t.rowStatus) {
        w("    /// @brief Creates a row in one request (columns + createAndGo, or createAndWait).");
        w("    /// @param[in] index Row index. @param[in] values Column values. @param[in] activate True: createAndGo.");
        w("    void create(" + ip + ", const " + t.valuesType + "& values, bool activate = true) const { c_->" + t.createFn + "(index, values, activate); }");
        w("    /// @brief Destroys a row. @param[in] index Row index.");
        w("    void destroy(" + ip + ") const { c_->" + t.destroyFn + "(index); }");
    }
    w();
    w("private:");
    w("    Client* c_;");
    w("};");
    w();
}

/// C++ access path of every scalar / table inside Data, e.g. "appSensors.appLimit".
void collectPaths(const Module& mod, const Group& g, const std::string& prefix, std::map<const MibNode*, std::string>& scalars,
                  std::map<std::size_t, std::string>& tables) {
    for (const MibNode* n : g.scalars) scalars[n] = prefix + memberName(n->name);
    for (std::size_t i : g.tables) tables[i] = prefix + memberName(mod.tables[i].table->name);
    for (const Group& c : g.children) collectPaths(mod, c, prefix + c.member + ".", scalars, tables);
}

/// Members and nested group structs of Remote; `init` collects the member initializers in declaration order.
void emitRemoteGroupBody(W& w, const Module& mod, const Group& g, const std::string& pad, std::vector<std::string>& init) {
    for (const Group& c : g.children) {
        std::vector<std::string> sub;
        W inner;
        emitRemoteGroupBody(inner, mod, c, pad + "    ", sub);
        std::string list;
        for (std::size_t i = 0; i < sub.size(); ++i) list += (i ? ", " : "") + sub[i];
        w(pad + "/// @brief Group " + c.mibName + (c.description.empty() ? "" : ": " + doc(c.description)));
        w(pad + "struct " + c.type + " {");
        w(pad + "    explicit " + c.type + "(Client& c) : " + list + " {}");
        std::string body = inner.str();
        if (!body.empty() && body.back() == '\n') body.pop_back();
        w(body);
        w(pad + "} " + c.member + ";");
        init.push_back(c.member + "(c)");
    }
    for (const MibNode* n : g.scalars) {
        w(pad + remoteScalarType(*n) + " " + memberName(n->name) + ";  ///< " + n->name + " (" + accessText(*n) + "). " + doc(n->description));
        init.push_back(memberName(n->name) + "(c)");
    }
    for (std::size_t i : g.tables) {
        const Table& t = mod.tables[i];
        w(pad + remoteTableType(t) + " " + memberName(t.table->name) + ";  ///< " + t.table->name + ": " + doc(t.table->description));
        init.push_back(memberName(t.table->name) + "(c)");
    }
}

// ---------------------------------------------------------------------------------------------
// header
// ---------------------------------------------------------------------------------------------

std::string header(const MibModel& m, const Module& mod, const Options& o) {
    W w;
    w("// Generated by snmpwrap-mibgen from " + (o.sourceInfo.empty() ? mod.name : o.sourceInfo) + " (module " + mod.name + ").");
    w("// DO NOT EDIT - changes are lost when the MIB is regenerated.");
    w("#pragma once");
    w();
    w("#include <array>");
    w("#include <cstdint>");
    w("#include <functional>");
    w("#include <map>");
    w("#include <memory>");
    w("#include <mutex>");
    w("#include <optional>");
    w("#include <string>");
    w("#include <utility>");
    w("#include <vector>");
    w();
    w("#include \"snmpwrap/agent.hpp\"");
    w("#include \"snmpwrap/client.hpp\"");
    w();
    w("/**");
    w(" * @brief Typed C++ interface of the MIB module " + mod.name + ".");
    w(" *");
    w(" * Agent side: implement Instrumentation and call registerMib(agent, impl).");
    w(" * Client side: use Client on top of an snmpwrap::Client.");
    w(" * All OIDs, types, ranges, enumerations and index layouts come from the MIB.");
    w(" */");
    w("namespace " + mod.ns + " {");
    w();

    // --- OIDs
    w("/// @brief Object OIDs of " + mod.name + " (without instance suffix).");
    w("namespace oids {");
    if (mod.hasRoot) w("inline const snmpwrap::Oid root" + oidInit(mod.root) + ";  ///< registration root (common prefix of all objects)");
    for (const MibNode* n : mod.all) w("inline const snmpwrap::Oid " + ident(n->name) + oidInit(n->oid) + ";  ///< " + n->name + " (" + toString(n->kind) + ")");
    w("}  // namespace oids");
    w();

    // --- enums
    for (const auto& [name, n] : mod.enums) {
        w("/// @brief Named numbers of " + (n->textualConvention.empty() ? n->name : n->textualConvention) + ".");
        w("enum class " + name + " : std::int32_t {");
        for (const MibEnum& e : n->enums) w("    " + ident(e.label) + " = " + std::to_string(e.value) + ",");
        w("};");
        w("/// @brief MIB label of a value. @param[in] v The value. @return E.g. \"" + n->enums.front().label + "\", or \"?\".");
        w("const char* toString(" + name + " v) noexcept;");
        w();
    }

    // --- table helper structs
    for (const Table& t : mod.tables) {
        std::string idxText;
        for (std::size_t i = 0; i < t.entry->index.size(); ++i)
            idxText += (i ? ", " : "") + std::string(t.entry->index[i].implied ? "IMPLIED " : "") + t.entry->index[i].name;
        w("/// @brief Row index of " + t.table->name + " (INDEX { " + idxText + " }).");
        w("struct " + t.indexType + " {");
        for (const MibNode* n : t.index) w("    " + cppType(*n) + " " + ident(n->name) + "{};  ///< " + n->name + ": " + accessText(*n));
        w();
        w("    /// @brief Encodes the index (RFC 2578 7.7). @return The row-index part of a cell OID.");
        w("    snmpwrap::Oid toOid() const;");
        w("    /**");
        w("     * @brief Decodes a row-index OID.");
        w("     * @param[in] oid Row-index part of a cell OID.");
        w("     * @return The index, or std::nullopt if @p oid is not a well-formed index of this table.");
        w("     */");
        w("    static std::optional<" + t.indexType + "> fromOid(const snmpwrap::Oid& oid);");
        w("    /// @brief SNMP (OID) order.");
        w("    friend bool operator<(const " + t.indexType + "& a, const " + t.indexType + "& b) { return a.toOid() < b.toOid(); }");
        w("    /// @brief Equality.");
        w("    friend bool operator==(const " + t.indexType + "& a, const " + t.indexType + "& b) { return a.toOid() == b.toOid(); }");
        w("    /// @brief Inequality.");
        w("    friend bool operator!=(const " + t.indexType + "& a, const " + t.indexType + "& b) { return !(a == b); }");
        w("};");
        w();
        w("/// @brief All readable columns of one row of " + t.table->name + " (as read by Client).");
        w("struct " + t.entryType + " {");
        for (const MibNode* c : t.columns) w("    " + cppType(*c) + " " + ident(c->name) + "{};  ///< " + c->name);
        if (t.rowStatus) w("    snmpwrap::RowStatus " + ident(t.rowStatus->name) + "{};  ///< " + t.rowStatus->name);
        w("};");
        w();
        if (t.rowStatus) {
            w("/// @brief Column values for creating a row of " + t.table->name + "; unset = not supplied.");
            w("struct " + t.valuesType + " {");
            for (const MibNode* c : t.writable()) w("    std::optional<" + cppType(*c) + "> " + ident(c->name) + ";  ///< " + c->name + (c->defaultValue.empty() ? "" : " (DEFVAL " + c->defaultValue + ")"));
            w("};");
            w();
        }
    }

    // --- Instrumentation
    w("/**");
    w(" * @brief Agent side of " + mod.name + ": implement this interface to provide the data.");
    w(" *");
    w(" * Values passed to setters are already checked against the MIB (type, range, SIZE, enumeration);");
    w(" * override the validate... hooks for additional checks. Throw snmpwrap::SetError from a setter to");
    w(" * fail the request (all changes of the request are rolled back). All methods are called from the");
    w(" * thread that runs the snmpwrap::Agent loop.");
    w(" */");
    w("class Instrumentation {");
    w("public:");
    w("    virtual ~Instrumentation() = default;");
    if (!mod.scalars.empty()) {
        w();
        w("    /// @name Scalars");
        w("    /// @{");
        for (const MibNode* n : mod.scalars) {
            const std::string f = ident(n->name), F = upperFirst(n->name), pt = paramType(*n);
            w();
            w("    /// @brief " + n->name + " (" + accessText(*n) + "). " + doc(n->description));
            w("    /// @return The current value.");
            w("    virtual " + cppType(*n) + " " + f + "() = 0;");
            if (n->writable()) {
                w("    /// @brief Stores a new value of " + n->name + ". @param[in] value New value (MIB-checked).");
                w("    virtual void set" + F + "(" + pt + " value) = 0;");
                w("    /// @brief Optional extra check before " + n->name + " is written; throw snmpwrap::SetError to reject.");
                w("    /// @param[in] value Proposed value.");
                w("    virtual void validate" + F + "(" + pt + " value) { (void)value; }");
            }
        }
        w("    /// @}");
    }
    for (const Table& t : mod.tables) {
        const std::string ip = "const " + t.indexType + "& index";
        w();
        w("    /// @name Table " + t.table->name + (t.rowStatus ? " (rows created / destroyed via " + t.rowStatus->name + ")" : ""));
        w("    /// " + doc(t.table->description));
        w("    /// @{");
        w();
        w("    /// @brief All rows of " + t.table->name + " (any order). @return The row indexes.");
        w("    virtual std::vector<" + t.indexType + "> " + t.rowsFn + "() = 0;");
        w("    /**");
        w("     * @brief First row after @p after in OID order. Default: scans " + t.rowsFn + "(); override for big tables.");
        w("     * @param[in] after Any OID suffix (need not be a valid index), nullptr = first row.");
        w("     * @return The next row or std::nullopt.");
        w("     */");
        w("    virtual std::optional<" + t.indexType + "> " + t.nextFn + "(const snmpwrap::Oid* after);");
        w("    /// @brief Row existence. Default: scans " + t.rowsFn + "(); override for big tables.");
        w("    /// @param[in] index Row index. @return True if the row exists.");
        w("    virtual bool " + t.hasFn + "(" + ip + ");");
        for (const MibNode* c : t.columns) {
            const std::string f = ident(c->name), F = upperFirst(c->name), pt = paramType(*c);
            w();
            w("    /// @brief " + c->name + " (" + accessText(*c) + "). " + doc(c->description));
            w("    /// @param[in] index Row index. @return The cell value.");
            w("    virtual " + cppType(*c) + " " + f + "(" + ip + ") = 0;");
            if (c->writable()) {
                w("    /// @brief Writes " + c->name + " of an existing row. @param[in] index Row index. @param[in] value New value (MIB-checked).");
                w("    virtual void set" + F + "(" + ip + ", " + pt + " value) = 0;");
                w("    /// @brief Optional extra check of " + c->name + " (also on row creation); throw snmpwrap::SetError to reject.");
                w("    /// @param[in] index Row index. @param[in] value Proposed value.");
                w("    virtual void validate" + F + "(" + ip + ", " + pt + " value) { (void)index; (void)value; }");
            }
        }
        if (t.rowStatus) {
            w();
            w("    /// @brief Creates a row (createAndGo / createAndWait). Columns with a DEFVAL are pre-filled.");
            w("    /// @param[in] index Row index. @param[in] values Column values of the request.");
            w("    virtual void " + t.createFn + "(" + ip + ", const " + t.valuesType + "& values) = 0;");
            w("    /// @brief Deletes a row; must tolerate a row that does not exist. @param[in] index Row index.");
            w("    virtual void " + t.destroyFn + "(" + ip + ") = 0;");
            w("    /// @brief Stored " + t.rowStatus->name + " of an existing row. @param[in] index Row index. @return The status.");
            w("    virtual snmpwrap::RowStatus " + ident(t.rowStatus->name) + "(" + ip + ") = 0;");
            w("    /// @brief Stores the status (Active, NotInService, NotReady). @param[in] index Row index. @param[in] status New status.");
            w("    virtual void set" + upperFirst(t.rowStatus->name) + "(" + ip + ", snmpwrap::RowStatus status) = 0;");
            w("    /// @brief True if the row may become active. Default: always true. @param[in] index Row index. @return Completeness.");
            w("    virtual bool " + t.completeFn + "(" + ip + ") { (void)index; return true; }");
        }
        w("    /// @}");
    }
    w("};");
    w();

    // --- registration
    if (mod.hasRoot) {
        w("/**");
        w(" * @brief Registers all objects of " + mod.name + " in @p agent below oids::root.");
        w(" * @param[in] agent The agent.");
        w(" * @param[in] impl  Your implementation; must outlive the agent.");
        w(" * @return The Mib that serves the objects.");
        w(" * @throws snmpwrap::Error if the registration fails.");
        w(" */");
        w("snmpwrap::Mib& registerMib(snmpwrap::Agent& agent, Instrumentation& impl);");
        w();
    }
    w("/**");
    w(" * @brief Adds all objects of " + mod.name + " to an existing Mib (whose root must lie above them).");
    w(" * @param[in] mib  Target Mib.");
    w(" * @param[in] impl Your implementation; must outlive the Mib.");
    w(" * @throws snmpwrap::Error if an object is outside the Mib root or already defined.");
    w(" */");
    w("void bind(snmpwrap::Mib& mib, Instrumentation& impl);");
    w();

    // --- notifications
    for (const MibNode* n : mod.notifications) {
        const NotificationSig sig = notificationSig(m, mod, *n);
        w("/**");
        w(" * @brief Sends the notification " + n->name + " (sysUpTime.0 and snmpTrapOID.0 are added). " + doc(n->description));
        for (const std::string& d : sig.paramDocs) w(" * @param[in] " + d);
        w(" * @throws snmpwrap::Error if a value cannot be encoded.");
        w(" */");
        w("void send" + upperFirst(n->name) + "(" + sig.params + ");");
        w();
    }

    // --- Client
    w("/**");
    w(" * @brief Typed client access to " + mod.name + " on top of an snmpwrap::Client.");
    w(" *");
    w(" * Setters check values against the MIB before anything is sent (snmpwrap::SetError).");
    w(" * Getters throw snmpwrap::Error if the agent has no such object / instance.");
    w(" */");
    w("class Client {");
    w("public:");
    w("    /// @brief Wraps a session. @param[in] client Open session; must outlive this object.");
    w("    explicit Client(snmpwrap::Client& client) : c_(client) {}");
    w("    /// @brief The session underneath. @return The session passed to the constructor.");
    w("    snmpwrap::Client& session() { return c_; }");
    for (const MibNode* n : mod.scalars) {
        const std::string f = ident(n->name), F = upperFirst(n->name);
        w();
        w("    /// @brief GET " + n->name + ".0. @return The value. @throws snmpwrap::Error, snmpwrap::TransportError, snmpwrap::ResponseError");
        w("    " + cppType(*n) + " " + f + "();");
        if (n->writable()) {
            w("    /// @brief SET " + n->name + ".0. @param[in] value New value. @throws snmpwrap::SetError (MIB check), snmpwrap::TransportError, snmpwrap::ResponseError");
            w("    void set" + F + "(" + paramType(*n) + " value);");
        }
    }
    for (const Table& t : mod.tables) {
        const std::string ip = "const " + t.indexType + "& index";
        w();
        w("    /// @brief Reads the whole " + t.table->name + " (walk). @return All rows by index.");
        w("    std::map<" + t.indexType + ", " + t.entryType + "> " + ident(t.table->name) + "();");
        for (const MibNode* c : t.columns) {
            w("    /// @brief GET one cell of " + c->name + ". @param[in] index Row index. @return The value.");
            w("    " + cppType(*c) + " " + ident(c->name) + "(" + ip + ");");
            if (c->writable()) {
                w("    /// @brief SET one cell of " + c->name + ". @param[in] index Row index. @param[in] value New value.");
                w("    void set" + upperFirst(c->name) + "(" + ip + ", " + paramType(*c) + " value);");
            }
        }
        if (t.rowStatus) {
            w("    /// @brief GET " + t.rowStatus->name + ". @param[in] index Row index. @return The row status.");
            w("    snmpwrap::RowStatus " + ident(t.rowStatus->name) + "(" + ip + ");");
            w("    /// @brief SET " + t.rowStatus->name + " (e.g. Active / NotInService). @param[in] index Row index. @param[in] status New status.");
            w("    void set" + upperFirst(t.rowStatus->name) + "(" + ip + ", snmpwrap::RowStatus status);");
            w("    /**");
            w("     * @brief Creates a row in ONE request: the given columns plus createAndGo (or createAndWait).");
            w("     * @param[in] index    Row index.");
            w("     * @param[in] values   Column values.");
            w("     * @param[in] activate True: createAndGo, false: createAndWait.");
            w("     */");
            w("    void " + t.createFn + "(" + ip + ", const " + t.valuesType + "& values, bool activate = true);");
            w("    /// @brief Destroys a row. @param[in] index Row index.");
            w("    void " + t.destroyFn + "(" + ip + ");");
        }
    }
    w();
    w("private:");
    w("    snmpwrap::Client& c_;");
    w("};");
    w();

    // --- Data model + DataAgent
    if (mod.hasData) {
        w("/// @brief Every object of " + mod.name + " that has a value - tells the DataAgent hooks which object is meant.");
        w("enum class Object {");
        for (const MibNode* n : valueObjects(mod)) w("    " + ident(n->name) + ",  ///< " + n->name + " (" + toString(n->access) + ")");
        w("};");
        w("/// @brief MIB name of an object. @param[in] o The object. @return E.g. \"" + valueObjects(mod).front()->name + "\".");
        w("const char* toString(Object o) noexcept;");
        w();
        for (const Table& t : mod.tables) emitTableContainer(w, t);
        w("/**");
        w(" * @brief All values of " + mod.name + " as one nested C++ structure that follows the MIB tree.");
        w(" *");
        w(" * Groups are nested structs (they hold no values themselves), scalars are plain members, tables are");
        w(" * row containers: `data.group.table[index].column`. Reading and writing is ordinary C++ - no agent is");
        w(" * needed. Local assignments are NOT range-checked; call validate() to check every value against the MIB.");
        w(" * Serve it with DataAgent.");
        w(" */");
        w("struct Data {");
        emitGroupBody(w, mod, mod.data, "    ");
        w();
        w("    /// @brief Checks every value against the MIB (type, range, SIZE, named numbers).");
        w("    /// @return One text per violation, e.g. \"group.object: value must be in 0..100\"; empty if all values are valid.");
        w("    std::vector<std::string> validate() const;");
        w("};");
        w();
        w("/**");
        w(" * @brief Serves a Data structure through an snmpwrap::Agent.");
        w(" *");
        w(" * GET / GETNEXT read the structure, SET on writable objects (checked against the MIB) writes it. Table rows");
        w(" * are created and destroyed through RowStatus where the MIB defines it. Agent callbacks run in the thread of");
        w(" * Agent::poll() and hold an internal mutex; every other thread that touches the Data must hold lock():");
        w(" * @code");
        w(" * snmpwrap::Agent agent;");
        w(" * " + mod.ns + "::Data data;");
        w(" * " + mod.ns + "::DataAgent adapter(agent, data);");
        w(" * while (agent.poll()) {");
        w(" *     auto guard = adapter.lock();   // never keep it across poll()");
        w(" *     data.<group>.<object> = 42;");
        w(" * }");
        w(" * @endcode");
        w(" * @note Data and the Agent must outlive the DataAgent. Not copyable.");
        w(" */");
        w("class DataAgent {");
        w("public:");
        w("    /// @brief Called after a manager changed a value: which object (switch over Object) and, for table cells, the row");
        w("    /// index (else empty; decode it with the table's ...Index::fromOid()).");
        w("    /// May throw snmpwrap::SetError to refuse a value change or a new row (the request is rolled back); the destruction");
        w("    /// of a row cannot be refused. Runs inside the agent");
        w("    /// callback with the mutex held (do not call lock() from it), possibly before the whole request is committed.");
        w("    using SetHook = std::function<void(Object object, const snmpwrap::Oid& index)>;");
        w();
        w("    /// @brief Called before a manager reads a value (GET, GETNEXT, walk): which object and, for table cells, the row");
        w("    /// index (else empty). Update the Data here for values that are computed on demand (counters, uptime, live readings).");
        w("    /// Runs inside the agent callback with the mutex held: write the Data directly, do not call lock().");
        w("    using GetHook = std::function<void(Object object, const snmpwrap::Oid& index)>;");
        w();
        w("    /// @brief Registers all objects of " + mod.name + " in @p agent.");
        w("    /// @param[in] agent The agent. @param[in] data The values to serve; must outlive this object.");
        w("    /// @throws snmpwrap::Error if the registration fails.");
        w("    DataAgent(snmpwrap::Agent& agent, Data& data);");
        w("    /// @brief Adds all objects to an existing Mib whose root lies above them (custom handlers, tests without an agent).");
        w("    /// @param[in] mib The Mib. @param[in] data The values to serve; must outlive this object and the Mib.");
        w("    /// @throws snmpwrap::Error if an object is outside the Mib root or already defined.");
        w("    DataAgent(snmpwrap::Mib& mib, Data& data);");
        w("    ~DataAgent();");
        w("    DataAgent(const DataAgent&) = delete;");
        w("    DataAgent& operator=(const DataAgent&) = delete;");
        w();
        w("    /// @brief Locks the data against the agent callbacks. @return The held lock (RAII).");
        w("    std::unique_lock<std::mutex> lock();");
        w("    /// @brief Sets the hook for changes made by managers (see SetHook). @param[in] hook The hook, or an empty function.");
        w("    void onSet(SetHook hook);");
        w("    /// @brief Sets the hook that runs before values are read (see GetHook). @param[in] hook The hook, or an empty function.");
        w("    void onGet(GetHook hook);");
        w();
        w("private:");
        w("    struct Impl;");
        w("    std::unique_ptr<Impl> impl_;");
        w("};");
        w();

        // --- Remote
        emitRemoteHelpers(w);
        for (const Table& t : mod.tables) emitRemoteTable(w, t);
        W body;
        std::vector<std::string> init;
        emitRemoteGroupBody(body, mod, mod.data, "    ", init);
        std::string list = "flat_(session)";
        for (const std::string& i : init) list += ", " + i.substr(0, i.size() - 3) + "(flat_)";  // "(c)" -> "(flat_)"
        w("/**");
        w(" * @brief " + mod.name + " on a remote agent, nested like Data: groups, scalars and table rows.");
        w(" *");
        w(" * Every scalar and cell has get() and, if writable, set(); set() checks the value against the MIB before");
        w(" * anything is sent. Tables: remote.<table>[index] for one row, remote.<table>.read() for all rows.");
        w(" * @code");
        w(" * snmpwrap::Client session(config);");
        w(" * " + mod.ns + "::Remote remote(session);");
        w(" * auto v = remote.<group>.<object>.get();");
        w(" * remote.<group>.<table>[1].<column>.set(v);");
        w(" * @endcode");
        w(" * @note Not copyable; the session must outlive it. Like snmpwrap::Client: one per thread.");
        w(" */");
        w("class Remote {");
        w("    Client flat_;  // first: every member below refers to it");
        w();
        w("public:");
        w("    /// @brief Wraps an open session. @param[in] session The session; must outlive this object.");
        w("    explicit Remote(snmpwrap::Client& session) : " + list + " {}");
        w("    Remote(const Remote&) = delete;");
        w("    Remote& operator=(const Remote&) = delete;");
        w();
        w("    /// @brief Reads every value of " + mod.name + " from the agent in one walk (GETBULK on v2c/v3) into a Data structure.");
        w("    /// @return All scalars and table rows the agent has. @throws snmpwrap::TransportError, snmpwrap::ResponseError");
        w("    Data read();");
        w();
        const std::string b = body.str();
        w(b.substr(0, b.empty() ? 0 : b.size() - 1));
        w("};");
        w();
    }
    w("}  // namespace " + mod.ns);
    return w.str();
}

// ---------------------------------------------------------------------------------------------
// source
// ---------------------------------------------------------------------------------------------

std::string source(const MibModel& m, const Module& mod, const Options& o) {
    W w;
    const std::string base = o.baseName.empty() ? snakeCase(o.module) : o.baseName;
    w("// Generated by snmpwrap-mibgen from " + (o.sourceInfo.empty() ? mod.name : o.sourceInfo) + " (module " + mod.name + ").");
    w("// DO NOT EDIT - changes are lost when the MIB is regenerated.");
    w("#include \"" + base + ".hpp\"");
    w();
    w("#include <set>");
    w("#include <utility>");
    w();
    w("namespace " + mod.ns + " {");
    w();
    w("namespace {");
    w();
    w("[[maybe_unused]] snmpwrap::Value ipValue(const std::array<std::uint8_t, 4>& a) {");
    w("    return snmpwrap::Value::ipAddress(a[0], a[1], a[2], a[3]);");
    w("}");
    w();
    w("/// Object OID relative to the Mib root.");
    w("[[maybe_unused]] snmpwrap::Oid rel(const snmpwrap::Mib& mib, const snmpwrap::Oid& oid) {");
    w("    if (!mib.root().isPrefixOf(oid) || oid.size() <= mib.root().size())");
    w("        throw snmpwrap::Error(oid.str() + \" is not below the Mib root \" + mib.root().str());");
    w("    return mib.root().suffixOf(oid);");
    w("}");
    w();
    w("/// Value of a Client result, or an Error for exceptions / unexpected types.");
    w("[[maybe_unused]] const snmpwrap::Value& expect(const snmpwrap::VarBind& vb, snmpwrap::Type type, const char* name) {");
    w("    if (vb.value.type() != type)");
    w("        throw snmpwrap::Error(std::string(name) + \": agent returned \" + vb.value.str());");
    w("    return vb.value;");
    w("}");
    w();
    w("/// Row-index part of a cell OID below `entry`.");
    w("[[maybe_unused]] snmpwrap::Oid cellIndex(const snmpwrap::Oid& entry, const snmpwrap::Oid& cell) {");
    w("    return snmpwrap::Oid(std::vector<snmpwrap::SubId>(cell.ids().begin() + static_cast<std::ptrdiff_t>(entry.size() + 1), cell.ids().end()));");
    w("}");
    w();
    for (const Table& t : mod.tables) {
        std::string specs;
        for (std::size_t i = 0; i < t.specs.size(); ++i) specs += (i ? ", " : "") + indexSpecExpr(t.specs[i]);
        w("const std::vector<snmpwrap::IndexSpec>& spec_" + t.indexType + "() {");
        w("    static const std::vector<snmpwrap::IndexSpec> s{" + specs + "};");
        w("    return s;");
        w("}");
        w();
    }
    for (const MibNode* n : mod.checked) genCheck(w, *n);
    w("}  // namespace");
    w();

    // enums
    for (const auto& [name, n] : mod.enums) {
        w("const char* toString(" + name + " v) noexcept {");
        w("    switch (v) {");
        for (const MibEnum& e : n->enums) w("        case " + name + "::" + ident(e.label) + ": return \"" + e.label + "\";");
        w("    }");
        w("    return \"?\";");
        w("}");
        w();
    }

    // index structs
    for (const Table& t : mod.tables) {
        std::string vals;
        for (std::size_t i = 0; i < t.index.size(); ++i) vals += (i ? ", " : "") + indexToValue(*t.index[i], ident(t.index[i]->name));
        w("snmpwrap::Oid " + t.indexType + "::toOid() const { return snmpwrap::encodeIndex(spec_" + t.indexType + "(), {" + vals + "}); }");
        w();
        w("std::optional<" + t.indexType + "> " + t.indexType + "::fromOid(const snmpwrap::Oid& oid) {");
        w("    const auto v = snmpwrap::decodeIndex(spec_" + t.indexType + "(), oid);");
        w("    if (!v) return std::nullopt;");
        w("    " + t.indexType + " r;");
        for (std::size_t i = 0; i < t.index.size(); ++i) {
            const MibNode& n = *t.index[i];
            std::string conv = fromValue(n, "(*v)[" + std::to_string(i) + "]");
            if (n.type == Type::Counter32 || n.type == Type::TimeTicks) conv = "(*v)[" + std::to_string(i) + "].asUInt()";
            w("    r." + ident(n.name) + " = " + conv + ";");
        }
        w("    return r;");
        w("}");
        w();
        // default Next / Has
        w("std::optional<" + t.indexType + "> Instrumentation::" + t.nextFn + "(const snmpwrap::Oid* after) {");
        w("    std::optional<" + t.indexType + "> best;");
        w("    snmpwrap::Oid bestOid;");
        w("    for (const " + t.indexType + "& i : " + t.rowsFn + "()) {");
        w("        snmpwrap::Oid o = i.toOid();");
        w("        if (after && !(o > *after)) continue;");
        w("        if (!best || o < bestOid) {");
        w("            best = i;");
        w("            bestOid = std::move(o);");
        w("        }");
        w("    }");
        w("    return best;");
        w("}");
        w();
        w("bool Instrumentation::" + t.hasFn + "(const " + t.indexType + "& index) {");
        w("    for (const " + t.indexType + "& i : " + t.rowsFn + "())");
        w("        if (i == index) return true;");
        w("    return false;");
        w("}");
        w();
    }

    // bind / registerMib
    w("void bind(snmpwrap::Mib& mib, Instrumentation& impl) {");
    for (const MibNode* n : mod.scalars) {
        const std::string f = ident(n->name), F = upperFirst(n->name);
        w("    {  // " + n->name);
        w("        snmpwrap::ScalarDef d;");
        w("        d.type = " + typeEnum(n->type) + ";");
        w("        d.get = [&impl] { return " + toValue(*n, "impl." + f + "()") + "; };");
        if (n->writable()) {
            w("        d.set = [&impl](const snmpwrap::Value& v) { impl.set" + F + "(" + fromValue(*n, "v") + "); };");
            w("        d.validate = [&impl](const snmpwrap::Value& v) {");
            w("            " + checkFn(*n) + "(v);");
            w("            impl.validate" + F + "(" + fromValue(*n, "v") + ");");
            w("        };");
        }
        w("        mib.scalar(rel(mib, oids::" + f + "), std::move(d));");
        w("    }");
    }
    for (const Table& t : mod.tables) {
        w("    {  // " + t.table->name);
        w("        snmpwrap::TableDef t;");
        w("        t.indexes = spec_" + t.indexType + "();");
        std::string cols;
        for (const MibNode* c : t.columns)
            cols += std::string(cols.empty() ? "" : ", ") + "{" + std::to_string(c->oid.ids().back()) + ", " + typeEnum(c->type) + ", snmpwrap::Access::" +
                    (c->writable() ? "ReadWrite" : "ReadOnly") + "}";
        w("        t.columns = {" + cols + "};");
        w("        t.hasRow = [&impl](const snmpwrap::Oid& o) {");
        w("            const auto i = " + t.indexType + "::fromOid(o);");
        w("            return i && impl." + t.hasFn + "(*i);");
        w("        };");
        w("        t.nextRow = [&impl](const snmpwrap::Oid* after) -> std::optional<snmpwrap::Oid> {");
        w("            const auto r = impl." + t.nextFn + "(after);");
        w("            if (!r) return std::nullopt;");
        w("            return r->toOid();");
        w("        };");
        w("        t.get = [&impl](const snmpwrap::Oid& o, snmpwrap::SubId col) -> snmpwrap::Value {");
        w("            const " + t.indexType + " i = " + t.indexType + "::fromOid(o).value();");
        w("            switch (col) {");
        for (const MibNode* c : t.columns)
            w("                case " + std::to_string(c->oid.ids().back()) + ": return " + toValue(*c, "impl." + ident(c->name) + "(i)") + ";");
        w("                default: break;");
        w("            }");
        w("            (void)i;");
        w("            throw snmpwrap::Error(\"" + t.table->name + ": unknown column \" + std::to_string(col));");
        w("        };");
        if (t.anyWritable()) {
            w("        t.set = [&impl](const snmpwrap::Oid& o, snmpwrap::SubId col, const snmpwrap::Value& v) {");
            w("            const " + t.indexType + " i = " + t.indexType + "::fromOid(o).value();");
            w("            switch (col) {");
            for (const MibNode* c : t.writable())
                w("                case " + std::to_string(c->oid.ids().back()) + ": impl.set" + upperFirst(c->name) + "(i, " + fromValue(*c, "v") + "); return;");
            w("                default: break;");
            w("            }");
            w("            throw snmpwrap::SetError(snmpwrap::ErrorStatus::NotWritable, \"" + t.table->name + ": column not writable\");");
            w("        };");
            w("        t.validate = [&impl](const snmpwrap::Oid& o, snmpwrap::SubId col, const snmpwrap::Value& v) {");
            w("            const auto i = " + t.indexType + "::fromOid(o);");
            w("            if (!i) return;  // Mib reports malformed indexes itself");
            w("            switch (col) {");
            for (const MibNode* c : t.writable()) {
                w("                case " + std::to_string(c->oid.ids().back()) + ":");
                w("                    " + checkFn(*c) + "(v);");
                w("                    impl.validate" + upperFirst(c->name) + "(*i, " + fromValue(*c, "v") + ");");
                w("                    return;");
            }
            w("                default: return;");
            w("            }");
            w("        };");
        }
        if (t.rowStatus) {
            std::string req;
            for (const MibNode* c : t.writable())
                if (c->defaultValue.empty()) req += (req.empty() ? "" : ", ") + std::to_string(c->oid.ids().back());
            w("        snmpwrap::RowStatusSpec rs;");
            w("        rs.column = " + std::to_string(t.rowStatus->oid.ids().back()) + ";");
            w("        rs.requiredColumns = {" + req + "};  // writable columns without DEFVAL");
            w("        rs.create = [&impl](const snmpwrap::Oid& o, const std::map<snmpwrap::SubId, snmpwrap::Value>& cols) {");
            w("            const " + t.indexType + " i = " + t.indexType + "::fromOid(o).value();");
            w("            " + t.valuesType + " values;");
            for (const MibNode* c : t.writable()) {
                if (c->defaultValue.empty()) continue;
                try {
                    const Value dv = m.parseValue(*c, c->defaultValue);
                    w("            values." + ident(c->name) + " = " + literal(*c, dv) + ";  // DEFVAL { " + c->defaultValue + " }");
                } catch (const Error&) {
                    w("            // DEFVAL { " + c->defaultValue + " } of " + c->name + " cannot be expressed; left unset");
                }
            }
            w("            for (const auto& [col, v] : cols) {");
            w("                switch (col) {");
            for (const MibNode* c : t.writable())
                w("                    case " + std::to_string(c->oid.ids().back()) + ": values." + ident(c->name) + " = " + fromValue(*c, "v") + "; break;");
            w("                    default: break;");
            w("                }");
            w("            }");
            w("            impl." + t.createFn + "(i, values);");
            w("        };");
            w("        rs.destroy = [&impl](const snmpwrap::Oid& o) {");
            w("            if (const auto i = " + t.indexType + "::fromOid(o)) impl." + t.destroyFn + "(*i);");
            w("        };");
            w("        rs.setState = [&impl](const snmpwrap::Oid& o, snmpwrap::RowStatus s) {");
            w("            impl.set" + upperFirst(t.rowStatus->name) + "(" + t.indexType + "::fromOid(o).value(), s);");
            w("        };");
            w("        rs.state = [&impl](const snmpwrap::Oid& o) { return impl." + ident(t.rowStatus->name) + "(" + t.indexType + "::fromOid(o).value()); };");
            w("        rs.complete = [&impl](const snmpwrap::Oid& o) { return impl." + t.completeFn + "(" + t.indexType + "::fromOid(o).value()); };");
            w("        t.rowStatus = std::move(rs);");
        }
        w("        mib.table(rel(mib, oids::" + ident(t.table->name) + "), std::move(t));");
        w("    }");
    }
    w("    (void)mib;");
    w("    (void)impl;");
    w("}");
    w();
    if (mod.hasRoot) {
        w("snmpwrap::Mib& registerMib(snmpwrap::Agent& agent, Instrumentation& impl) {");
        w("    snmpwrap::Mib& mib = agent.addMib(oids::root);");
        w("    bind(mib, impl);");
        w("    return mib;");
        w("}");
        w();
    }

    // notifications
    for (const MibNode* n : mod.notifications) {
        const NotificationSig sig = notificationSig(m, mod, *n);
        w("void send" + upperFirst(n->name) + "(" + sig.params + ") {");
        std::string list;
        for (std::size_t i = 0; i < sig.varbinds.size(); ++i) list += (i ? ",\n                    " : "") + sig.varbinds[i];
        w("    agent.sendTrap(snmpwrap::Oid" + oidInit(n->oid) + ",");
        w("                   {" + list + "});");
        w("}");
        w();
    }

    // Client
    for (const MibNode* n : mod.scalars) {
        const std::string f = ident(n->name), F = upperFirst(n->name);
        w(cppType(*n) + " Client::" + f + "() {");
        w("    const snmpwrap::VarBind vb = c_.get(oids::" + f + " + snmpwrap::SubId{0});");
        w("    return " + fromValue(*n, "expect(vb, " + typeEnum(n->type) + ", \"" + n->name + ".0\")") + ";");
        w("}");
        w();
        if (n->writable()) {
            w("void Client::set" + F + "(" + paramType(*n) + " value) {");
            w("    const snmpwrap::Value v = " + toValue(*n, "value") + ";");
            w("    " + checkFn(*n) + "(v);");
            w("    c_.set(oids::" + f + " + snmpwrap::SubId{0}, v);");
            w("}");
            w();
        }
    }
    for (const Table& t : mod.tables) {
        const std::string ip = "const " + t.indexType + "& index";
        const std::string entry = "oids::" + ident(t.entry->name);
        w("std::map<" + t.indexType + ", " + t.entryType + "> Client::" + ident(t.table->name) + "() {");
        w("    std::map<" + t.indexType + ", " + t.entryType + "> rows;");
        w("    c_.walk(" + entry + ", [&rows](const snmpwrap::VarBind& vb) {");
        w("        if (vb.oid.size() <= " + entry + ".size() + 1) return true;");
        w("        const auto i = " + t.indexType + "::fromOid(cellIndex(" + entry + ", vb.oid));");
        w("        if (!i) return true;");
        w("        " + t.entryType + "& row = rows[*i];");
        w("        switch (vb.oid[" + entry + ".size()]) {");
        for (const MibNode* c : t.columns) {
            w("            case " + std::to_string(c->oid.ids().back()) + ":");
            w("                if (vb.value.type() == " + typeEnum(c->type) + ") row." + ident(c->name) + " = " + fromValue(*c, "vb.value") + ";");
            w("                break;");
        }
        if (t.rowStatus) {
            w("            case " + std::to_string(t.rowStatus->oid.ids().back()) + ":");
            w("                if (vb.value.type() == snmpwrap::Type::Integer) row." + ident(t.rowStatus->name) + " = static_cast<snmpwrap::RowStatus>(vb.value.asInt());");
            w("                break;");
        }
        w("            default: break;");
        w("        }");
        w("        (void)row;");
        w("        return true;");
        w("    });");
        w("    return rows;");
        w("}");
        w();
        for (const MibNode* c : t.columns) {
            const std::string f = ident(c->name);
            w(cppType(*c) + " Client::" + f + "(" + ip + ") {");
            w("    const snmpwrap::VarBind vb = c_.get(oids::" + f + " + index.toOid());");
            w("    return " + fromValue(*c, "expect(vb, " + typeEnum(c->type) + ", \"" + c->name + "\")") + ";");
            w("}");
            w();
            if (c->writable()) {
                w("void Client::set" + upperFirst(c->name) + "(" + ip + ", " + paramType(*c) + " value) {");
                w("    const snmpwrap::Value v = " + toValue(*c, "value") + ";");
                w("    " + checkFn(*c) + "(v);");
                w("    c_.set(oids::" + f + " + index.toOid(), v);");
                w("}");
                w();
            }
        }
        if (t.rowStatus) {
            const std::string rs = ident(t.rowStatus->name);
            w("snmpwrap::RowStatus Client::" + rs + "(" + ip + ") {");
            w("    const snmpwrap::VarBind vb = c_.get(oids::" + rs + " + index.toOid());");
            w("    return static_cast<snmpwrap::RowStatus>(expect(vb, snmpwrap::Type::Integer, \"" + t.rowStatus->name + "\").asInt());");
            w("}");
            w();
            w("void Client::set" + upperFirst(t.rowStatus->name) + "(" + ip + ", snmpwrap::RowStatus status) {");
            w("    c_.set(oids::" + rs + " + index.toOid(), snmpwrap::Value::integer(static_cast<std::int32_t>(status)));");
            w("}");
            w();
            w("void Client::" + t.createFn + "(" + ip + ", const " + t.valuesType + "& values, bool activate) {");
            w("    const snmpwrap::Oid idx = index.toOid();");
            w("    std::vector<snmpwrap::VarBind> sets;");
            if (t.writable().empty()) w("    (void)values;  // the table has no writable columns besides its RowStatus");
            for (const MibNode* c : t.writable()) {
                const std::string f = ident(c->name);
                w("    if (values." + f + ") {");
                w("        snmpwrap::Value v = " + toValue(*c, "*values." + f) + ";");
                w("        " + checkFn(*c) + "(v);");
                w("        sets.push_back({oids::" + f + " + idx, std::move(v)});");
                w("    }");
            }
            w("    sets.push_back({oids::" + rs + " + idx, snmpwrap::Value::integer(static_cast<std::int32_t>(activate ? snmpwrap::RowStatus::CreateAndGo : snmpwrap::RowStatus::CreateAndWait))});");
            w("    c_.set(sets);");
            w("}");
            w();
            w("void Client::" + t.destroyFn + "(" + ip + ") {");
            w("    c_.set(oids::" + rs + " + index.toOid(), snmpwrap::Value::integer(static_cast<std::int32_t>(snmpwrap::RowStatus::Destroy)));");
            w("}");
            w();
        }
    }
    // Data model
    if (mod.hasData) {
        w("const char* toString(Object o) noexcept {");
        w("    switch (o) {");
        for (const MibNode* n : valueObjects(mod)) w("        case Object::" + ident(n->name) + ": return \"" + n->name + "\";");
        w("    }");
        w("    return \"?\";");
        w("}");
        w();
        w("std::vector<std::string> Data::validate() const {");
        w("    std::vector<std::string> bad;");
        w("    auto check = [&bad](const std::string& where, const std::string& object, const auto& fn) {");
        w("        try {");
        w("            fn();");
        w("        } catch (const snmpwrap::SetError& e) {");
        w("            std::string text = e.what();  // \"<object>: <reason>\" - the object is named in `where`");
        w("            if (text.rfind(object + \": \", 0) == 0) text.erase(0, object.size() + 2);");
        w("            bad.push_back(where + \": \" + text);");
        w("        }");
        w("    };");
        emitValidate(w, mod, mod.data, "", "");
        w("    return bad;");
        w("}");
        w();
        w("struct DataAgent::Impl final : Instrumentation {");
        w("    explicit Impl(Data& data) : d(data) {}");
        w("    Data& d;");
        w("    std::mutex mu;");
        w("    SetHook hook;");
        w("    GetHook getHook;");
        w("    void changed(Object object, const snmpwrap::Oid& index = snmpwrap::Oid{}) {");
        w("        if (hook) hook(object, index);");
        w("    }");
        w("    void reading(Object object, const snmpwrap::Oid& index = snmpwrap::Oid{}) {");
        w("        if (getHook) getHook(object, index);");
        w("    }");
        w();
        emitImpl(w, mod, mod.data, "");
        w("};");
        w();
        // Remote: a whole row in one GET
        for (const Table& t : mod.tables) {
            w(t.entryType + " " + remoteRowType(t) + "::read() const {");
            w("    const snmpwrap::Oid idx = index_.toOid();");
            std::string oidsList;
            std::vector<const MibNode*> cols(t.columns.begin(), t.columns.end());
            if (t.rowStatus) cols.push_back(t.rowStatus);
            for (std::size_t i = 0; i < cols.size(); ++i) oidsList += (i ? ", " : "") + std::string("oids::") + ident(cols[i]->name) + " + idx";
            w("    const std::vector<snmpwrap::VarBind> vbs = c_->session().get(std::vector<snmpwrap::Oid>{" + oidsList + "});");
            w("    if (vbs.size() != " + std::to_string(cols.size()) + ") throw snmpwrap::Error(\"" + t.table->name + ": unexpected answer\");");
            w("    " + t.entryType + " e;");
            for (std::size_t i = 0; i < cols.size(); ++i) {
                const MibNode* c = cols[i];
                const std::string at = "vbs[" + std::to_string(i) + "]";
                if (c == t.rowStatus)
                    w("    e." + ident(c->name) + " = static_cast<snmpwrap::RowStatus>(expect(" + at + ", snmpwrap::Type::Integer, \"" + c->name + "\").asInt());");
                else
                    w("    e." + ident(c->name) + " = " + fromValue(*c, "expect(" + at + ", " + typeEnum(c->type) + ", \"" + c->name + "\")") + ";");
            }
            w("    return e;");
            w("}");
            w();
        }
        // Remote: everything into a Data
        std::map<const MibNode*, std::string> scalarPaths;
        std::map<std::size_t, std::string> tablePaths;
        collectPaths(mod, mod.data, "", scalarPaths, tablePaths);
        w("Data Remote::read() {");
        w("    Data d;");
        w("    flat_.session().walk(oids::root, [&d](const snmpwrap::VarBind& vb) {");
        w("        if (vb.value.isException()) return true;");
        for (const MibNode* n : mod.scalars) {
            w("        if (vb.oid == oids::" + ident(n->name) + " + snmpwrap::SubId{0}) {");
            w("            if (vb.value.type() == " + typeEnum(n->type) + ") d." + scalarPaths[n] + " = " + fromValue(*n, "vb.value") + ";");
            w("            return true;");
            w("        }");
        }
        for (std::size_t ti = 0; ti < mod.tables.size(); ++ti) {
            const Table& t = mod.tables[ti];
            const std::string entry = "oids::" + ident(t.entry->name);
            w("        if (" + entry + ".isPrefixOf(vb.oid) && vb.oid.size() > " + entry + ".size() + 1) {");
            w("            const auto i = " + t.indexType + "::fromOid(cellIndex(" + entry + ", vb.oid));");
            w("            if (!i) return true;");
            w("            " + t.entryType + "& row = d." + tablePaths[ti] + "[*i];");
            w("            switch (vb.oid[" + entry + ".size()]) {");
            for (const MibNode* c : t.columns) {
                w("                case " + std::to_string(c->oid.ids().back()) + ":");
                w("                    if (vb.value.type() == " + typeEnum(c->type) + ") row." + ident(c->name) + " = " + fromValue(*c, "vb.value") + ";");
                w("                    break;");
            }
            if (t.rowStatus) {
                w("                case " + std::to_string(t.rowStatus->oid.ids().back()) + ":");
                w("                    if (vb.value.type() == snmpwrap::Type::Integer) row." + ident(t.rowStatus->name) + " = static_cast<snmpwrap::RowStatus>(vb.value.asInt());");
                w("                    break;");
            }
            w("                default: break;");
            w("            }");
            w("            return true;");
            w("        }");
        }
        w("        return true;");
        w("    });");
        w("    return d;");
        w("}");
        w();
        w("DataAgent::DataAgent(snmpwrap::Agent& agent, Data& data) : impl_(std::make_unique<Impl>(data)) {");
        w("    registerMib(agent, *impl_);");
        w("}");
        w();
        w("DataAgent::DataAgent(snmpwrap::Mib& mib, Data& data) : impl_(std::make_unique<Impl>(data)) {");
        w("    bind(mib, *impl_);");
        w("}");
        w();
        w("DataAgent::~DataAgent() = default;");
        w();
        w("std::unique_lock<std::mutex> DataAgent::lock() { return std::unique_lock<std::mutex>(impl_->mu); }");
        w();
        w("void DataAgent::onSet(SetHook hook) {");
        w("    std::lock_guard<std::mutex> l(impl_->mu);");
        w("    impl_->hook = std::move(hook);");
        w("}");
        w();
        w("void DataAgent::onGet(GetHook hook) {");
        w("    std::lock_guard<std::mutex> l(impl_->mu);");
        w("    impl_->getHook = std::move(hook);");
        w("}");
        w();
    }
    w("}  // namespace " + mod.ns);
    return w.str();
}

}  // namespace

std::string snakeCase(const std::string& module) {
    std::string out;
    for (char c : module) out.push_back(std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '_');
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "mib_" + out;
    return out;
}

Output generate(const MibModel& model, const Options& options) {
    const Module mod = collect(model, options);
    return Output{header(model, mod, options), source(model, mod, options)};
}

}  // namespace snmpwrap::mibgen
