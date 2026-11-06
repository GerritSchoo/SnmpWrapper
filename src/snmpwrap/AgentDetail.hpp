#pragma once
// Internal: one registered subtree of the Agent and how it answers the phases of a request - separate from
// the Agent (which owns the Net-SNMP registration and the loop) so that it can be tested with hand-made requests.

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#include <memory>
#include <string>

#include "snmpwrap/Mib.hpp"

namespace snmpwrap::detail {

/**
 * One subtree registered at Net-SNMP: its root, the IHandler that answers for it, and the state the Agent needs
 * to remove it safely. A pointer to it is the `myvoid` of the Net-SNMP handler, which calls handle().
 */
class Registration {
public:
    /// @param[in] root Root of the subtree. @param[in] handler Answers for it (not null).
    Registration(Oid root, std::shared_ptr<IHandler> handler);
    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;

    /// @brief The root. @return The root passed to the constructor.
    const Oid& root() const noexcept { return root_; }
    /// @brief The handler. @return The handler passed to the constructor.
    const std::shared_ptr<IHandler>& handler() const noexcept { return handler_; }

    /// @brief The Net-SNMP registration (owned by Net-SNMP while registered). @return It, or nullptr.
    netsnmp_handler_registration* netsnmpRegistration() const noexcept { return nreg_; }
    /// @brief Remembers the Net-SNMP registration. @param[in] nreg The registration.
    void setNetsnmpRegistration(netsnmp_handler_registration* nreg) noexcept { nreg_ = nreg; }

    /// @brief Answer as non-existent from now on (Agent::removeHandler()); SETs in progress still finish.
    void markRemoved() noexcept { removed_ = true; }
    /// @brief Removed? @return True after markRemoved().
    bool removed() const noexcept { return removed_; }
    /// @brief SET requests in progress (their TxnHolders are alive). @return The number.
    int pendingSets() const noexcept { return pendingSets_; }
    /// @brief True once removed and no SET request refers to it any more: it can be unregistered and freed.
    bool canBeUnregistered() const noexcept { return removed_ && pendingSets_ == 0; }

    /// @brief Name under which the SET transaction is attached to the request info. @return The key.
    std::string txnKey() const;

    /**
     * The answer to one request phase (ari->mode: GET, GETNEXT - GETBULK arrives as GETNEXT -, or one of the SET
     * phases RESERVE1 / RESERVE2 / ACTION / COMMIT / FREE / UNDO). Values and errors are written into @p requests
     * the way Net-SNMP expects; the SET transaction lives in a TxnHolder attached to @p ari.
     * A removed registration answers as non-existent (GET: noSuchObject, GETNEXT: untouched, RESERVE1: noCreation).
     * Never throws (Net-SNMP's C frames must not see exceptions).
     * @return SNMP_ERR_NOERROR, or SNMP_ERR_GENERR for an unknown mode / an unexpected failure.
     */
    int handle(netsnmp_agent_request_info* ari, netsnmp_request_info* requests);

private:
    friend class TxnHolder;  // counts pendingSets_

    void get(netsnmp_agent_request_info* ari, netsnmp_request_info* requests);
    void getNext(netsnmp_agent_request_info* ari, netsnmp_request_info* requests);
    void set(netsnmp_agent_request_info* ari, netsnmp_request_info* requests);
    bool answerAsRemoved(netsnmp_agent_request_info* ari, netsnmp_request_info* requests);

    Oid root_;
    std::shared_ptr<IHandler> handler_;
    netsnmp_handler_registration* nreg_ = nullptr;
    int pendingSets_ = 0;
    bool removed_ = false;
};

/**
 * The SET transaction of one request for one Registration, attached to the agent request info (it lives across
 * the RESERVE1 / ACTION / COMMIT / UNDO phases) and freed together with it. While it lives, the Registration counts
 * it as pending and is not unregistered (the transaction refers to its handler).
 */
class TxnHolder {
public:
    /// @param[in] reg The registration. @param[in] txn The transaction its handler prepared.
    TxnHolder(Registration& reg, std::unique_ptr<ISetTransaction> txn);
    ~TxnHolder();
    TxnHolder(const TxnHolder&) = delete;
    TxnHolder& operator=(const TxnHolder&) = delete;

    /// @brief The transaction. @return It.
    ISetTransaction& transaction() noexcept { return *txn_; }

    /// @brief Destructor callback of the Net-SNMP data list node. @param[in] holder A TxnHolder.
    static void free(void* holder);

private:
    Registration& reg_;
    std::unique_ptr<ISetTransaction> txn_;
};

}  // namespace snmpwrap::detail
