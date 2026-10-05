/**
 * @file mib_binder.hpp
 * @brief Run-time binding of application data to MIB objects by name: the MIB file defines the
 *        interface (OIDs, types, access, ranges, enumerations, indexes, RowStatus), your code
 *        only supplies the data.
 *
 * @code
 * MibModel model = MibModel::load({"mibs/SNMPWRAPPER-TEST-MIB.txt"});
 * Agent agent;
 * MibBinder bind(agent.addMib(model.oid("snmpWrapperTestMIB")), model);
 *
 * int limit = 50;
 * bind.scalar("swtLimit",                                   // type, read-write, range 1..100: from the MIB
 *             [&] { return Value::integer(limit); },
 *             [&](const Value& v) { limit = v.asInt(); });
 *
 * TableBinding rows;                                        // index, columns, RowStatus: from the MIB
 * rows.rows = ...; rows.get = ...; rows.create = ...; ...
 * bind.table("swtRowTable", rows);
 *
 * bind.finish();                                            // error if an object of the MIB is not bound
 * agent.run();
 * @endcode
 */
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "snmpwrap/agent.hpp"
#include "snmpwrap/mib.hpp"
#include "snmpwrap/mib_model.hpp"

namespace snmpwrap {

/**
 * @brief Data callbacks of a scalar; everything else comes from the MIB.
 */
struct ScalarBinding {
    std::function<Value()> get;                  ///< Required. Must return the MIB type of the object.
    std::function<void(const Value&)> set;       ///< Required for read-write objects, forbidden for read-only ones.
    std::function<void(const Value&)> validate;  ///< Optional extra check, runs after the MIB check (range, SIZE, enum).
};

/**
 * @brief Data callbacks of a table; index layout, columns, types and the RowStatus column come from the MIB.
 *
 * Column numbers (SubId) are the last sub-identifier of the column OID; use MibBinder::column() to get
 * them by name. Semantics of all callbacks are those of TableDef / RowStatusSpec.
 */
struct TableBinding {
    std::function<std::vector<Oid>()> rows;                        ///< All row indexes (or use nextRow + hasRow).
    std::function<bool(const Oid& index)> hasRow;                  ///< Optional (big tables).
    std::function<std::optional<Oid>(const Oid* after)> nextRow;   ///< Optional (big tables).
    std::function<Value(const Oid& index, SubId column)> get;      ///< Required.
    std::function<void(const Oid& index, SubId column, const Value&)> set;       ///< For writable columns.
    std::function<void(const Oid& index, SubId column, const Value&)> validate;  ///< Optional extra check.

    /// @name RowStatus tables only (required if the MIB table has a column of type RowStatus)
    /// @{
    /// Creates a row. Columns that the request did not supply but that have a DEFVAL in the MIB are
    /// already filled in with their default value.
    std::function<void(const Oid& index, const std::map<SubId, Value>& columns)> create;
    std::function<void(const Oid& index)> destroy;                   ///< Removes a row (tolerate missing rows).
    std::function<void(const Oid& index, RowStatus status)> setState; ///< Stores the row status.
    std::function<RowStatus(const Oid& index)> state;                ///< Returns the row status.
    std::function<bool(const Oid& index)> complete;                  ///< Optional: row ready to be active.
    /// Optional override. Default: writable columns without DEFVAL must be supplied by createAndGo.
    std::optional<std::set<SubId>> requiredColumns;
    /// @}
};

/// @brief OIDs of a NOTIFICATION-TYPE and of its OBJECTS.
struct NotificationInfo {
    Oid oid;                          ///< The notification OID (snmpTrapOID.0 value).
    std::vector<const MibNode*> objects;  ///< The OBJECTS, in MIB order.
};

/**
 * @brief Binds data to the objects of a MibModel and registers them in a Mib.
 *
 * Every bind call checks the binding against the MIB and throws Error with a clear message on a
 * mismatch: unknown name, object outside the Mib's root, wrong object kind, `set` missing for a
 * read-write object or given for a read-only one, RowStatus callbacks missing, …
 * Values written by managers are validated against the MIB SYNTAX automatically.
 */
class MibBinder {
public:
    /// @brief Options of the binder.
    struct Options {
        /// True: a read-write object without `set` is an error. False: it is served read-only.
        bool requireSetters = true;
    };

    /**
     * @brief Creates a binder.
     * @param[in] mib     Target subtree, usually `agent.addMib(model.oid("<module root>"))`.
     * @param[in] model   The loaded MIB.
     * @param[in] options Binding rules.
     */
    MibBinder(Mib& mib, MibModel model, Options options);
    /// @brief Creates a binder with default options. @param[in] mib Target subtree. @param[in] model The loaded MIB.
    MibBinder(Mib& mib, MibModel model) : MibBinder(mib, std::move(model), Options()) {}

    /**
     * @brief Binds a scalar.
     * @param[in] name    MIB name ("swtLimit" or "MODULE::swtLimit").
     * @param[in] binding Data callbacks.
     * @return Reference to this binder.
     * @throws Error on any mismatch with the MIB.
     */
    MibBinder& scalar(std::string_view name, ScalarBinding binding);

    /**
     * @brief Binds a scalar (shortcut).
     * @param[in] name MIB name.
     * @param[in] get  Getter.
     * @param[in] set  Setter (required for read-write objects).
     * @return Reference to this binder.
     * @throws Error on any mismatch with the MIB.
     */
    MibBinder& scalar(std::string_view name, std::function<Value()> get, std::function<void(const Value&)> set = {}) {
        return scalar(name, ScalarBinding{std::move(get), std::move(set), {}});
    }

    /**
     * @brief Binds a table.
     * @param[in] name    MIB name of the table (or of its entry).
     * @param[in] binding Data callbacks.
     * @return Reference to this binder.
     * @throws Error on any mismatch with the MIB.
     */
    MibBinder& table(std::string_view name, TableBinding binding);

    /**
     * @brief Column number of a column, for use in TableBinding callbacks.
     * @param[in] name MIB name of the column, e.g. "swtRowName".
     * @return The column number (last sub-identifier of the column OID).
     * @throws Error if @p name is no column.
     */
    SubId column(std::string_view name) const;

    /**
     * @brief Looks up a notification.
     * @param[in] name MIB name of the NOTIFICATION-TYPE.
     * @return Its OID and objects.
     * @throws Error if unknown or no notification.
     */
    NotificationInfo notification(std::string_view name) const;

    /**
     * @brief Sends a notification defined in the MIB.
     * @param[in] agent  The agent.
     * @param[in] name   MIB name of the NOTIFICATION-TYPE.
     * @param[in] values One value per OBJECT, in MIB order (types are checked against the MIB).
     * @param[in] index  Row index for objects that are table columns (empty if all are scalars).
     * @throws Error if the number or types of the values do not match, or a column object has no index.
     */
    void sendNotification(Agent& agent, std::string_view name, const std::vector<Value>& values,
                          const Oid& index = {}) const;

    /**
     * @brief Accessible scalars and tables below the Mib root that are not bound yet.
     * @return Their names.
     */
    std::vector<std::string> unbound() const;

    /**
     * @brief Completes the binding.
     * @param[in] allowUnbound False (default): throw if objects of the MIB are not bound.
     * @throws Error listing the unbound objects.
     */
    void finish(bool allowUnbound = false) const;

    /// @brief The model used by this binder. @return The MibModel.
    const MibModel& model() const noexcept { return model_; }

private:
    const MibNode& objectBelowRoot(std::string_view name, MibNodeKind kind, const char* what) const;

    Mib& mib_;
    MibModel model_;
    Options options_;
    std::set<Oid> bound_;
};

}  // namespace snmpwrap
