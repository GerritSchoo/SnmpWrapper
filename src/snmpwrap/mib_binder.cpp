#include "snmpwrap/mib_binder.hpp"

#include <memory>
#include <set>

namespace snmpwrap {

namespace {

/// RowStatus column: the textual convention itself or a type derived from it (recognized by its enumeration).
bool isRowStatusColumn(const MibNode& c) {
    if (c.textualConvention == "RowStatus") return true;
    if (c.type != Type::Integer || c.enums.size() != 6) return false;
    static const char* const kLabels[] = {"active", "notInService", "notReady", "createAndGo", "createAndWait", "destroy"};
    for (const char* label : kLabels) {
        bool found = false;
        for (const MibEnum& e : c.enums) found = found || e.label == label;
        if (!found) return false;
    }
    return true;
}

}  // namespace

MibBinder::MibBinder(Mib& mib, MibModel model, Options options)
    : mib_(mib), model_(std::move(model)), options_(options) {}

const MibNode& MibBinder::objectBelowRoot(std::string_view name, MibNodeKind kind, const char* what) const {
    const MibNode& n = model_.node(name);
    if (n.kind != kind)
        throw Error("'" + n.name + "' is a " + toString(n.kind) + " in the MIB, not a " + what);
    if (!mib_.root().isPrefixOf(n.oid) || n.oid.size() <= mib_.root().size())
        throw Error("'" + n.name + "' (" + n.oid.str() + ") is not below the Mib root " + mib_.root().str());
    return n;
}

MibBinder& MibBinder::scalar(std::string_view name, ScalarBinding b) {
    const MibNode& n = objectBelowRoot(name, MibNodeKind::Scalar, "scalar");
    const std::string what = "scalar '" + n.name + "'";
    if (!n.readable()) throw Error(what + " is " + toString(n.access) + " in the MIB and cannot be served");
    if (!b.get) throw Error(what + ": get callback is required");
    if (n.writable() && !b.set && options_.requireSetters)
        throw Error(what + " is " + toString(n.access) + " in the MIB, but no set callback was given");
    if (!n.writable() && b.set) throw Error(what + " is read-only in the MIB, but a set callback was given");

    ScalarDef def;
    def.type = n.type;
    def.get = std::move(b.get);
    def.set = std::move(b.set);
    def.validate = [model = model_, node = n, user = std::move(b.validate)](const Value& v) {
        model.validate(node, v);
        if (user) user(v);
    };
    mib_.scalar(mib_.root().suffixOf(n.oid), std::move(def));
    bound_.insert(n.oid);
    return *this;
}

MibBinder& MibBinder::table(std::string_view name, TableBinding b) {
    const MibNode* tn = &model_.node(name);
    if (tn->kind == MibNodeKind::Entry) tn = model_.findByOid(Oid(std::vector<SubId>(tn->oid.ids().begin(), tn->oid.ids().end() - 1)));
    if (!tn) throw Error("table of '" + std::string(name) + "' not found");
    const MibNode& table = objectBelowRoot(tn->module + "::" + tn->name, MibNodeKind::Table, "table");
    const std::string what = "table '" + table.name + "'";

    TableDef def;
    def.indexes = model_.indexSpecs(table);

    // columns: everything a manager can read; the RowStatus column is handled by Mib itself
    std::map<SubId, MibNode> colNodes;
    const MibNode* rsCol = nullptr;
    bool hasWritable = false;
    for (const MibNode* c : model_.columns(table)) {
        if (!c->readable()) continue;
        if (isRowStatusColumn(*c)) {
            rsCol = c;
            continue;
        }
        const SubId id = c->oid.ids().back();
        colNodes.emplace(id, *c);
        def.columns.push_back({id, c->type, c->writable() ? Access::ReadWrite : Access::ReadOnly});
        hasWritable = hasWritable || c->writable();
    }
    if (def.columns.empty() && !rsCol) throw Error(what + " has no accessible columns");

    if (!b.get) throw Error(what + ": get callback is required");
    // read-create columns of a RowStatus table can still be set at creation time without a setter
    if (hasWritable && !b.set && options_.requireSetters && !rsCol)
        throw Error(what + " has writable columns in the MIB, but no set callback was given");
    if (!hasWritable && b.set) throw Error(what + " has no writable columns in the MIB, but a set callback was given");

    def.rows = std::move(b.rows);
    def.hasRow = std::move(b.hasRow);
    def.nextRow = std::move(b.nextRow);
    def.get = std::move(b.get);
    def.set = std::move(b.set);
    def.validate = [model = model_, cols = colNodes, user = std::move(b.validate)](const Oid& idx, SubId col, const Value& v) {
        auto it = cols.find(col);
        if (it != cols.end()) model.validate(it->second, v);
        if (user) user(idx, col, v);
    };

    if (rsCol) {
        std::string missing;
        if (!b.create) missing += " create";
        if (!b.destroy) missing += " destroy";
        if (!b.setState) missing += " setState";
        if (!b.state) missing += " state";
        if (!missing.empty())
            throw Error(what + " has the RowStatus column '" + rsCol->name + "' in the MIB; missing callbacks:" + missing);

        RowStatusSpec rs;
        rs.column = rsCol->oid.ids().back();
        // DEFVALs of the MIB for columns the request did not supply
        std::map<SubId, Value> defaults;
        std::set<SubId> undecodable;  // DEFVAL forms we cannot convert (e.g. OID values of unknown names)
        for (const auto& [id, node] : colNodes) {
            if (!node.writable() || node.defaultValue.empty()) continue;
            try {
                defaults.emplace(id, model_.parseValue(node, node.defaultValue));
            } catch (const Error&) {
                undecodable.insert(id);
            }
        }
        if (b.requiredColumns) {
            rs.requiredColumns = *b.requiredColumns;
        } else {
            for (const auto& [id, node] : colNodes)
                // without a usable default the manager has to supply the column
                if (node.writable() && (node.defaultValue.empty() || undecodable.count(id))) rs.requiredColumns.insert(id);
        }
        rs.create = [create = std::move(b.create), defaults](const Oid& idx, const std::map<SubId, Value>& cols) {
            std::map<SubId, Value> all = cols;
            for (const auto& [id, v] : defaults) all.emplace(id, v);  // emplace keeps supplied values
            create(idx, all);
        };
        rs.destroy = std::move(b.destroy);
        rs.setState = std::move(b.setState);
        rs.state = std::move(b.state);
        rs.complete = std::move(b.complete);
        def.rowStatus = std::move(rs);
    } else if (b.create || b.destroy || b.setState || b.state) {
        throw Error(what + " has no RowStatus column in the MIB; create/destroy/setState/state must not be given");
    }

    mib_.table(mib_.root().suffixOf(table.oid), std::move(def));
    bound_.insert(table.oid);
    return *this;
}

SubId MibBinder::column(std::string_view name) const {
    const MibNode& n = model_.node(name);
    if (n.kind != MibNodeKind::Column) throw Error("'" + n.name + "' is not a table column");
    return n.oid.ids().back();
}

NotificationInfo MibBinder::notification(std::string_view name) const {
    const MibNode& n = model_.node(name);
    if (n.kind != MibNodeKind::Notification) throw Error("'" + n.name + "' is not a notification");
    NotificationInfo info;
    info.oid = n.oid;
    for (const std::string& obj : n.objects) {
        const MibNode* o = model_.find(n.module + "::" + obj);
        if (!o) o = &model_.node(obj);
        info.objects.push_back(o);
    }
    return info;
}

void MibBinder::sendNotification(Agent& agent, std::string_view name, const std::vector<Value>& values,
                                 const Oid& index) const {
    const NotificationInfo info = notification(name);
    if (values.size() != info.objects.size())
        throw Error("notification '" + std::string(name) + "' has " + std::to_string(info.objects.size()) + " objects, " +
                    std::to_string(values.size()) + " values given");
    std::vector<VarBind> vars;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const MibNode& o = *info.objects[i];
        if (values[i].type() != o.type)
            throw Error("notification '" + std::string(name) + "': " + o.name + " expects " + toString(o.type) + ", got " +
                        toString(values[i].type()));
        Oid inst = o.oid;
        if (o.kind == MibNodeKind::Column) {
            if (index.empty()) throw Error("notification '" + std::string(name) + "': " + o.name + " is a column and needs a row index");
            inst.append(index);
        } else {
            inst.append(SubId{0});
        }
        vars.push_back({inst, values[i]});
    }
    agent.sendTrap(info.oid, vars);
}

std::vector<std::string> MibBinder::unbound() const {
    std::vector<std::string> out;
    for (const MibNode& n : model_.nodes()) {
        if (!mib_.root().isPrefixOf(n.oid) || n.oid.size() <= mib_.root().size()) continue;
        const bool candidate = (n.kind == MibNodeKind::Scalar && n.readable()) || n.kind == MibNodeKind::Table;
        if (candidate && !bound_.count(n.oid)) out.push_back(n.name);
    }
    return out;
}

void MibBinder::finish(bool allowUnbound) const {
    if (allowUnbound) return;
    const auto missing = unbound();
    if (missing.empty()) return;
    std::string list;
    for (const auto& m : missing) list += (list.empty() ? "" : ", ") + m;
    throw Error("MIB objects without binding below " + mib_.root().str() + ": " + list);
}

}  // namespace snmpwrap
