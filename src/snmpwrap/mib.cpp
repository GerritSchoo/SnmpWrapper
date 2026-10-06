#include "snmpwrap/mib.hpp"

#include <algorithm>

namespace snmpwrap {

namespace {

constexpr std::size_t kNoIndex = SetError::kUnknownIndex;

/// Runs a user callback during SET handling and attributes any failure to varbind `vi`.
template <class F>
void callUser(std::size_t vi, F&& f) {
    try {
        f();
    } catch (const SetError& e) {
        throw SetError(e.status(), e.what(), e.index() == kNoIndex ? vi : e.index());
    } catch (const std::exception& e) {
        throw SetError(ErrorStatus::GenErr, e.what(), vi);
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Row enumeration helper: uses TableDef::nextRow if present, otherwise scans rows() (loaded once).
// ---------------------------------------------------------------------------------------------
class Mib::RowCursor {
public:
    explicit RowCursor(const TableDef& def) : def_(def) {}

    /// First row index strictly greater than *after (or the first row if after == nullptr).
    std::optional<Oid> next(const Oid* after) {
        if (def_.nextRow) {
            auto r = def_.nextRow(after);
            // a non-increasing answer would make GETNEXT loop forever / walks never end
            if (r && after && !(*r > *after))
                throw Error("TableDef::nextRow returned " + r->str() + ", which is not greater than " + after->str());
            return r;
        }
        if (!loaded_) {
            rows_ = def_.rows();
            loaded_ = true;
        }
        const Oid* best = nullptr;
        for (const Oid& r : rows_) {
            if (after && !(r > *after)) continue;
            if (!best || r < *best) best = &r;
        }
        if (!best) return std::nullopt;
        return *best;
    }

private:
    const TableDef& def_;
    std::vector<Oid> rows_;
    bool loaded_ = false;
};

// ---------------------------------------------------------------------------------------------
// SET transaction: ordered list of operations with undo closures, plus deferred (irreversible) commits.
// ---------------------------------------------------------------------------------------------
class Mib::Txn : public SetTransaction {
public:
    struct Op {
        std::size_t varbind;  // for error attribution
        std::function<void()> apply;
        std::function<void()> undo;  // empty if nothing to roll back
    };

    void apply() override {
        for (const Op& op : ops_) {
            try {
                op.apply();
            } catch (const SetError& e) {
                throw SetError(e.status(), e.what(), e.index() == kNoIndex ? op.varbind : e.index());
            } catch (const std::exception& e) {
                throw SetError(ErrorStatus::CommitFailed, e.what(), op.varbind);
            }
            if (op.undo) undone_.push_back(op.undo);
        }
    }

    void undo() noexcept override {
        for (auto it = undone_.rbegin(); it != undone_.rend(); ++it) {
            try {
                (*it)();
            } catch (...) {
            }
        }
        undone_.clear();
    }

    void commit() noexcept override {
        for (auto& c : commits_) {
            try {
                c();
            } catch (...) {
            }
        }
        undone_.clear();
    }

    std::vector<Op> ops_;
    std::vector<std::function<void()>> commits_;

private:
    std::vector<std::function<void()>> undone_;  // undo closures of successfully applied ops
};

// ---------------------------------------------------------------------------------------------
// Definition
// ---------------------------------------------------------------------------------------------

void Mib::checkOverlap(const Oid& rel) const {
    if (rel.empty()) throw Error("empty relative OID");
    auto overlaps = [&](const Oid& other) { return other.isPrefixOf(rel) || rel.isPrefixOf(other); };
    for (const auto& s : scalars_)
        if (overlaps(s.first)) throw Error("object " + rel.str() + " overlaps " + s.first.str() + " below " + root_.str());
    for (const auto& t : tables_)
        if (overlaps(t.first)) throw Error("object " + rel.str() + " overlaps " + t.first.str() + " below " + root_.str());
}

Mib& Mib::scalar(const Oid& rel, ScalarDef def) {
    if (!def.get) throw Error("scalar " + rel.str() + ": getter is required");
    checkOverlap(rel);
    scalars_.emplace(rel, std::move(def));
    objectsDirty_ = true;
    return *this;
}

Mib& Mib::table(const Oid& rel, TableDef def) {
    const std::string name = "table " + rel.str();
    if (!def.get) throw Error(name + ": get callback is required");
    if (!def.rows && !(def.nextRow && def.hasRow)) throw Error(name + ": provide rows, or nextRow together with hasRow");
    (void)decodeIndex(def.indexes, Oid{});  // validates the index layout (throws Error)

    if (def.rowStatus) {
        const RowStatusSpec& rs = *def.rowStatus;
        if (!rs.create || !rs.destroy || !rs.setState || !rs.state)
            throw Error(name + ": rowStatus needs create, destroy, setState and state callbacks");
        def.columns.push_back({rs.column, Type::Integer, Access::ReadWrite});
    }
    if (def.columns.empty()) throw Error(name + ": no columns");
    std::sort(def.columns.begin(), def.columns.end(), [](const Column& a, const Column& b) { return a.id < b.id; });
    for (std::size_t i = 0; i < def.columns.size(); ++i) {
        if (def.columns[i].id == 0) throw Error(name + ": column id 0 is invalid");
        if (i && def.columns[i].id == def.columns[i - 1].id)
            throw Error(name + ": duplicate column id " + std::to_string(def.columns[i].id));
    }
    checkOverlap(rel);
    tables_.emplace(rel, std::move(def));
    objectsDirty_ = true;
    return *this;
}

const std::vector<Mib::Object>& Mib::objects() {
    if (objectsDirty_) {
        objects_.clear();
        for (const auto& [key, def] : scalars_) objects_.push_back({key, &def, nullptr});
        for (const auto& [key, def] : tables_) objects_.push_back({key, nullptr, &def});
        std::sort(objects_.begin(), objects_.end(), [](const Object& a, const Object& b) { return a.key < b.key; });
        objectsDirty_ = false;
    }
    return objects_;
}

const Column* Mib::findColumn(const TableDef& def, SubId id) {
    auto it = std::lower_bound(def.columns.begin(), def.columns.end(), id,
                               [](const Column& c, SubId v) { return c.id < v; });
    return (it != def.columns.end() && it->id == id) ? &*it : nullptr;
}

bool Mib::validIndex(const TableDef& def, const Oid& index) {
    if (index.empty()) return false;
    return def.indexes.empty() || decodeIndex(def.indexes, index).has_value();
}

bool Mib::rowExists(const TableDef& def, const Oid& index) {
    if (def.hasRow) return def.hasRow(index);
    const auto rows = def.rows();
    return std::find(rows.begin(), rows.end(), index) != rows.end();
}

// ---------------------------------------------------------------------------------------------
// GET / GETNEXT
// ---------------------------------------------------------------------------------------------

Value Mib::fetch(const Cell& cell) {
    Value v;
    if (cell.scalar) {
        v = cell.scalar->get();
    } else if (cell.table->rowStatus && cell.column->id == cell.table->rowStatus->column) {
        v = Value::integer(static_cast<int>(cell.table->rowStatus->state(cell.index)));
    } else {
        v = cell.table->get(cell.index, cell.column->id);
    }
    const Type declared = cell.scalar ? cell.scalar->type : cell.column->type;
    if (v.type() != declared)
        throw Error(std::string("getter returned ") + toString(v.type()) + ", declared " + toString(declared));
    return v;
}

std::optional<Mib::Cell> Mib::lookup(const Oid& oid) {
    if (!root_.isPrefixOf(oid) || oid.size() <= root_.size()) return std::nullopt;
    const Oid rel = root_.suffixOf(oid);

    // scalar: rel = <object>.0
    if (rel.size() >= 2 && rel[rel.size() - 1] == 0) {
        Oid object(std::vector<SubId>(rel.ids().begin(), rel.ids().end() - 1));
        if (auto s = scalars_.find(object); s != scalars_.end()) {
            Cell c;
            c.scalar = &s->second;
            return c;
        }
    }

    // table cell: rel = <table>.1.<column>.<index...>
    for (auto& [key, def] : tables_) {
        const std::size_t n = key.size();
        if (!key.isPrefixOf(rel) || rel.size() < n + 3 || rel[n] != 1) continue;
        const Column* col = findColumn(def, rel[n + 1]);
        if (!col) return std::nullopt;
        Oid index(std::vector<SubId>(rel.ids().begin() + static_cast<std::ptrdiff_t>(n) + 2, rel.ids().end()));
        if (!validIndex(def, index) || !rowExists(def, index)) return std::nullopt;
        Cell c;
        c.table = &def;
        c.column = col;
        c.index = std::move(index);
        return c;
    }
    return std::nullopt;
}

std::optional<Value> Mib::get(const Oid& oid) {
    auto cell = lookup(oid);
    if (!cell) return std::nullopt;
    return fetch(*cell);
}

Type Mib::missing(const Oid& oid) {
    if (!root_.isPrefixOf(oid)) return Type::NoSuchObject;
    const Oid rel = root_.suffixOf(oid);
    for (const auto& [key, def] : scalars_)
        if (key.isPrefixOf(rel)) return Type::NoSuchInstance;
    for (const auto& [key, def] : tables_) {
        const std::size_t n = key.size();
        if (key.isPrefixOf(rel) && rel.size() >= n + 2 && rel[n] == 1 && findColumn(def, rel[n + 1]))
            return Type::NoSuchInstance;
    }
    return Type::NoSuchObject;
}

std::optional<VarBind> Mib::nextInTable(const Oid& key, const TableDef& def, const Oid& rel) {
    std::size_t colPos = 0;
    std::optional<Oid> afterIndex;  // set => continue after this row inside column colPos

    if (key.isPrefixOf(rel)) {
        const Oid r = key.suffixOf(rel);  // position inside the table: [] | [1] | [1, col, index...]
        if (!r.empty() && r[0] > 1) return std::nullopt;
        if (r.size() >= 2 && r[0] == 1) {
            const SubId c = r[1];
            auto it = std::lower_bound(def.columns.begin(), def.columns.end(), c,
                                       [](const Column& col, SubId v) { return col.id < v; });
            colPos = static_cast<std::size_t>(it - def.columns.begin());
            if (colPos == def.columns.size()) return std::nullopt;
            if (it->id == c && r.size() > 2)
                afterIndex = Oid(std::vector<SubId>(r.ids().begin() + 2, r.ids().end()));
        }
    }

    RowCursor cursor(def);
    for (; colPos < def.columns.size(); ++colPos) {
        // first row after `afterIndex` that has a well-formed index
        std::optional<Oid> row;
        Oid cur;
        const Oid* after = nullptr;
        if (afterIndex) {
            cur = *afterIndex;
            after = &cur;
        }
        for (;;) {
            row = cursor.next(after);
            if (!row || validIndex(def, *row)) break;
            cur = *row;
            after = &cur;
        }
        afterIndex.reset();
        if (!row) continue;

        Cell cell;
        cell.table = &def;
        cell.column = &def.columns[colPos];
        cell.index = *row;
        return VarBind{root_ + key + SubId{1} + def.columns[colPos].id + *row, fetch(cell)};
    }
    return std::nullopt;
}

std::optional<VarBind> Mib::getNext(const Oid& after) {
    Oid rel;  // position relative to root; empty = before everything
    if (!(after < root_)) {
        if (!root_.isPrefixOf(after)) return std::nullopt;
        rel = root_.suffixOf(after);
    }

    const auto& objs = objects();
    auto it = std::partition_point(objs.begin(), objs.end(), [&](const Object& o) {
        return o.key < rel && !o.key.isPrefixOf(rel);  // objects entirely before the position
    });
    for (; it != objs.end(); ++it) {
        if (it->scalar) {
            const Oid inst = it->key + SubId{0};
            if (rel < inst) {
                Cell c;
                c.scalar = it->scalar;
                return VarBind{root_ + inst, fetch(c)};
            }
        } else if (auto hit = nextInTable(it->key, *it->table, rel)) {
            return hit;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// SET
// ---------------------------------------------------------------------------------------------

std::unique_ptr<SetTransaction> Mib::prepare(const std::vector<VarBind>& sets) {
    auto txn = std::make_unique<Txn>();

    struct Group {
        const TableDef* def = nullptr;
        Oid index;
        std::size_t first = 0;  // varbind index of the first member
        std::map<SubId, std::pair<Value, std::size_t>> cols;
        std::optional<std::pair<Value, std::size_t>> rowStatus;
    };
    std::map<std::pair<const TableDef*, Oid>, Group> groups;

    // --- pass 1: scalars are planned right away, table cells are grouped per row ---------------
    for (std::size_t i = 0; i < sets.size(); ++i) {
        const Oid& oid = sets[i].oid;
        const Value& value = sets[i].value;
        if (!root_.isPrefixOf(oid) || oid.size() <= root_.size())
            throw SetError(ErrorStatus::NoCreation, "no such object: " + oid.str(), i);
        const Oid rel = root_.suffixOf(oid);

        // scalar?
        if (rel.size() >= 2 && rel[rel.size() - 1] == 0) {
            Oid object(std::vector<SubId>(rel.ids().begin(), rel.ids().end() - 1));
            if (auto s = scalars_.find(object); s != scalars_.end()) {
                const ScalarDef& def = s->second;
                if (!def.set) throw SetError(ErrorStatus::NotWritable, "not writable: " + oid.str(), i);
                if (value.type() != def.type)
                    throw SetError(ErrorStatus::WrongType,
                                   std::string("expected ") + toString(def.type) + ", got " + toString(value.type()), i);
                if (def.validate) callUser(i, [&] { def.validate(value); });
                Value old;
                callUser(i, [&] { old = def.get(); });
                txn->ops_.push_back({i, [&def, value] { def.set(value); }, [&def, old] { def.set(old); }});
                continue;
            }
        }

        // table cell?
        bool matched = false;
        for (auto& [key, def] : tables_) {
            const std::size_t n = key.size();
            if (!key.isPrefixOf(rel) || rel.size() < n + 3 || rel[n] != 1) continue;
            const Column* col = findColumn(def, rel[n + 1]);
            if (!col) break;
            Oid index(std::vector<SubId>(rel.ids().begin() + static_cast<std::ptrdiff_t>(n) + 2, rel.ids().end()));
            Group& g = groups.try_emplace(std::pair<const TableDef*, Oid>(&def, index)).first->second;
            if (!g.def) {
                g.def = &def;
                g.index = index;
                g.first = i;
            }
            if (def.rowStatus && col->id == def.rowStatus->column)
                g.rowStatus = {value, i};
            else
                g.cols[col->id] = {value, i};
            matched = true;
            break;
        }
        if (!matched) throw SetError(ErrorStatus::NoCreation, "no such object: " + oid.str(), i);
    }

    // --- pass 2: plan every touched row ---------------------------------------------------------
    for (auto& entry : groups) {
        Group& g = entry.second;
        const TableDef& def = *g.def;
        const Oid index = g.index;
        const bool idxOk = validIndex(def, index);
        bool exists = false;
        callUser(g.first, [&] { exists = idxOk && rowExists(def, index); });

        // plain column checks (type, access, user validator)
        auto checkColumns = [&](bool creating) {
            for (auto& entryCol : g.cols) {
                const SubId colId = entryCol.first;
                const Column* col = findColumn(def, colId);
                const Value& v = entryCol.second.first;
                const std::size_t at = entryCol.second.second;
                if (col->access != Access::ReadWrite || (!creating && !def.set))
                    throw SetError(ErrorStatus::NotWritable, "column " + std::to_string(colId) + " is not writable", at);
                if (v.type() != col->type)
                    throw SetError(ErrorStatus::WrongType,
                                   std::string("expected ") + toString(col->type) + ", got " + toString(v.type()), at);
                if (def.validate) callUser(at, [&] { def.validate(index, colId, v); });
            }
        };
        // writes of plain columns into an existing row, with old values captured for rollback
        auto planColumnWrites = [&] {
            for (auto& entryCol : g.cols) {
                const SubId c = entryCol.first;
                const Value v = entryCol.second.first;
                const std::size_t at = entryCol.second.second;
                Value old;
                callUser(at, [&] { old = def.get(index, c); });
                txn->ops_.push_back({at, [&def, index, c, v] { def.set(index, c, v); },
                                     [&def, index, c, old] { def.set(index, c, old); }});
            }
        };

        if (!def.rowStatus) {
            if (!exists) throw SetError(ErrorStatus::NoCreation, "row does not exist: " + index.str(), g.first);
            checkColumns(false);
            planColumnWrites();
            continue;
        }

        const RowStatusSpec& rs = *def.rowStatus;
        std::optional<RowStatus> want;
        std::size_t rsAt = g.first;
        if (g.rowStatus) {
            rsAt = g.rowStatus->second;
            const Value& v = g.rowStatus->first;
            if (v.type() != Type::Integer)
                throw SetError(ErrorStatus::WrongType, std::string("RowStatus must be Integer, got ") + toString(v.type()), rsAt);
            const int n = v.asInt();
            if (n < 1 || n > 6 || n == static_cast<int>(RowStatus::NotReady))
                throw SetError(ErrorStatus::WrongValue, "invalid RowStatus value " + std::to_string(n), rsAt);
            want = static_cast<RowStatus>(n);
        }

        if (!exists) {
            if (!want) throw SetError(ErrorStatus::InconsistentName, "row does not exist: " + index.str(), g.first);
            if (*want == RowStatus::Destroy) continue;  // nothing to destroy
            if (*want != RowStatus::CreateAndGo && *want != RowStatus::CreateAndWait)
                throw SetError(ErrorStatus::InconsistentValue, "row does not exist: " + index.str(), rsAt);
            if (!idxOk) throw SetError(ErrorStatus::InconsistentName, "malformed row index " + index.str(), rsAt);
            checkColumns(true);
            if (*want == RowStatus::CreateAndGo)
                for (SubId req : rs.requiredColumns)
                    if (!g.cols.count(req))
                        throw SetError(ErrorStatus::InconsistentValue,
                                       "createAndGo requires column " + std::to_string(req), rsAt);

            std::map<SubId, Value> initial;
            for (auto& entryCol : g.cols) initial.emplace(entryCol.first, entryCol.second.first);
            const bool go = *want == RowStatus::CreateAndGo;
            std::set<SubId> supplied;
            for (auto& entryCol : g.cols) supplied.insert(entryCol.first);

            txn->ops_.push_back({rsAt, [&rs, index, initial] { rs.create(index, initial); }, [&rs, index] { rs.destroy(index); }});
            txn->ops_.push_back({rsAt,
                                 [&rs, index, go, supplied] {
                                     bool complete = rs.complete
                                                         ? rs.complete(index)
                                                         : std::includes(supplied.begin(), supplied.end(),
                                                                         rs.requiredColumns.begin(), rs.requiredColumns.end());
                                     if (go && !complete)
                                         throw SetError(ErrorStatus::InconsistentValue, "row is not complete");
                                     rs.setState(index, go ? RowStatus::Active : complete ? RowStatus::NotInService : RowStatus::NotReady);
                                 },
                                 {}});
            continue;
        }

        // existing row
        if (want == RowStatus::CreateAndGo || want == RowStatus::CreateAndWait)
            throw SetError(ErrorStatus::InconsistentValue, "row already exists: " + index.str(), rsAt);
        if (want == RowStatus::Destroy) {
            txn->commits_.push_back([&rs, index] { rs.destroy(index); });  // irreversible: only in COMMIT
            continue;
        }
        checkColumns(false);
        if (rs.rejectEditWhileActive && !g.cols.empty() && want != RowStatus::NotInService) {
            RowStatus current = RowStatus::NotReady;
            callUser(g.first, [&] { current = rs.state(index); });
            if (current == RowStatus::Active)
                throw SetError(ErrorStatus::InconsistentValue,
                               "row " + index.str() + " is active: set RowStatus to notInService before changing columns",
                               g.cols.begin()->second.second);
        }
        planColumnWrites();

        if (want) {  // Active / NotInService
            const RowStatus target = *want;
            auto old = std::make_shared<std::optional<RowStatus>>();
            txn->ops_.push_back({rsAt,
                                 [&rs, index, target, old] {
                                     if (rs.complete && !rs.complete(index))
                                         throw SetError(ErrorStatus::InconsistentValue, "row is not complete");
                                     *old = rs.state(index);
                                     rs.setState(index, target);
                                 },
                                 [&rs, index, old] {
                                     if (*old) rs.setState(index, **old);
                                 }});
        } else if (!g.cols.empty() && rs.complete) {  // notReady rows become notInService once complete
            auto old = std::make_shared<std::optional<RowStatus>>();
            txn->ops_.push_back({g.first,
                                 [&rs, index, old] {
                                     if (rs.state(index) == RowStatus::NotReady && rs.complete(index)) {
                                         *old = RowStatus::NotReady;
                                         rs.setState(index, RowStatus::NotInService);
                                     }
                                 },
                                 [&rs, index, old] {
                                     if (*old) rs.setState(index, **old);
                                 }});
        }
    }
    return txn;
}

}  // namespace snmpwrap
