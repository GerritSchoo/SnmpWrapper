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

struct Module {
    std::string name, ns;
    Oid root;
    bool hasRoot = false;
    std::vector<const MibNode*> all, scalars, notifications;
    std::vector<Table> tables;
    std::map<std::string, const MibNode*> enums;  // enum type name -> first node using it
    std::vector<const MibNode*> checked;          // writable objects that get a generated check function
};

const MibNode& resolveName(const MibModel& m, const std::string& module, const std::string& name) {
    if (const MibNode* n = m.find(module + "::" + name)) return *n;
    return m.node(name);
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

    for (const MibNode* n : mod.scalars)
        if (n->writable()) mod.checked.push_back(n);
    for (const Table& t : mod.tables)
        for (const MibNode* c : t.columns)
            if (c->writable()) mod.checked.push_back(c);

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
    w("void " + checkFn(n) + "(const snmpwrap::Value& v) {");
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
    w("#include <map>");
    w("#include <optional>");
    w("#include <string>");
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
