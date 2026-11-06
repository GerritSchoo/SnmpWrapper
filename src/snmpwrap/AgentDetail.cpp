#include "AgentDetail.hpp"

#include <optional>
#include <vector>

#include "Convert.hpp"

namespace snmpwrap::detail {

namespace {

Oid requestOid(const netsnmp_request_info* r) { return fromNetOid(r->requestvb->name, r->requestvb->name_length); }

/// Reports `e` on the varbind it names (index into the request list), or on the first one.
void failSet(netsnmp_agent_request_info* ari, netsnmp_request_info* requests, const SetError& e) {
    netsnmp_request_info* target = requests;
    if (e.index() != SetError::kUnknownIndex) {
        std::size_t k = 0;
        for (auto* r = requests; r; r = r->next, ++k)
            if (k == e.index()) {
                target = r;
                break;
            }
    }
    netsnmp_set_request_error(ari, target, static_cast<int>(e.status()));
}

}  // namespace

// --- TxnHolder ------------------------------------------------------------------------------------

TxnHolder::TxnHolder(Registration& reg, std::unique_ptr<ISetTransaction> txn) : reg_(reg), txn_(std::move(txn)) {
    ++reg_.pendingSets_;
}

TxnHolder::~TxnHolder() { --reg_.pendingSets_; }

void TxnHolder::free(void* holder) { delete static_cast<TxnHolder*>(holder); }

// --- Registration ---------------------------------------------------------------------------------

Registration::Registration(Oid root, std::shared_ptr<IHandler> handler) : root_(std::move(root)), handler_(std::move(handler)) {}

std::string Registration::txnKey() const { return "snmpwrap.txn." + root_.str(); }

void Registration::get(netsnmp_agent_request_info* ari, netsnmp_request_info* requests) {
    for (auto* r = requests; r; r = r->next) {
        if (r->processed) continue;
        try {
            const Oid oid = requestOid(r);
            auto v = handler_->get(oid);
            if (v)
                assign(r->requestvb, *v);
            else
                netsnmp_set_request_error(ari, r, handler_->missing(oid) == Type::NoSuchObject ? SNMP_NOSUCHOBJECT : SNMP_NOSUCHINSTANCE);
        } catch (const std::exception&) {
            netsnmp_set_request_error(ari, r, SNMP_ERR_GENERR);
        }
    }
}

void Registration::getNext(netsnmp_agent_request_info* ari, netsnmp_request_info* requests) {
    for (auto* r = requests; r; r = r->next) {
        if (r->processed) continue;
        try {
            const Oid start = requestOid(r);
            std::optional<VarBind> hit;
            if (r->inclusive) {
                if (auto v = handler_->get(start)) hit = VarBind{start, std::move(*v)};
            }
            if (!hit) hit = handler_->getNext(start);
            // Nothing (more) in this subtree: leave the request untouched so the agent core
            // continues with the next registered subtree.
            if (!hit || !root_.isPrefixOf(hit->oid)) continue;
            auto name = toNetOid(hit->oid);
            snmp_set_var_objid(r->requestvb, name.data(), name.size());
            assign(r->requestvb, hit->value);
        } catch (const std::exception&) {
            netsnmp_set_request_error(ari, r, SNMP_ERR_GENERR);
        }
    }
}

void Registration::set(netsnmp_agent_request_info* ari, netsnmp_request_info* requests) {
    const std::string key = txnKey();
    auto* holder = static_cast<TxnHolder*>(netsnmp_agent_get_list_data(ari, key.c_str()));

    switch (ari->mode) {
        case MODE_SET_RESERVE1: {
            std::vector<VarBind> sets;
            for (auto* r = requests; r; r = r->next) {
                try {
                    Value value = fromVar(r->requestvb);  // first: GCC < 13 leaks the OID if a later aggregate initializer throws
                    sets.push_back({requestOid(r), std::move(value)});
                } catch (const std::exception&) {
                    netsnmp_set_request_error(ari, r, SNMP_ERR_WRONGTYPE);  // ASN.1 type snmpwrap cannot represent
                    return;
                }
            }
            try {
                auto txn = handler_->prepare(sets);
                if (!txn) {
                    netsnmp_set_request_error(ari, requests, SNMP_ERR_GENERR);
                    return;
                }
                auto newHolder = std::make_unique<TxnHolder>(*this, std::move(txn));
                netsnmp_data_list* node = netsnmp_create_data_list(key.c_str(), newHolder.get(), TxnHolder::free);
                if (!node) {
                    netsnmp_set_request_error(ari, requests, SNMP_ERR_GENERR);  // newHolder is freed here
                    return;
                }
                newHolder.release();  // owned by the list node now; freed by TxnHolder::free together with the request
                netsnmp_agent_add_list_data(ari, node);
            } catch (const SetError& e) {
                failSet(ari, requests, e);
            } catch (const std::exception&) {
                netsnmp_set_request_error(ari, requests, SNMP_ERR_GENERR);
            }
            break;
        }
        case MODE_SET_ACTION:
            if (!holder) return;
            try {
                holder->transaction().apply();
            } catch (const SetError& e) {
                failSet(ari, requests, e);
            } catch (const std::exception&) {
                netsnmp_set_request_error(ari, requests, SNMP_ERR_COMMITFAILED);
            }
            break;
        case MODE_SET_COMMIT:
            if (holder) holder->transaction().commit();
            break;
        case MODE_SET_UNDO:
            if (holder) holder->transaction().undo();
            break;
        default:  // RESERVE2, FREE: the holder is released together with the request info
            break;
    }
}

bool Registration::answerAsRemoved(netsnmp_agent_request_info* ari, netsnmp_request_info* requests) {
    switch (ari->mode) {
        case MODE_GET:
            for (auto* r = requests; r; r = r->next)
                if (!r->processed) netsnmp_set_request_error(ari, r, SNMP_NOSUCHOBJECT);
            return true;
        case MODE_GETNEXT: return true;  // untouched: the agent core continues with the next subtree
        case MODE_SET_RESERVE1: netsnmp_set_request_error(ari, requests, SNMP_ERR_NOCREATION); return true;
        default: return false;  // later phases of a SET in progress still run
    }
}

int Registration::handle(netsnmp_agent_request_info* ari, netsnmp_request_info* requests) {
    try {
        if (removed_ && answerAsRemoved(ari, requests)) return SNMP_ERR_NOERROR;
        switch (ari->mode) {
            case MODE_GET: get(ari, requests); break;
            case MODE_GETNEXT: getNext(ari, requests); break;  // GETBULK arrives as GETNEXT
            case MODE_SET_RESERVE1:
            case MODE_SET_RESERVE2:
            case MODE_SET_ACTION:
            case MODE_SET_COMMIT:
            case MODE_SET_FREE:
            case MODE_SET_UNDO: set(ari, requests); break;
            default: return SNMP_ERR_GENERR;
        }
    } catch (...) {
        return SNMP_ERR_GENERR;
    }
    return SNMP_ERR_NOERROR;
}

}  // namespace snmpwrap::detail
