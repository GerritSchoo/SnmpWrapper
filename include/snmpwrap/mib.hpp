/**
 * @file mib.hpp
 * @brief Data model of the agent: Handler interface, SET transactions and the generic Mib
 *        (scalars, tables, RowStatus) that works for any MIB without reading MIB files.
 *
 * OID layout used by Mib (standard SMIv2 conventions), relative to the Mib root:
 * - scalar:     `<rel>.0`
 * - table cell: `<rel>.1.<column>.<row index>`  (`.1` is the conceptual "Entry" object)
 */
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "snmpwrap/error.hpp"
#include "snmpwrap/index.hpp"
#include "snmpwrap/oid.hpp"
#include "snmpwrap/value.hpp"

namespace snmpwrap {

/**
 * @brief One SNMP SET request (all varbinds that fall into one registered subtree).
 *
 * Created by Handler::prepare() after every varbind was validated. The agent then drives it
 * through Net-SNMP's SET phases:
 * - apply()  – perform the changes. May throw SetError (typically ErrorStatus::CommitFailed);
 *              everything done so far must then be undoable.
 * - undo()   – only after a failed apply() (or a failure in another subtree of the same request);
 *              must restore the previous state.
 * - commit() – only after a successful apply(); must not fail. Irreversible work (such as
 *              deleting rows) belongs here.
 *
 * You only need this class when implementing your own Handler; Mib provides a complete implementation.
 */
class SetTransaction {
public:
    virtual ~SetTransaction() = default;

    /**
     * @brief Performs the changes of the request.
     * @throws SetError to fail the request; the agent calls undo() afterwards.
     */
    virtual void apply() = 0;

    /// @brief Reverts everything apply() did. Must not throw.
    virtual void undo() noexcept = 0;

    /// @brief Finalizes the request after a successful apply(). Must not throw.
    virtual void commit() noexcept = 0;
};

/**
 * @brief Low-level interface for one registered OID subtree.
 *
 * Implement it for full control (e.g. data that lives in a database or another process), or use
 * Mib, which covers scalars and tables generically. Register it with Agent::addHandler().
 *
 * All methods are called from the thread running the Agent loop. Exceptions thrown by them are
 * caught by the agent and turned into SNMP errors (genErr unless a SetError says otherwise).
 */
class Handler {
public:
    virtual ~Handler() = default;

    /**
     * @brief GET: value of exactly one instance.
     * @param[in] oid Full instance OID requested by the manager.
     * @return The value, or std::nullopt if the instance does not exist.
     */
    virtual std::optional<Value> get(const Oid& oid) = 0;

    /**
     * @brief GETNEXT / GETBULK / walk: the next existing instance.
     * @param[in] after Any OID (it need not exist).
     * @return The first instance strictly greater than @p after, or std::nullopt if there is none
     *         in this subtree (the agent then continues with the next registered subtree).
     */
    virtual std::optional<VarBind> getNext(const Oid& after) = 0;

    /**
     * @brief Which exception a GET of a non-existing instance yields (RFC 3416 4.2.1).
     * @param[in] oid The requested OID for which get() returned std::nullopt.
     * @return Type::NoSuchObject if no object matches the OID, Type::NoSuchInstance if the object
     *         exists but this instance does not. The default returns Type::NoSuchInstance.
     */
    virtual Type missing(const Oid& oid) {
        (void)oid;
        return Type::NoSuchInstance;
    }

    /**
     * @brief SET phase 1: validates all varbinds of the request and creates the transaction.
     * @param[in] sets The varbinds of this subtree, in request order.
     * @return The transaction to apply / undo / commit.
     * @throws SetError to reject the request; set its index to the position in @p sets.
     *         The default implementation rejects everything (read-only subtree).
     */
    virtual std::unique_ptr<SetTransaction> prepare(const std::vector<VarBind>& sets) {
        (void)sets;
        throw SetError(ErrorStatus::NotWritable, "subtree is read-only", 0);
    }
};

/// @brief Access of a table column.
enum class Access {
    ReadOnly,   ///< read-only / not writable.
    ReadWrite,  ///< read-write (or read-create in tables with RowStatus).
};

/**
 * @brief Definition of a scalar object (instance OID `<root>.<rel>.0`).
 *
 * @code
 * int limit = 50;
 * mib.scalar({1, 4}, {Type::Integer,
 *     [&] { return Value::integer(limit); },                 // get
 *     [&](const Value& v) { limit = v.asInt(); },            // set      (optional)
 *     [](const Value& v) {                                   // validate (optional)
 *         if (v.asInt() < 1 || v.asInt() > 100) throw SetError(ErrorStatus::WrongValue);
 *     }});
 * @endcode
 */
struct ScalarDef {
    /// Declared type; get() must return it and SETs of other types are rejected with WrongType.
    Type type = Type::Integer;
    /// Required. Returns the current value.
    std::function<Value()> get;
    /// Optional. Stores a new value; without it the scalar is read-only (notWritable).
    /// May throw SetError (e.g. CommitFailed) – the whole request is then rolled back.
    std::function<void(const Value&)> set;
    /// Optional. Checks a new value before anything is written; throw SetError to reject.
    std::function<void(const Value&)> validate;
};

/// @brief One accessible column of a table.
struct Column {
    SubId id = 0;                      ///< Column number (the sub-id after `<table>.1`), > 0.
    Type type = Type::Integer;         ///< Declared value type.
    Access access = Access::ReadOnly;  ///< Writability.
};

/// @brief RowStatus values (RFC 2579 textual convention).
enum class RowStatus : int {
    Active = 1,         ///< Row is in use.
    NotInService = 2,   ///< Row is complete but not in use.
    NotReady = 3,       ///< Row is missing required information (never set by managers).
    CreateAndGo = 4,    ///< Manager: create the row and activate it immediately.
    CreateAndWait = 5,  ///< Manager: create the row, activate later.
    Destroy = 6,        ///< Manager: delete the row.
};

/**
 * @brief Enables row creation and deletion through a RowStatus column.
 *
 * Mib implements the RFC 2579 state machine; your callbacks only store rows. The RowStatus
 * column is added to the table automatically (Integer, read-write) and answered via state() –
 * do NOT list it in TableDef::columns.
 *
 * What Mib does for the manager's requests:
 * - createAndGo   – requires all requiredColumns in the same request (else inconsistentValue),
 *                   calls create() and then setState(Active).
 * - createAndWait – calls create() and setState(NotInService or NotReady, depending on completeness).
 * - active / notInService – allowed for existing, complete rows (else inconsistentValue).
 *                   Plain columns of an active row are rejected if rejectEditWhileActive is set.
 * - destroy       – calls destroy() in the commit phase; no-op for non-existing rows.
 * - cells of a non-existing row without createAndGo/Wait -> inconsistentName.
 * - a notReady row becomes notInService automatically once complete() returns true.
 */
struct RowStatusSpec {
    /// Column number of the RowStatus object.
    SubId column = 0;
    /// Columns that createAndGo must supply. Also the completeness rule for createAndWait if complete is empty.
    std::set<SubId> requiredColumns;

    /// Required. Creates row @p index with the column values given in the same request (may be a subset).
    std::function<void(const Oid& index, const std::map<SubId, Value>& columns)> create;
    /// Required. Removes the row. Must tolerate a non-existing row (also used to roll back a failed create).
    std::function<void(const Oid& index)> destroy;
    /// Required. Stores the status of an existing row (Active, NotInService or NotReady).
    std::function<void(const Oid& index, RowStatus status)> setState;
    /// Required. Returns the stored status of an existing row.
    std::function<RowStatus(const Oid& index)> state;
    /// Optional. True if the existing row has everything it needs to be active (default: always true).
    std::function<bool(const Oid& index)> complete;
    /// If true (RFC 2579 behaviour), columns of an `active` row can only be changed together with a
    /// RowStatus of notInService in the same request; otherwise the request fails with inconsistentValue.
    /// Default false: active rows stay editable.
    bool rejectEditWhileActive = false;
};

/**
 * @brief Definition of a conceptual table (cells `<root>.<rel>.1.<column>.<index>`).
 *
 * Index columns that are `not-accessible` in the MIB are NOT listed in #columns; their values
 * form the row index (see #indexes and index.hpp).
 *
 * Rows can be provided in two ways:
 * - small tables: #rows returns all row indexes (any order);
 * - big tables:   #nextRow + #hasRow – GETNEXT and walks never enumerate the whole table.
 *
 * @code
 * TableDef t;
 * t.indexes = {IndexSpec::integer()};
 * t.columns = {{2, Type::OctetString, Access::ReadWrite}, {3, Type::Gauge32, Access::ReadOnly}};
 * t.rows    = [&] { std::vector<Oid> r; for (auto& [i, row] : data) r.push_back(indexInt(i)); return r; };
 * t.get     = [&](const Oid& idx, SubId col) {
 *     auto& row = data.at(idx[0]);
 *     return col == 2 ? Value::string(row.name) : Value::gauge(row.speed);
 * };
 * t.set     = [&](const Oid& idx, SubId, const Value& v) { data.at(idx[0]).name = v.asString(); };
 * mib.table(2, std::move(t));
 * @endcode
 */
struct TableDef {
    /// Optional index layout. Empty: every non-empty OID is accepted as row index (no validation).
    /// Set: malformed indexes are reported as non-existing, and row creation with them fails (inconsistentName).
    std::vector<IndexSpec> indexes;
    /// Accessible columns (at least one).
    std::vector<Column> columns;

    /// All current row indexes, any order. Required unless #nextRow and #hasRow are given.
    std::function<std::vector<Oid>()> rows;
    /// Optional. True if the row exists (otherwise #rows is scanned).
    std::function<bool(const Oid& index)> hasRow;
    /// Optional. First row index strictly greater than `*after`, or the first row if `after == nullptr`,
    /// in OID order. Must be strictly increasing (checked; otherwise genErr).
    std::function<std::optional<Oid>(const Oid* after)> nextRow;

    /// Required. Value of one cell of an existing row; must return the column's declared type.
    std::function<Value(const Oid& index, SubId column)> get;
    /// Optional. Writes one cell of an existing row; without it all columns are read-only (except via RowStatus create).
    std::function<void(const Oid& index, SubId column, const Value&)> set;
    /// Optional. Checks a new cell value before anything is written (also on row creation); throw SetError.
    std::function<void(const Oid& index, SubId column, const Value&)> validate;

    /// Optional. Row creation / deletion via RowStatus.
    std::optional<RowStatusSpec> rowStatus;
};

/**
 * @brief Generic, MIB-independent subtree made of scalars and tables below a common root OID.
 *
 * Provides correct SNMP semantics out of the box: GET with noSuchObject / noSuchInstance,
 * GETNEXT / GETBULK in OID order, atomic multi-varbind SET with rollback, RowStatus.
 * Usually created with Agent::addMib(); can also be used stand-alone (e.g. in unit tests).
 *
 * @code
 * Mib& mib = agent.addMib(Oid::parse("1.3.6.1.4.1.99999"));
 * mib.scalar({1, 1}, {Type::OctetString, [] { return Value::string("hello"); }});
 * // -> 1.3.6.1.4.1.99999.1.1.0 = "hello"
 * @endcode
 */
class Mib : public Handler {
public:
    /**
     * @brief Creates an empty subtree.
     * @param[in] root Absolute OID all objects are relative to, e.g. your enterprise subtree.
     */
    explicit Mib(Oid root) : root_(std::move(root)) {}

    /// @brief The root OID. @return The root passed to the constructor.
    const Oid& root() const noexcept { return root_; }

    /**
     * @brief Adds a scalar object.
     * @param[in] rel Object OID relative to the root; the instance is `<root>.<rel>.0`.
     *                Example: root 1.3.6.1.4.1.99999, rel {1, 2} -> 1.3.6.1.4.1.99999.1.2.0.
     * @param[in] def Type and callbacks.
     * @return Reference to this Mib (for chaining).
     * @throws Error if the getter is missing or @p rel overlaps an existing object.
     */
    Mib& scalar(const Oid& rel, ScalarDef def);

    /**
     * @brief Adds a scalar object directly below the root.
     * @param[in] id  Object number; the instance is `<root>.<id>.0`.
     * @param[in] def Type and callbacks.
     * @return Reference to this Mib.
     * @throws Error as scalar(const Oid&, ScalarDef).
     */
    Mib& scalar(SubId id, ScalarDef def) { return scalar(Oid{id}, std::move(def)); }

    /**
     * @brief Adds a table.
     * @param[in] rel Table OID relative to the root; cells are `<root>.<rel>.1.<column>.<index>`.
     * @param[in] def Columns, index layout and callbacks.
     * @return Reference to this Mib.
     * @throws Error if required callbacks are missing, column ids are 0 or duplicated, the index layout
     *         is invalid, or @p rel overlaps an existing object.
     */
    Mib& table(const Oid& rel, TableDef def);

    /**
     * @brief Adds a table directly below the root.
     * @param[in] id  Table number; cells are `<root>.<id>.1.<column>.<index>`.
     * @param[in] def Columns, index layout and callbacks.
     * @return Reference to this Mib.
     * @throws Error as table(const Oid&, TableDef).
     */
    Mib& table(SubId id, TableDef def) { return table(Oid{id}, std::move(def)); }

    /// @copydoc Handler::get
    std::optional<Value> get(const Oid& oid) override;
    /// @copydoc Handler::getNext
    std::optional<VarBind> getNext(const Oid& after) override;
    /// @copydoc Handler::missing
    Type missing(const Oid& oid) override;
    /// @copydoc Handler::prepare
    std::unique_ptr<SetTransaction> prepare(const std::vector<VarBind>& sets) override;

private:
    struct Object {
        Oid key;  // relative to root_
        const ScalarDef* scalar = nullptr;
        const TableDef* table = nullptr;
    };
    struct Cell {
        const ScalarDef* scalar = nullptr;
        const TableDef* table = nullptr;
        const Column* column = nullptr;
        Oid index;
    };
    class Txn;
    class RowCursor;

    std::optional<Cell> lookup(const Oid& oid);
    const std::vector<Object>& objects();
    Value fetch(const Cell& cell);
    std::optional<VarBind> nextInTable(const Oid& key, const TableDef& def, const Oid& rel);
    static bool rowExists(const TableDef& def, const Oid& index);
    static bool validIndex(const TableDef& def, const Oid& index);
    static const Column* findColumn(const TableDef& def, SubId id);
    void checkOverlap(const Oid& rel) const;

    Oid root_;
    std::map<Oid, ScalarDef> scalars_;  // key: relative object OID (without the trailing .0)
    std::map<Oid, TableDef> tables_;    // key: relative table OID (without the entry .1)
    std::vector<Object> objects_;       // sorted by key; rebuilt lazily
    bool objectsDirty_ = true;
};

}  // namespace snmpwrap
