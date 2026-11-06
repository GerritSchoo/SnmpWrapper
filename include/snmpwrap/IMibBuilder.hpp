/**
 * @file IMibBuilder.hpp
 * @brief Interface IMibBuilder (defining scalars, tables and request hooks of a subtree) and the definitions it
 *        takes: ScalarDef, TableDef, Column, RowStatusSpec - implemented by Mib; mock it in tests.
 */
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "snmpwrap/Index.hpp"
#include "snmpwrap/Oid.hpp"
#include "snmpwrap/Value.hpp"

namespace snmpwrap {

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
 * form the row index (see #indexes and Index.hpp).
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
 * @brief Defining the objects of a subtree: scalars, tables and request hooks - implemented by Mib; mock it in tests.
 *
 * The generated bind() and DataAgent only use this interface, so their tests can pass a mock.
 */
class IMibBuilder {
public:
    virtual ~IMibBuilder() = default;

    /// @brief The root OID all objects are relative to. @return The root.
    virtual const Oid& root() const noexcept = 0;
    /// @brief Adds a scalar `<root>.<rel>.0`. @return *this. @throws Error (getter missing, overlap). See Mib::scalar().
    virtual IMibBuilder& scalar(const Oid& rel, ScalarDef def) = 0;
    /// @brief Adds a table `<root>.<rel>`. @return *this. @throws Error (callbacks missing, bad columns, overlap). See Mib::table().
    virtual IMibBuilder& table(const Oid& rel, TableDef def) = 0;
    /// @brief Hook at the end of every SET request (true = committed). See Mib::onRequestEnd().
    virtual void onRequestEnd(std::function<void(bool committed)> hook) = 0;
    /// @brief Hook at the start of a rollback. See Mib::onUndo().
    virtual void onUndo(std::function<void()> hook) = 0;

protected:
    IMibBuilder() = default;
    IMibBuilder(const IMibBuilder&) = default;
    IMibBuilder& operator=(const IMibBuilder&) = default;
};

}  // namespace snmpwrap
