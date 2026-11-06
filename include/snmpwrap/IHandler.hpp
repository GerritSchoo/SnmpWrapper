/**
 * @file IHandler.hpp
 * @brief Interfaces IHandler (answers requests for one OID subtree) and ISetTransaction (one SET request) -
 *        implemented by Mib; mock them in tests.
 */
#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "snmpwrap/Error.hpp"
#include "snmpwrap/Oid.hpp"
#include "snmpwrap/Value.hpp"

namespace snmpwrap {

/**
 * @brief One SNMP SET request (all varbinds that fall into one registered subtree).
 *
 * Created by IHandler::prepare() after every varbind was validated. The agent then drives it
 * through Net-SNMP's SET phases:
 * - apply()  – perform the changes. May throw SetError (typically ErrorStatus::CommitFailed);
 *              everything done so far must then be undoable.
 * - undo()   – only after a failed apply() (or a failure in another subtree of the same request);
 *              must restore the previous state.
 * - commit() – only after a successful apply(); must not fail. Irreversible work (such as
 *              deleting rows) belongs here.
 *
 * You only need this class when implementing your own IHandler; Mib provides a complete implementation.
 */
class ISetTransaction {
public:
    virtual ~ISetTransaction() = default;

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
class IHandler {
public:
    virtual ~IHandler() = default;

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
    virtual std::unique_ptr<ISetTransaction> prepare(const std::vector<VarBind>& sets) {
        (void)sets;
        throw SetError(ErrorStatus::NotWritable, "subtree is read-only", 0);
    }
};

}  // namespace snmpwrap
