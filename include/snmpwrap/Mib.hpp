/**
 * @file Mib.hpp
 * @brief Data model of the agent: IHandler interface, SET transactions and the generic Mib
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
#include <vector>

#include "snmpwrap/Error.hpp"
#include "snmpwrap/IHandler.hpp"
#include "snmpwrap/IMibBuilder.hpp"
#include "snmpwrap/Oid.hpp"
#include "snmpwrap/Value.hpp"

namespace snmpwrap {

/**
 * @brief Generic, MIB-independent subtree made of scalars and tables below a common root OID.
 *
 * Provides correct SNMP semantics out of the box: GET with noSuchObject / noSuchInstance,
 * GETNEXT / GETBULK in OID order, atomic multi-varbind SET with rollback, RowStatus.
 * Usually created with Agent::addMib(); can also be used stand-alone (e.g. in unit tests).
 *
 * @code
 * std::shared_ptr<Mib> mib = agent->addMib(Oid::parse("1.3.6.1.4.1.99999"));
 * mib->scalar({1, 1}, {Type::OctetString, [] { return Value::string("hello"); }});
 * // -> 1.3.6.1.4.1.99999.1.1.0 = "hello"
 * @endcode
 */
class Mib : public IHandler, public IMibBuilder {
public:
    /**
     * @brief Creates an empty subtree.
     * @param[in] root Absolute OID all objects are relative to, e.g. your enterprise subtree.
     */
    explicit Mib(Oid root) : root_(std::move(root)) {}

    /// @brief The root OID. @return The root passed to the constructor.
    const Oid& root() const noexcept override { return root_; }

    /**
     * @brief Adds a scalar object.
     * @param[in] rel Object OID relative to the root; the instance is `<root>.<rel>.0`.
     *                Example: root 1.3.6.1.4.1.99999, rel {1, 2} -> 1.3.6.1.4.1.99999.1.2.0.
     * @param[in] def Type and callbacks.
     * @return Reference to this Mib (for chaining).
     * @throws Error if the getter is missing or @p rel overlaps an existing object.
     */
    Mib& scalar(const Oid& rel, ScalarDef def) override;

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
    Mib& table(const Oid& rel, TableDef def) override;

    /**
     * @brief Adds a table directly below the root.
     * @param[in] id  Table number; cells are `<root>.<id>.1.<column>.<index>`.
     * @param[in] def Columns, index layout and callbacks.
     * @return Reference to this Mib.
     * @throws Error as table(const Oid&, TableDef).
     */
    Mib& table(SubId id, TableDef def) { return table(Oid{id}, std::move(def)); }

    /**
     * @brief Sets a function that is called once at the end of every SET request that changed this Mib.
     * @param[in] hook Called with true after the request was committed, with false after it was rolled back.
     *                 Must not throw (exceptions are ignored). Replaces a previous hook; empty = none.
     * @note Use it to act on a complete request instead of single values (the generated DataAgent does).
     */
    void onRequestEnd(std::function<void(bool committed)> hook) override { requestEnd_ = std::move(hook); }

    /**
     * @brief Sets a function that is called when the rollback of a SET request starts, before any value is put back.
     * @param[in] hook Must not throw (exceptions are ignored). Replaces a previous hook; empty = none.
     * @note The set / destroy / setState callbacks that follow restore earlier values; they are not changes made by
     *       a manager. The generated DataAgent uses this to restore its Data without calling its hooks.
     *       onRequestEnd(false) follows once the rollback is done.
     */
    void onUndo(std::function<void()> hook) override { undoStart_ = std::move(hook); }

    /// @copydoc IHandler::get
    std::optional<Value> get(const Oid& oid) override;
    /// @copydoc IHandler::getNext
    std::optional<VarBind> getNext(const Oid& after) override;
    /// @copydoc IHandler::missing
    Type missing(const Oid& oid) override;
    /// @copydoc IHandler::prepare
    std::unique_ptr<ISetTransaction> prepare(const std::vector<VarBind>& sets) override;

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
    std::function<void(bool)> requestEnd_;
    std::function<void()> undoStart_;
};

}  // namespace snmpwrap
